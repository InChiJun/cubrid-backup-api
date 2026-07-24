/*
 * bkwalk - offline, independent walker for a CUBRID backup file.
 * Re-derives the log-phase boundary WITHOUT the runtime parser and, for NONE
 * (uncompressed) backups, extracts each post-boundary section's payload so it
 * can be compared byte-for-byte against the real server log files.
 *
 * Output (stdout):
 *   HDR zip_method=<n> bkpagesize=<n>
 *   FS off=<file offset of FILE_START tag> volid=<v> nbytes=<n> label=<basename>
 *   BOUNDARY off=<offset of first neg-volid FS after a data volume>
 *   VIOLATION data-volume-after-boundary off=... volid=...   (C2 failure)
 *   END walked=<bytes>
 * With -x <outdir>: writes each post-boundary section payload (truncated to
 * nbytes) to <outdir>/<label>.payload  (NONE mode only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include "cubrid_backup_format.h"

static FILE *in_fp;
static long long walked = 0;

static int rd (void *buf, long long n)      /* returns 0 on EOF */
{
    size_t got = fread (buf, 1, (size_t) n, in_fp);
    walked += (long long) got;
    return got == (size_t) n;
}
static int skip (long long n)
{
    if (fseek (in_fp, (long) n, SEEK_CUR) != 0) return 0;
    walked += n;
    return 1;
}

int main (int argc, char *argv[])
{
    char *outdir = NULL;
    unsigned char gh[CUB_BK_HEADER_STRUCT];
    CUB_BKUP_HEADER *hdr;
    int zip, pagesz;
    long long boundary = -1;
    int saw_data = 0, violations = 0;

    if (argc >= 4 && strcmp (argv[2], "-x") == 0) outdir = argv[3];
    if (argc < 2) { fprintf (stderr, "usage: bkwalk <backup_file> [-x outdir]\n"); return 2; }
    in_fp = fopen (argv[1], "rb");
    if (!in_fp) { perror ("open"); return 2; }

    if (!rd (gh, CUB_BK_HEADER_STRUCT)) return 2;
    hdr = (CUB_BKUP_HEADER *) gh;
    if (hdr->iopageid != CUB_BK_START_PAGE_ID
        || memcmp (hdr->magic, CUB_BK_MAGIC, sizeof (CUB_BK_MAGIC) - 1) != 0)
    { fprintf (stderr, "not a v2 backup header\n"); return 2; }
    zip = hdr->zip_method; pagesz = hdr->bkpagesize;
    printf ("HDR zip_method=%d bkpagesize=%d\n", zip, pagesz);
    if (!skip (CUB_BK_HEADER_IO_SIZE - CUB_BK_HEADER_STRUCT)) return 2;

    FILE *sec_fp = NULL;              /* current section extract target */
    long long sec_left = 0;           /* payload bytes still wanted (nbytes) */

    for (;;)
    {
        int32_t tag;
        long long tag_off = walked;
        if (!rd (&tag, 4)) break;                       /* clean EOF */

        if (tag == CUB_BK_FILE_START_PAGE_ID)           /* -4 */
        {
            unsigned char fs[CUB_BK_FILE_UNIT];
            int64_t nbytes; int16_t volid;
            memcpy (fs, &tag, 4);
            if (!rd (fs + 4, CUB_BK_FILE_UNIT - 4)) { fprintf (stderr, "trunc FS\n"); return 1; }
            memcpy (&nbytes, fs + CUB_BK_WIRE_NBYTES_OFF, 8);
            memcpy (&volid,  fs + CUB_BK_WIRE_VOLID_OFF,  2);
            fs[CUB_BK_FILE_UNIT - 1] = '\0';            /* vlabel NUL safety */
            char *label = basename ((char *) fs + 24);  /* vlabel at unit offset 8+16 */
            printf ("FS off=%lld volid=%d nbytes=%lld label=%s\n",
                    tag_off, (int) volid, (long long) nbytes, label);

            if (volid >= 0) { saw_data = 1;
                if (boundary >= 0) { printf ("VIOLATION data-volume-after-boundary off=%lld volid=%d\n", tag_off, (int) volid); violations++; } }
            else if (saw_data && boundary < 0) { boundary = tag_off; printf ("BOUNDARY off=%lld\n", boundary); }

            if (sec_fp) { fclose (sec_fp); sec_fp = NULL; }
            if (outdir && boundary >= 0 && zip == CUB_BK_ZIP_NONE)
            {
                char path[4300];
                snprintf (path, sizeof (path), "%s/%s.payload", outdir, label);
                sec_fp = fopen (path, "wb");
                sec_left = nbytes;
            }
            continue;
        }

        if (zip == CUB_BK_ZIP_LZ4)                      /* structure walk only */
        {
            if (tag > 0 && tag <= pagesz + CUB_BK_PAGE_OVERHEAD) { if (!skip (tag)) break; continue; }
            fprintf (stderr, "unexpected LZ4 tag %d at %lld\n", tag, tag_off); return 1;
        }
        /* NONE: fixed-stride unit = prefix(8) + iopage(pagesz) + trailer(4).
         * tag(4) already consumed; prefix remainder = 4, then payload, then 4. */
        if (tag >= 0 || tag == CUB_BK_FILE_END_PAGE_ID)
        {
            if (!skip (4)) break;
            if (sec_fp && tag >= 0 && sec_left > 0)
            {
                static unsigned char *page = NULL;
                long long take = (sec_left < pagesz) ? sec_left : pagesz;
                if (!page) page = malloc ((size_t) pagesz);
                if (!rd (page, pagesz)) break;
                fwrite (page, 1, (size_t) take, sec_fp);
                sec_left -= take;
            }
            else if (!skip (pagesz)) break;
            if (!skip (4)) break;                       /* unit trailer (overhead 12 = 8 prefix + 4 trailer) */
            continue;
        }
        /* other negative sentinels (-2/-3/-6): stop structured walk gracefully */
        printf ("STOP tag=%d off=%lld\n", tag, tag_off);
        break;
    }
    if (sec_fp) fclose (sec_fp);
    printf ("END walked=%lld violations=%d\n", walked, violations);
    return violations ? 1 : 0;
}
