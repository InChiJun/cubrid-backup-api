#ifndef _BACKUP_MANAGER_H_
#define _BACKUP_MANAGER_H_

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "backup_common.h"

#define ERR_INFO "in %s () at %s:%d\n", __func__, __FILE__, __LINE__

#define PRINT_LOG_INFO(...) \
    print_log ("INFO:", __VA_ARGS__)
#define PRINT_LOG_ERR(...) \
    print_log ("ERROR:", __VA_ARGS__)
#define PRINT_LOG_WARN(...) \
    print_log ("WARNING:", __VA_ARGS__)

typedef struct backup_option BACKUP_OPTION;
struct backup_option
{
    bool remove_archive;
    bool sa_mode;
    bool no_check;
    int thread_count;
    bool compress;
    bool except_active_log;
    int sleep_msecs;

    int       fifo_size;              /* bytes, default 64KB, clamped <= 1MB */
    long long buffer_memory_size;     /* bytes, 0 = buffering disabled       */
    long long buffer_disk_limit;      /* bytes, 0 = no disk tier             */
    char      buffer_disk_path[PATH_MAX];
    bool      buffer_disk_keep_spool; /* default false = unlink-on-open      */
};

typedef struct restore_option RESTORE_OPTION;
struct restore_option
{
    bool partial_recovery;
    bool use_database_location_path;
};

typedef struct backup_manager BACKUP_MANAGER;
struct backup_manager
{
    char cubrid_home[PATH_MAX];
    char backup_home[PATH_MAX];

    int io_size; /* PAGE_SIZE * 8 */

    BACKUP_OPTION default_backup_option;
    RESTORE_OPTION default_restore_option;

    FILE* log_fp;
};

extern BACKUP_MANAGER* backup_mgr;

int start_backup_manager (void);
int stop_backup_manager (void);
int validate_dir (const char*);
int check_path_length_limit (const char*);

int print_log (const char *prefix_str, const char *msg, ...);
#endif
