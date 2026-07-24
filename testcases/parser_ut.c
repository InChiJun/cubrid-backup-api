/*
 * parser_ut - deterministic unit tests for the observational log-phase parser
 * and mem_ring_put, driven with SYNTHETIC backup streams (no CUBRID server).
 *
 * Includes backup_core.c directly to exercise its static functions
 * (parser_observe / gate_ok / volid_valid / mem_ring_put). Links against the
 * other TUs for print_log / backup_mgr / handle_mgr.
 *
 * Covers design §12: T5 (boundary/ordering), T10 (gate), T11 (mem_ring_put
 * wrap fuzz), T12 (tiny/partial reads), plus false-positive/false-negative.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "../src/backup_core.c"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf ("  [NOK] %s\n", msg); } \
    else printf ("  [OK] %s\n", msg); } while (0)

/* ── synthetic stream builders (mirror the verified on-wire framing) ── */

static void build_gh (unsigned char* b, int version, int zip, int bkpagesize)
{
    int32_t v;
    memset (b, 0, CUB_BK_HEADER_IO_SIZE);
    v = CUB_BK_START_PAGE_ID;              memcpy (b + 0,      &v, 4);
    memcpy (b + 4, CUB_BK_MAGIC, strlen (CUB_BK_MAGIC));
    v = version;                           memcpy (b + 36,     &v, 4);
    v = bkpagesize;                        memcpy (b + 12448,  &v, 4);
    v = zip;                               memcpy (b + 12452,  &v, 4);
}

static void build_fs (unsigned char* b, int64_t nbytes, int16_t volid)
{
    int32_t tag = CUB_BK_FILE_START_PAGE_ID;
    memset (b, 0, CUB_BK_FILE_UNIT);
    memcpy (b + 0,                     &tag,    4);
    memcpy (b + CUB_BK_WIRE_NBYTES_OFF, &nbytes, 8);
    memcpy (b + CUB_BK_WIRE_VOLID_OFF,  &volid,  2);
}

static size_t build_zip (unsigned char* b, int buf_len)
{
    memcpy (b, &buf_len, 4);
    memset (b + 4, 0xAB, (size_t) buf_len);
    return 4 + (size_t) buf_len;
}

/* NONE-mode page-sized unit: bkpagesize+OVERHEAD bytes, leading 4-byte iopageid
 * tag (data page pageid>=0, or FILE_END -5). */
static size_t build_none_page (unsigned char* b, int32_t tag, int bkpagesize)
{
    size_t sz = (size_t) bkpagesize + CUB_BK_PAGE_OVERHEAD;
    memset (b, 0xCD, sz);
    memcpy (b, &tag, 4);
    return sz;
}

/* raw 4-byte tag (for anomaly injection) */
static size_t build_tag (unsigned char* b, int32_t tag)
{
    memcpy (b, &tag, 4);
    return 4;
}

/* ── parser harness ── */

static void arm (BACKUP_HANDLE* h)
{
    h->parser.global_header = malloc ((size_t) CUB_BK_HEADER_STRUCT);
    h->parser.state = PS_GATE;
    h->parser.need_bytes = (size_t) CUB_BK_HEADER_STRUCT;
    h->parser.got_bytes = 0;
    h->parser.skip_bytes = 0;
    h->parser.saw_data_volume = 0;
    h->parser.backup_page_size = 0;
    h->parser.compressed = 0;
    h->parser_on = true;
    h->log_phase = false;
    h->phase_published = false;
    h->bytes_total = 0;
}

static void disarm (BACKUP_HANDLE* h)
{
    if (h->parser.global_header) { free (h->parser.global_header); h->parser.global_header = NULL; }
    h->parser.state = PS_DISABLED;
    h->parser_on = false;
}

/* feed [buf,buf+n) to the parser in fixed-size chunks (0 == all at once) */
static void feed (BACKUP_HANDLE* h, const unsigned char* buf, size_t n, size_t chunk)
{
    size_t i;
    if (chunk == 0) { parser_observe (h, (const char *) buf, n); return; }
    for (i = 0; i < n; i += chunk)
    {
        size_t k = (n - i < chunk) ? (n - i) : chunk;
        parser_observe (h, (const char *) (buf + i), k);
    }
}

/* run one full-stream scenario at both chunkings; return log_phase result via out */
static void run_scenario (BACKUP_HANDLE* h, const unsigned char* s, size_t n,
                          const char* name, int expect_log, int expect_disabled)
{
    size_t chunks[3] = { 0, 1, 7 };   /* whole, byte-at-a-time, awkward */
    int c;
    for (c = 0; c < 3; c++)
    {
        char label[128];
        arm (h);
        feed (h, s, n, chunks[c]);
        snprintf (label, sizeof (label), "%s [chunk=%zu]: log_phase=%d", name, chunks[c], (int) h->log_phase);
        CHECK (h->log_phase == (bool) expect_log, label);
        if (expect_disabled)
        {
            snprintf (label, sizeof (label), "%s [chunk=%zu]: parser disabled", name, chunks[c]);
            CHECK (h->parser.state == PS_DISABLED, label);
        }
        disarm (h);
    }
}

