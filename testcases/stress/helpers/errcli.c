/*
 * errcli - error-path client for the backup API.
 * Phase 1: begin -> read only N chunks -> end MID-STREAM (early end / user abort).
 *          Expect: end returns without hanging; process stays healthy.
 * Phase 2: in the SAME process, run a full begin/read-all/end backup.
 *          Expect: success -> proves the handle is reusable after an aborted run.
 * exit 0 = both phases behaved; nonzero = which phase failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cubrid_backup_api.h"

static void set_info (CUBRID_BACKUP_INFO *i, char *db)
{
    memset (i, 0, sizeof (*i));
    i->backup_level   = 0;
    i->remove_archive = -1;
    i->sa_mode        = -1;
    i->no_check       = -1;
    i->compress       = -1;
    i->db_name        = db;
}

int main (int argc, char *argv[])
{
    CUBRID_BACKUP_INFO info;
    void *h = NULL;
    char buf[65536];
    int len = 0, rc, n;
    long long total = 0;
    FILE *fp;

    if (argc < 3) { printf ("usage: errcli DB FULL_BACKUP_PATH\n"); return 9; }

    if (cubrid_backup_initialize () == -1) { printf ("[NOK] init\n"); return 1; }

    /* ── phase 1: early end after 5 chunks ── */
    set_info (&info, argv[1]);
    if (cubrid_backup_begin (&info, &h) == -1) { printf ("[NOK] p1 begin\n"); return 1; }
    for (n = 0; n < 5; n++)
    {
        rc = cubrid_backup_read (h, buf, sizeof (buf), &len);
        if (rc == -1) { printf ("[NOK] p1 read#%d\n", n); return 1; }
        if (rc == 0) break;                      /* stream ended earlier than 5 chunks */
    }
    rc = cubrid_backup_end (h);                  /* mid-stream end: must not hang */
    printf ("[P1] early end rc=%d (0=success, -1=failure; either is fine, no-hang is the test)\n", rc);

    /* ── phase 2: full backup on the same (reused) handle slot ── */
    set_info (&info, argv[1]);
    h = NULL;
    if (cubrid_backup_begin (&info, &h) == -1) { printf ("[NOK] p2 begin (handle not reusable?)\n"); return 2; }
    fp = fopen (argv[2], "w+b");
    if (fp == NULL) { printf ("[NOK] p2 fopen\n"); return 2; }
    while (1)
    {
        rc = cubrid_backup_read (h, buf, sizeof (buf), &len);
        if (rc == -1) { printf ("[NOK] p2 read\n"); return 2; }
        if (len > 0) { fwrite (buf, 1, len, fp); total += len; }
        if (rc == 0) break;
    }
    fclose (fp);
    if (cubrid_backup_end (h) == -1) { printf ("[NOK] p2 end\n"); return 2; }
    printf ("[P2] full backup after abort: %lld bytes\n", total);
    printf ("[OK] errcli: early-end + handle-reuse passed\n");
    return 0;
}
