#include <stdlib.h>
#include <stdarg.h>
#include <regex.h>
#include <time.h>
#include <sys/timeb.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include "backup_manager.h"

#define INT_MAX 2147483647

#ifndef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#endif

#define LOG_HEADER_MAX_SIZE  (50)
#define LOG_MESSAGE_MAX_SIZE (LOG_HEADER_MAX_SIZE + 1024)

BACKUP_MANAGER backup_manager;

BACKUP_MANAGER* backup_mgr = &backup_manager;

/* print_log() serializes on this leaf lock: once the drain thread also logs,
 * the static log_buffer and the append to log_fp are shared state. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* fork-safety: the backup child forked in execute_backup runs briefly before
 * execv and may call print_log, which takes log_mutex. If another thread (e.g. the
 * drain) held log_mutex at the instant of fork, the child would inherit it locked
 * and self-deadlock. These pthread_atfork handlers acquire it around fork so the
 * child inherits a consistent mutex and then unlocks it. */
static void log_atfork_prepare (void) { pthread_mutex_lock (&log_mutex); }
static void log_atfork_parent  (void) { pthread_mutex_unlock (&log_mutex); }
static void log_atfork_child   (void) { pthread_mutex_unlock (&log_mutex); }

static
int make_log_header (char* buf, size_t buf_len, const char *prefix_str)
{
    struct tm tm, * tm_p;
    time_t sec;
    int millisec, len;
    struct timeb tb;
    char* p;

    if (buf == NULL)
    {
        return 0;
    }

    /* current time */
    (void) ftime (&tb);
    sec = tb.time;
    millisec = tb.millitm;

    tm_p = localtime_r (&sec, &tm);

    len = (int) strftime (buf, buf_len, "%y-%m-%d %H:%M:%S", tm_p);
    p = buf + len;
    buf_len -= len;

    len += snprintf (p, buf_len, ".%03d (%d) %s ", millisec, getpid (), (prefix_str ? prefix_str : ""));

    assert (len <= LOG_HEADER_MAX_SIZE);

    return len;
}

int print_log (const char *prefix_str, const char *msg, ...)
{
    static char log_buffer[LOG_MESSAGE_MAX_SIZE];

    char* p;
    int len, n;

    va_list arg_list;

    if (IS_NULL (backup_mgr->log_fp))
    {
        return SUCCESS;
    }

    pthread_mutex_lock (&log_mutex);

    p = log_buffer;
    len = LOG_MESSAGE_MAX_SIZE;
    n = make_log_header (p, len, prefix_str);
    len -= n;
    p += n;

    if (len > 0)
    {
        va_start (arg_list, msg);
        n = vsnprintf (p, len, msg, arg_list);
        if (n >= len)
        {
            p[len - 1] = '\0';
        }
        va_end (arg_list);
    }

    /* "%s": log_buffer is data, not a format string — conf-supplied values
     * (e.g. buffer_disk_path) may contain '%'. */
    fprintf (backup_mgr->log_fp, "%s", log_buffer);
    fflush (backup_mgr->log_fp);

    pthread_mutex_unlock (&log_mutex);

    return SUCCESS;
}