/* ── mem_ring_put/pop wrap fuzz (T11) ── */
static int test_mem_ring (BACKUP_HANDLE* h)
{
    const size_t cap = 100;
    const size_t total = 50000;
    unsigned char* ref = malloc (total);
    unsigned char out[256];
    size_t prod = 0, cons = 0;
    unsigned int r = 12345;   /* deterministic LCG */
    int ok = 1;

    for (size_t i = 0; i < total; i++) ref[i] = (unsigned char) (i * 31 + 7);

    h->mem_buf = malloc (cap);
    h->mem_cap = cap; h->mem_len = 0; h->mem_head = 0;

    while (cons < total)
    {
        size_t k;
        r = r * 1103515245u + 12345u;
        /* put a pseudo-random chunk */
        if (prod < total)
        {
            size_t want = (r >> 16) % 40;
            if (want > total - prod) want = total - prod;
            k = mem_ring_put (h, (const char *) (ref + prod), want);
            if (k > want) { ok = 0; break; }
            prod += k;
        }
        r = r * 1103515245u + 12345u;
        /* pop a pseudo-random chunk and verify it matches the reference FIFO */
        {
            size_t want = (r >> 16) % (sizeof (out));
            /* exercise the runtime reader path: mem_ring_copy + explicit advance */
            size_t got = (want < h->mem_len) ? want : h->mem_len;
            mem_ring_copy (h, (char *) out, h->mem_head, got);
            h->mem_head = (h->mem_head + got) % h->mem_cap;
            h->mem_len -= got;
            if (got > 0)
            {
                if (memcmp (out, ref + cons, got) != 0) { ok = 0; break; }
                cons += got;
            }
        }
    }
    ok = ok && (prod == total) && (cons == total);
    free (ref); free (h->mem_buf); h->mem_buf = NULL;
    return ok;
}

