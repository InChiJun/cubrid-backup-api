/*
 * backup_tc05 - slow-consumer / tiered-buffer stress
 *
 * Simulates a slow downstream (e.g. tape) by sleeping between reads so the
 * tiered buffer (memory -> disk spool -> WAIT/backpressure) is exercised.
 * Drive the tier that is hit with the buffer_* keys in cubrid_backup.conf:
 *   - buffer_memory_size small                  -> memory tier fills, WAIT
 *   - buffer_memory_size small + buffer_disk_*  -> spill to disk spool
 *   - buffer_memory_size=0                       -> buffering off (legacy path)
 * The produced backup file must still restore byte-for-byte, so run_test.sh
 * restores it and compares. A small read buffer + per-read delay make the
 * buffer do real work regardless of downstream speed.
 *
 * ./backup_tc05 [DB_NAME] [BACKUP_LEVEL] [BACKUP_FILE_PATH] [DELAY_US] [READ_BUF]
 *   DELAY_US : microseconds to sleep between reads (default 3000)
 *   READ_BUF : read buffer size in bytes           (default 4096)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "cubrid_backup_api.h"

void usage ()
{
    printf ("./backup_tc05 [DB_NAME] [BACKUP_LEVEL] [BACKUP_FILE_PATH] [DELAY_US] [READ_BUF]\n\n");
    printf ("ex)\n");
    printf ("slow consumer ==> ./backup_tc05 demodb 0 ./backup_dir/demodb_bk0v000 3000 4096\n");
}

void set_backup_info (CUBRID_BACKUP_INFO *backup_info, char *db_name, char *backup_level)
{
    backup_info->backup_level   = atoi (backup_level);
    backup_info->remove_archive = -1;
    backup_info->sa_mode        = -1;
    backup_info->no_check       = -1;
    backup_info->compress       = -1;
    backup_info->db_name        = db_name;
}

int main (int argc, char *argv[])
{
    CUBRID_BACKUP_INFO cub_backup_info;
    void *cub_backup_handle = NULL;

    char *backup_data_buffer;
    int   backup_data_size = 0;
    int   total_backup_data_size = 0;
    int   backup_result;

    long  delay_us = 3000;
    int   read_buf = 4096;

    FILE *backup_fp;

    if (argc < 4)
    {
        usage ();
        exit (1);
    }

    if (argc > 4)
    {
        delay_us = atol (argv[4]);
    }

    if (argc > 5)
    {
        read_buf = atoi (argv[5]);
    }

    if (read_buf <= 0)
    {
        read_buf = 4096;
    }

    backup_data_buffer = malloc (read_buf);

    if (backup_data_buffer == NULL)
    {
        printf ("[NOK] out of memory for read buffer\n");
        exit (1);
    }

    set_backup_info (&cub_backup_info, argv[1], argv[2]);

    backup_fp = fopen (argv[3], "w+b");

    if (backup_fp == NULL)
    {
        printf ("[NOK] cannot open backup file %s\n", argv[3]);
        exit (1);
    }

    if (-1 == cubrid_backup_initialize ())
    {
        printf ("[NOK] failed the execution of cubrid_backup_initialize ()\n");
        exit (1);
    }

    if (-1 == cubrid_backup_begin (&cub_backup_info, &cub_backup_handle))
    {
        printf ("[NOK] failed the execution of cubrid_backup_begin ()\n");
        exit (1);
    }

    while (1)
    {
        backup_result = cubrid_backup_read (cub_backup_handle, backup_data_buffer, read_buf, &backup_data_size);
        if (-1 == backup_result)
        {
            printf ("[NOK] failed the execution of cubrid_backup_read ()\n");
            exit (1);
        }

        if (backup_data_size != 0)
        {
            fwrite (backup_data_buffer, 1, backup_data_size, backup_fp);

            total_backup_data_size += backup_data_size;
        }

        if (0 == backup_result) // 0: backup end, 1: read more backup data
        {
            break;
        }

        /* slow downstream: let the tiered buffer fill / spill / apply backpressure */
        if (delay_us > 0)
        {
            usleep (delay_us);
        }
    }

    if (0 == total_backup_data_size)
    {
        printf ("[NOK] backup_data_size ==> %d\n", total_backup_data_size);
    }
    else
    {
        printf ("[OK] backup_data_size ==> %d\n", total_backup_data_size);
    }

    fclose (backup_fp);

    free (backup_data_buffer);

    if (-1 == cubrid_backup_end (cub_backup_handle))
    {
        printf ("[NOK] failed the execution of cubrid_backup_end ()\n");
        exit (1);
    }

    if (-1 == cubrid_backup_finalize ())
    {
        printf ("[NOK] failed the execution of cubrid_backup_finalize ()\n");
        exit (1);
    }

    return 0;
}