static
int open_log_file (void)
{
    char log_file_path[PATH_MAX];

    snprintf (log_file_path, PATH_MAX, "%s/log/cubrid_backup.log", getenv ("CUBRID"));

    backup_mgr->log_fp = fopen (log_file_path, "a");
    if (backup_mgr->log_fp == NULL)
    {
        //PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int close_log_file (void)
{
    if (backup_mgr->log_fp != NULL)
    {
        fclose (backup_mgr->log_fp);
    }

    backup_mgr->log_fp = NULL;

    return SUCCESS;
}

int validate_dir (const char* path)
{
    struct stat path_stat;

    if (IS_NULL (path) || strlen (path) >= PATH_MAX)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error; 
    }

    if (IS_FAILURE (access (path, R_OK | W_OK | X_OK)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (stat (path, &path_stat)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else
    {
        if (!S_ISDIR (path_stat.st_mode))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

int remove_all_entries (const char* dir)
{
    return SUCCESS;
}

static
int remove_trailing_slash (char* path_buffer)
{
    int path_len;
    int i;

    if (IS_NULL (path_buffer))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    path_len = strlen (path_buffer);

    for (i = path_len - 1; i >= 0; i --)
    {
        if (path_buffer[i] == ' ')
        {
            continue;
        }
        else
        {
            if (path_buffer[i] == '/')
            {
                path_buffer[i] = '\0';
            }

            break;
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

int check_path_length_limit (const char* path)
{
    if (strlen (path) >= PATH_MAX - 1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_cubrid_home (void)
{
    char* cub_home;

    cub_home = getenv ("CUBRID");

    if (IS_FAILURE (validate_dir (cub_home)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    snprintf (backup_mgr->cubrid_home, PATH_MAX, "%s", cub_home);

    remove_trailing_slash (backup_mgr->cubrid_home);

    return SUCCESS;

error:

    return FAILURE;
}

static
int make_backup_home (void)
{
    char backup_path[PATH_MAX];

    /* (1) $CUBRID/tmp */
    snprintf (backup_path, PATH_MAX, "%s/tmp", backup_mgr->cubrid_home);

    // 여기서 체크 후 error log 남을 수 있지만 버그 아니다.
    if (IS_SUCCESS (validate_dir (backup_path)))
    {

    }
    else
    {
        /* (2) $CUBRID_TMP */
        snprintf (backup_path, PATH_MAX, "%s", getenv ("CUBRID_TMP"));

        remove_trailing_slash (backup_path);

        // 여기서 체크 후 error log 남을 수 있지만 버그 아니다.
        if (IS_SUCCESS (validate_dir (backup_path)))
        {

        }
        else
        {
            /* (3) /tmp */
            snprintf (backup_path, PATH_MAX, "%s", "/tmp");

            if (IS_SUCCESS (validate_dir (backup_path)))
            {

            }
            else
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }
        }
    }

    snprintf (backup_mgr->backup_home, PATH_MAX, "%s/%s", backup_path, ".cubrid_backup");

    if (IS_FAILURE (check_path_length_limit (backup_mgr->backup_home)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (mkdir (backup_mgr->backup_home, S_IRWXU)))
    {
        if (errno != EEXIST)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
        else
        {
            if (IS_FAILURE (validate_dir (backup_mgr->backup_home)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }
            else
            {
                if (IS_FAILURE (remove_all_entries (backup_mgr->backup_home)))
                {
                    PRINT_LOG_ERR (ERR_INFO);
                    goto error;
                }
            }
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int remove_backup_home (void)
{
    if (IS_FAILURE (remove_all_entries (backup_mgr->backup_home)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

#if 0
    if (IS_FAILURE (rmdir (backup_mgr->backup_home)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
#endif

    backup_mgr->backup_home[0] = '\0';

    return SUCCESS;

error:

    return FAILURE;
}

static
int init_default_backup_option (void)
{
    BACKUP_OPTION* backup_opt;

    backup_opt = &backup_mgr->default_backup_option;

    backup_opt->remove_archive     = false;
    backup_opt->sa_mode            = false;
    backup_opt->no_check           = false; /* [M] */
    backup_opt->thread_count       = 0;     /* [M] */
    backup_opt->compress           = false; /* [M] */
    backup_opt->except_active_log  = false; /* [M] */
    backup_opt->sleep_msecs        = 0;     /* [M] */

    backup_opt->fifo_size              = 64 * 1024;          /* 64KB               */
    backup_opt->buffer_memory_size     = 0;                  /* buffering OFF (opt-in): set >0 to enable the drain/tiered-buffer path */
    backup_opt->buffer_disk_limit      = 0;                  /* disk tier off      */
    backup_opt->buffer_disk_path[0]    = '\0';
    backup_opt->buffer_disk_keep_spool = false;

    return SUCCESS;
}

static
int init_default_restore_option (void)
{
    RESTORE_OPTION* restore_opt;

    restore_opt = &backup_mgr->default_restore_option;

    restore_opt->partial_recovery           = false;
    restore_opt->use_database_location_path = false;

    return SUCCESS;
}

static
int set_bool_value (bool* dest, char* src)
{
    if (IS_ZERO (strncasecmp (src, "true", 5)))
    {
        *dest = true;

        goto end;
    }

    if (IS_ZERO (strncasecmp (src, "false", 6)))
    {
        *dest = false;

        goto end;
    }

    if (IS_ZERO (strncmp (src, "1", 2)))
    {
        *dest = true;

        goto end;
    }

    if (IS_ZERO (strncmp (src, "0", 2)))
    {
        *dest = false;

        goto end;
    }

    return FAILURE;

end:

    return SUCCESS;
}

static
int set_int_value (int* dest, char* src)
{
    int i;
    long long_val;

    for (i = 0; i < strlen (src); i++)
    {
        if (src[i] < '0' || src[i] > '9')
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    long_val = strtol (src, NULL, 10);

    if (errno == ERANGE || long_val > INT_MAX)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    *dest = (int) long_val;

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_size_value (long long* dest, char* src)
{
    char* endp;
    long long val, factor = 1;

    errno = 0;
    val = strtoll (src, &endp, 10);

    if (errno == ERANGE || val < 0)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* optional binary (1024-based) size suffix */
    if (*endp != '\0')
    {
        if (IS_ZERO (strncasecmp (endp, "KB", 3)))
        {
            factor = 1024LL;
        }
        else if (IS_ZERO (strncasecmp (endp, "MB", 3)))
        {
            factor = 1024LL * 1024;
        }
        else if (IS_ZERO (strncasecmp (endp, "GB", 3)))
        {
            factor = 1024LL * 1024 * 1024;
        }
        else
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    if (val > LLONG_MAX / factor)   /* suffix-multiply overflow guard */
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    *dest = val * factor;

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_path_value (char* dest, size_t dest_size, char* src)
{
    if (strlen (src) >= dest_size)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    snprintf (dest, dest_size, "%s", src);

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_backup_option (char* key, char* value)
{
    BACKUP_OPTION* backup_opt;

    long long sz;

    backup_opt = &backup_mgr->default_backup_option;

    if (IS_ZERO (strncasecmp (key, "remove_archive", 15)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->remove_archive, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "sa_mode", 8)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->sa_mode, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "no_check", 9)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->no_check, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "thread_count", 13)))
    {
        if (IS_FAILURE (set_int_value (&backup_opt->thread_count, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "compress", 9)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->compress, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "except_active_log", 18)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->except_active_log, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "sleep_msecs", 12)))
    {
        if (IS_FAILURE (set_int_value (&backup_opt->sleep_msecs, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "fifo_size", 10)))
    {
        if (IS_FAILURE (set_size_value (&sz, value)) || sz > INT_MAX)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        backup_opt->fifo_size = (int) sz;
    }
    else if (IS_ZERO (strncasecmp (key, "buffer_memory_size", 19)))
    {
        if (IS_FAILURE (set_size_value (&backup_opt->buffer_memory_size, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "buffer_disk_limit", 18)))
    {
        if (IS_FAILURE (set_size_value (&backup_opt->buffer_disk_limit, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "buffer_disk_path", 17)))
    {
        if (IS_FAILURE (set_path_value (backup_opt->buffer_disk_path, PATH_MAX, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (IS_ZERO (strncasecmp (key, "buffer_disk_keep_spool", 23)))
    {
        if (IS_FAILURE (set_bool_value (&backup_opt->buffer_disk_keep_spool, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_restore_option (char* key, char* value)
{
    RESTORE_OPTION* restore_opt;

    restore_opt = &backup_mgr->default_restore_option;

    if (0 == strncasecmp (key, "partial_recovery", 17))
    {
        if (IS_FAILURE (set_bool_value (&restore_opt->partial_recovery, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (0 == strncasecmp (key, "use_database_location_path", 27))
    {
        if (IS_FAILURE (set_bool_value (&restore_opt->use_database_location_path, value)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int compile_regex (regex_t* re_opt_header, regex_t* re_opt, regex_t* re_empty_line)
{
    char* regex_header     = "^[[:blank:]]*\\[[[:blank:]]*(backup|restore)[[:blank:]]*\\][[:space:]]*$";
    char* regex_key_value  = "^[[:blank:]]*([_[:alpha:]]+)[[:blank:]]*=[[:blank:]]*([[:alnum:]/._-]+)[[:space:]]*$";
    char* regex_empty_line = "^[[:space:]]*$";

    if (IS_FAILURE (regcomp (re_opt_header, regex_header, REG_ICASE | REG_EXTENDED)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (regcomp (re_opt, regex_key_value, REG_ICASE | REG_EXTENDED)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (regcomp (re_empty_line, regex_empty_line, REG_ICASE | REG_EXTENDED)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int free_regex (regex_t* re_opt_header, regex_t* re_opt, regex_t* re_empty_line)
{
    regfree (re_opt_header);
    regfree (re_opt);
    regfree (re_empty_line);

    return SUCCESS;
}

static
int read_option (FILE* conf_fp)
{
    regex_t re_opt_header;
    regex_t re_opt;
    regex_t re_empty_line;

    regmatch_t match[3];

    char line_str[1024];

    char* key;
    char* value;

    enum read_state
    {
        BACKUP_OPTION_READ_STATE,
        RESTORE_OPTION_READ_STATE,
        NO_READ_STATE
    };

    enum read_state read_state = NO_READ_STATE;

    int state = 0;

    if (IS_FAILURE (compile_regex (&re_opt_header, &re_opt, &re_empty_line)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    while (NULL != fgets (line_str, 1024, conf_fp))
    {
        /* empty string */
        if (IS_SUCCESS (regexec (&re_empty_line, line_str, 3, match, 0)))
        {
            continue;
        }

        /* [header] */
        if (IS_SUCCESS (regexec (&re_opt_header, line_str, 3, match, 0)))
        {
            /* [backup] */
            if (line_str[match[1].rm_so] == 'b')
            {
                read_state = BACKUP_OPTION_READ_STATE;

                continue;
            }
            /* [restore] */
            else if (line_str[match[1].rm_so] == 'r')
            {
                read_state = RESTORE_OPTION_READ_STATE;

                continue;
            }
        }

        /* key = value */
        if (IS_SUCCESS (regexec (&re_opt, line_str, 3, match, 0)))
        {
            key = &line_str[match[1].rm_so];
            line_str[match[1].rm_eo] = '\0';

            value = &line_str[match[2].rm_so];
            line_str[match[2].rm_eo] = '\0';

            if (read_state == BACKUP_OPTION_READ_STATE)
            {
                if (IS_FAILURE (set_backup_option (key, value)))
                {
                    PRINT_LOG_ERR (ERR_INFO);
                    goto error;
                }
            }
            else if (read_state == RESTORE_OPTION_READ_STATE)
            {
                if (IS_FAILURE (set_restore_option (key, value)))
                {
                    PRINT_LOG_ERR (ERR_INFO);
                    goto error;
                }
            }
            else
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }
        }
    }

    free_regex (&re_opt_header, &re_opt, &re_empty_line);

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            free_regex (&re_opt_header, &re_opt, &re_empty_line);
        default:
            break;
    }

    return FAILURE;
}

static
int read_conf_file (void)
{
    char conf_file[PATH_MAX];
    FILE* conf_fp = NULL;

    int state = 0;

    init_default_backup_option ();
    init_default_restore_option ();

    snprintf (conf_file, PATH_MAX, "%s/conf/cubrid_backup.conf", backup_mgr->cubrid_home);

    if (IS_FAILURE (check_path_length_limit (conf_file)))
    {
        goto end;
    }

    conf_fp = fopen (conf_file, "r");
    if (conf_fp == NULL)
    {
        goto end;
    }

    state = 1;

    if (IS_FAILURE (read_option (conf_fp)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    fclose (conf_fp);

end:

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            fclose (conf_fp);
        default:
            break;
    }

    return FAILURE;
}

static
int set_io_size (void)
{
    struct stat backup_home_stat;

    if (IS_FAILURE (stat (backup_mgr->backup_home, &backup_home_stat)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    backup_mgr->io_size = backup_home_stat.st_blksize * 8;

    return SUCCESS;

error:

    return FAILURE;
}

static
int validate_backup_option (void)
{
    BACKUP_OPTION* backup_opt;

    backup_opt = &backup_mgr->default_backup_option;

    /* fifo_size: clamp to [64KB, 1MB] (Linux default pipe-max-size). Non-fatal. */
    if (backup_opt->fifo_size < 64 * 1024)
    {
        PRINT_LOG_WARN ("fifo_size below 64KB; raising to 64KB\n");
        backup_opt->fifo_size = 64 * 1024;
    }

    if (backup_opt->fifo_size > 1024 * 1024)
    {
        PRINT_LOG_WARN ("fifo_size above 1MB; clamping to 1MB\n");
        backup_opt->fifo_size = 1024 * 1024;
    }

    /* memory tier must hold at least one io_size chunk when enabled */
    if (backup_opt->buffer_memory_size != 0 &&
        backup_opt->buffer_memory_size < backup_mgr->io_size)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* disk tier is an overflow of the memory tier and needs a valid spool dir */
    if (backup_opt->buffer_disk_limit > 0)
    {
        struct statvfs vfs;

        if (backup_opt->buffer_memory_size == 0)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (validate_dir (backup_opt->buffer_disk_path)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        /* free-space shortfall is a warning, not a failure (runtime ENOSPC degrades) */
        if (statvfs (backup_opt->buffer_disk_path, &vfs) == 0)
        {
            long long avail = (long long) vfs.f_bavail * (long long) vfs.f_frsize;

            if (avail < backup_opt->buffer_disk_limit)
            {
                PRINT_LOG_WARN ("spool free space is below buffer_disk_limit\n");
            }
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

int start_backup_manager (void)
{
    static bool atfork_registered = false;

    if (!atfork_registered)                 /* register once per process, before any backup fork */
    {
        (void) pthread_atfork (log_atfork_prepare, log_atfork_parent, log_atfork_child);
        atfork_registered = true;
    }

    if (IS_FAILURE (open_log_file ()))
    {
        //PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (set_cubrid_home ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (make_backup_home ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (read_conf_file ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (set_io_size ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (validate_backup_option ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

int stop_backup_manager (void)
{
    if (IS_FAILURE (remove_backup_home ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    close_log_file ();

    return SUCCESS;

error:

    return FAILURE;
}