int main (void)
{
    BACKUP_HANDLE* h;
    unsigned char* s = malloc (256 * 1024);   /* scratch stream buffer */
    size_t n;

    if (start_handle_manager () != SUCCESS) { printf ("[NOK] start_handle_manager\n"); return 1; }
    h = &handle_mgr->backup_handle;

    printf ("== T5: boundary detection & ordering ==\n");

    /* S1: gh -> volinfo(-5) -> data(0) -> zip -> archive(-20) : flip at archive */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 4096, -5);  n += CUB_BK_FILE_UNIT;   /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;   /* data volume */
    n += build_zip (s + n, 500);                          /* compressed page */
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;   /* archive = boundary */
    run_scenario (h, s, n, "S1 full->archive", 1, 0);

    /* S6: pre-data negatives only + data, NO trailing neg -> must NOT flip */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 32, -6);   n += CUB_BK_FILE_UNIT;    /* tde keys, pre-data */
    build_fs (s + n, 4096, -5); n += CUB_BK_FILE_UNIT;    /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;    /* data volume */
    n += build_zip (s + n, 300);
    run_scenario (h, s, n, "S6 pre-data-neg only", 0, 0);

    /* S7: boundary is log-info(-4) with no archive present */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;    /* data */
    build_fs (s + n, 4096, -4); n += CUB_BK_FILE_UNIT;    /* log info = boundary */
    run_scenario (h, s, n, "S7 boundary=info(-4)", 1, 0);

    printf ("== T10: gate / fail-safe ==\n");

    /* S3: ZLIB -> gate reject */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, 2 /*ZLIB*/, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;
    run_scenario (h, s, n, "S3 gate ZLIB->reject", 0, 1);

    /* S4: version 1 -> gate reject */
    n = 0;
    build_gh (s + n, 1, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;
    run_scenario (h, s, n, "S4 gate v1->reject", 0, 1);

    printf ("== self-check (false-positive guards) ==\n");

    /* S5: implausible buf_len (> bkpagesize+overhead) -> disable, no flip */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;
    n += build_tag (s + n, 16384 + 12 + 1);              /* buf_len just over bound */
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;
    run_scenario (h, s, n, "S5 buf_len overflow->disable", 0, 1);

    /* S8: unexpected raw negative tag mid-stream (-3 END-like) -> disable */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;
    n += build_tag (s + n, -3);                          /* END as a raw tag */
    run_scenario (h, s, n, "S8 unexpected neg tag->disable", 0, 1);

    /* S9: bad volid in FILE_START (< -22) -> disable, no flip */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);  n += CUB_BK_FILE_UNIT;
    build_fs (s + n, 4096, -100); n += CUB_BK_FILE_UNIT; /* impossible volid */
    run_scenario (h, s, n, "S9 bad volid->disable", 0, 1);

    printf ("== T13: NONE (uncompressed) fixed-stride walk ==\n");

    /* N1: NONE gh -> volinfo(-5) -> data(0) -> data pages -> FILE_END(-5) -> archive(-20).
     * Proves: NONE gate accepted, data pages + FILE_END(-5) strided (NOT disabled —
     * the key difference from LZ4), boundary latched at first neg-volid after data. */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_NONE, 4096); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 4096, -5);  n += CUB_BK_FILE_UNIT;          /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;          /* data volume */
    n += build_none_page (s + n, 0, 4096);                       /* data page pageid 0 */
    n += build_none_page (s + n, 1, 4096);                       /* data page pageid 1 */
    n += build_none_page (s + n, CUB_BK_FILE_END_PAGE_ID, 4096); /* FILE_END(-5): must stride */
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;          /* archive = boundary */
    run_scenario (h, s, n, "N1 NONE full->archive", 1, 0);

    /* N2: NONE pre-data negatives + data + FILE_END, NO trailing neg-volid -> must NOT flip */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_NONE, 4096); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 32, -6);    n += CUB_BK_FILE_UNIT;          /* tde, pre-data */
    build_fs (s + n, 4096, -5);  n += CUB_BK_FILE_UNIT;          /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;          /* data volume */
    n += build_none_page (s + n, 0, 4096);
    n += build_none_page (s + n, CUB_BK_FILE_END_PAGE_ID, 4096);
    run_scenario (h, s, n, "N2 NONE pre-data-neg only", 0, 0);

    /* N3: NONE boundary = log-info(-4) with no archive present */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_NONE, 4096); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;          /* data */
    n += build_none_page (s + n, 0, 4096);
    build_fs (s + n, 4096, -4);  n += CUB_BK_FILE_UNIT;          /* log info = boundary */
    run_scenario (h, s, n, "N3 NONE boundary=info(-4)", 1, 0);

    printf ("== T14: multiple data volumes before the boundary ==\n");

    /* S10: gh -> volinfo(-5) -> data(0) -> data(1) -> data(2) -> zip -> archive(-20).
     * Several positive volids in a row: boundary must latch at the archive (after
     * the LAST data volume), exercising the saw_data_volume re-assert + FS-unit
     * skip path that the single-data-volume scenarios never hit. */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 4096, -5);  n += CUB_BK_FILE_UNIT;   /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;   /* data volume 0 */
    build_fs (s + n, 8192, 1);   n += CUB_BK_FILE_UNIT;   /* data volume 1 */
    build_fs (s + n, 8192, 2);   n += CUB_BK_FILE_UNIT;   /* data volume 2 */
    n += build_zip (s + n, 500);
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;   /* archive = boundary */
    run_scenario (h, s, n, "S10 multi-data-vol->archive", 1, 0);

    /* S11: positive volids interleaved with compressed pages, non-contiguous ids,
     * then boundary. Must NOT arm early on any of the positive volids. */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_LZ4, 16384); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;
    n += build_zip (s + n, 300);
    build_fs (s + n, 8192, 1);   n += CUB_BK_FILE_UNIT;
    n += build_zip (s + n, 400);
    build_fs (s + n, 8192, 5);   n += CUB_BK_FILE_UNIT;   /* non-contiguous volid */
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;   /* boundary after last data vol */
    run_scenario (h, s, n, "S11 interleaved multi-vol->archive", 1, 0);

    /* N4: NONE mode, multiple data volumes + data pages, then archive boundary. */
    n = 0;
    build_gh (s + n, CUB_BK_HDR_VERSION, CUB_BK_ZIP_NONE, 4096); n += CUB_BK_HEADER_IO_SIZE;
    build_fs (s + n, 4096, -5);  n += CUB_BK_FILE_UNIT;   /* volinfo, pre-data */
    build_fs (s + n, 8192, 0);   n += CUB_BK_FILE_UNIT;   /* data volume 0 */
    build_fs (s + n, 8192, 1);   n += CUB_BK_FILE_UNIT;   /* data volume 1 */
    n += build_none_page (s + n, 0, 4096);
    n += build_none_page (s + n, 1, 4096);
    build_fs (s + n, 4096, -20); n += CUB_BK_FILE_UNIT;   /* archive = boundary */
    run_scenario (h, s, n, "N4 NONE multi-data-vol->archive", 1, 0);

    printf ("== T11: mem_ring_put/pop wrap fuzz ==\n");
    CHECK (test_mem_ring (h), "mem_ring wrap: 50000 bytes byte-exact FIFO across wraps");

    printf ("\n==== parser_ut: %d checks, %d failures ====\n", g_checks, g_fail);
    free (s);
    return g_fail ? 1 : 0;
}
