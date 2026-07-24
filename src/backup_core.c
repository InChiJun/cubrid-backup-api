#include <sys/wait.h>
#include <sys/prctl.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include "backup_api.h"
#include "backup_core.h"
#include "backup_manager.h"
#include "handle_manager.h"
#include "cubrid_backup_format.h"

pthread_once_t backup_api_once_initialize = PTHREAD_ONCE_INIT;
pthread_once_t backup_api_once_finalize   = PTHREAD_ONCE_INIT;

pthread_mutex_t backup_api_state_mutex;
BACKUP_API_STATE backup_api_state = BACKUP_API_STATE_NOT_READY;

void initialize_backup_api (void)
{
    pthread_mutex_init (&backup_api_state_mutex, NULL);

    backup_api_once_finalize = PTHREAD_ONCE_INIT;
}

void finalize_backup_api (void)
{
    if (backup_api_once_initialize != PTHREAD_ONCE_INIT)
    {
        pthread_mutex_destroy (&backup_api_state_mutex);
        backup_api_once_initialize = PTHREAD_ONCE_INIT;
    }

    backup_api_state = BACKUP_API_STATE_NOT_READY;
}

int transit_backup_api_state (BACKUP_API_STATE state_now, BACKUP_API_STATE state_where)
{
    int state = 0;

    if (backup_api_once_initialize == PTHREAD_ONCE_INIT)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (backup_api_state != state_now)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    backup_api_state = state_where;

    if (IS_FAILURE (pthread_mutex_unlock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&backup_api_state_mutex);
        default:
            break;
    }

    return FAILURE;
}

static
int transit_backup_api_state_to_end (void)
{
    int state = 0;

    if (backup_api_once_initialize == PTHREAD_ONCE_INIT)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (backup_api_state == BACKUP_API_STATE_NOT_READY ||
        backup_api_state == BACKUP_API_STATE_INITIALIZING)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    backup_api_state = BACKUP_API_STATE_FINALIZING;

    if (IS_FAILURE (pthread_mutex_unlock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&backup_api_state_mutex);
        default:
            break;
    }

    return FAILURE;
}

static
int check_backup_api_state (BACKUP_API_STATE state_now)
{
    if (backup_api_once_initialize == PTHREAD_ONCE_INIT)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (backup_api_state != state_now)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_unlock (&backup_api_state_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    pthread_mutex_unlock (&backup_api_state_mutex);

    return FAILURE;
}

int check_api_call_sequence (FUNC_CALL func_call)
{
    switch (func_call)
    {
        case FUNC_CALL_INITIALIZE:
            if (IS_FAILURE (transit_backup_api_state (BACKUP_API_STATE_NOT_READY, BACKUP_API_STATE_INITIALIZING)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        case FUNC_CALL_FINALIZE:
            if (IS_FAILURE (transit_backup_api_state_to_end ()))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        case FUNC_CALL_BACKUP_BEGIN:
            if (IS_FAILURE (transit_backup_api_state (BACKUP_API_STATE_READY, BACKUP_API_STATE_BACKUP_SERVICE)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        case FUNC_CALL_BACKUP_END:
        case FUNC_CALL_BACKUP_READ:
            if (IS_FAILURE (check_backup_api_state (BACKUP_API_STATE_BACKUP_SERVICE)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        case FUNC_CALL_RESTORE_BEGIN:
            if (IS_FAILURE (transit_backup_api_state (BACKUP_API_STATE_READY, BACKUP_API_STATE_RESTORE_SERVICE)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        case FUNC_CALL_RESTORE_END:
        case FUNC_CALL_RESTORE_WRITE:
            if (IS_FAILURE (check_backup_api_state (BACKUP_API_STATE_RESTORE_SERVICE)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;

        default:
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int check_backup_info (CUBRID_BACKUP_INFO* backup_info)
{
    if (backup_info->backup_level < BACKUP_FULL_LEVEL ||
        backup_info->backup_level > BACKUP_SMALL_INCREMENT_LEVEL)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    // -1: use cubrid_backup.conf configuration
    // 0 : unset
    // 1 : set
    if (backup_info->remove_archive < -1 ||
        backup_info->remove_archive > 1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (backup_info->sa_mode < -1 ||
        backup_info->sa_mode > 1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (backup_info->no_check < -1 ||
        backup_info->no_check > 1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (backup_info->compress < -1 ||
        backup_info->compress > 1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_NULL (backup_info->db_name))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (strlen (backup_info->db_name) > MAX_DB_NAME_LEN)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_backup_info (CUBRID_BACKUP_INFO* backup_info, BACKUP_HANDLE* backup_handle)
{
    BACKUP_OPTION* backup_opt;

    if (IS_FAILURE (check_backup_info (backup_info)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    backup_opt = &backup_mgr->default_backup_option;

    backup_handle->backup_level = backup_info->backup_level;

    if (backup_info->remove_archive == -1)
    {
        backup_handle->remove_archive = backup_opt->remove_archive;
    }
    else
    {
        backup_handle->remove_archive = backup_info->remove_archive == 1 ? true : false;
    }

    if (backup_info->sa_mode == -1)
    {
        backup_handle->sa_mode = backup_opt->sa_mode;
    }
    else
    {
        backup_handle->sa_mode = backup_info->sa_mode == 1 ? true : false;
    }

    if (backup_info->no_check == -1)
    {
        backup_handle->no_check = backup_opt->no_check;
    }
    else
    {
        backup_handle->no_check = backup_info->no_check == 1 ? true : false;
    }

    if (backup_info->compress == -1)
    {
        backup_handle->compress = backup_opt->compress;
    }
    else
    {
        backup_handle->compress = backup_info->compress == 1 ? true : false;
    }

    snprintf (backup_handle->db_name, MAX_DB_NAME_LEN + 1, "%s", backup_info->db_name);

    return SUCCESS;

error:

    return FAILURE;
}

static
int check_restore_info (CUBRID_RESTORE_INFO* restore_info)
{
    if (/* restore_info->restore_type != RESTORE_TO_DB || */
        restore_info->restore_type != RESTORE_TO_FILE)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (restore_info->backup_level < BACKUP_FULL_LEVEL ||
        restore_info->backup_level > BACKUP_SMALL_INCREMENT_LEVEL)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

#if 0
    if (restore_handle->up_to_date != NULL)
    {
        // check format, now not supported
    }
#endif

    if (restore_info->restore_type == RESTORE_TO_FILE)
    {
        if (IS_NULL (restore_info->backup_file_path))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (validate_dir (restore_info->backup_file_path)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    if (IS_NULL (restore_info->db_name))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (strlen (restore_info->db_name) > MAX_DB_NAME_LEN)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int set_restore_info (CUBRID_RESTORE_INFO* restore_info, RESTORE_HANDLE* restore_handle)
{
    if (IS_FAILURE (check_restore_info (restore_info)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    restore_handle->restore_type = restore_info->restore_type;

    restore_handle->backup_level = restore_info->backup_level;

    snprintf (restore_handle->backup_file_path, PATH_MAX, "%s", restore_info->backup_file_path);

    snprintf (restore_handle->db_name, MAX_DB_NAME_LEN + 1, "%s", restore_info->db_name);

    return SUCCESS;

error:

    return FAILURE;
}

static
int make_fifo (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = (BACKUP_HANDLE *)handle;

        /* ex) demodb_bk0v000 */
        snprintf (backup_handle->fifo_path, PATH_MAX, "%s/%s_bk%dv000", backup_mgr->backup_home,
                                                                        backup_handle->db_name,
                                                                        backup_handle->backup_level);

        if (IS_FAILURE (check_path_length_limit (backup_handle->fifo_path)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_SUCCESS (access (backup_handle->fifo_path, F_OK)))
        {
            if (IS_FAILURE (unlink (backup_handle->fifo_path)))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }
        }

        if (IS_FAILURE (mkfifo (backup_handle->fifo_path, S_IRUSR|S_IWUSR)))
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
int open_fifo (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;

    if (IS_FAILURE (make_fifo (handle_type, handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = (BACKUP_HANDLE *)handle;

        /* O_CLOEXEC: the forked backupdb must NOT inherit this read end — an
         * inherited reader keeps the FIFO alive after a client crash, so
         * backupdb never gets EPIPE and blocks in write() forever (orphan). */
        backup_handle->fifo_fd = open (backup_handle->fifo_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (backup_handle->fifo_fd == -1)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        /* Size the kernel pipe buffer on the read end before backupdb opens the
         * write end. Best-effort: on failure keep the default size and continue. */
        if (fcntl (backup_handle->fifo_fd, F_SETPIPE_SZ,
                   backup_mgr->default_backup_option.fifo_size) == -1)
        {
            PRINT_LOG_WARN ("F_SETPIPE_SZ failed; using default pipe size\n");
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
int remove_fifo (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = (BACKUP_HANDLE *)handle;

        if (IS_FAILURE (unlink (backup_handle->fifo_path)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        backup_handle->fifo_path[0] = '\0';
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
int close_fifo (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = (BACKUP_HANDLE *)handle;

        if (backup_handle->fifo_fd != -1)
        {
            close (backup_handle->fifo_fd);
        }

        backup_handle->fifo_fd = -1;
    }
    else
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (remove_fifo (handle_type, handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int execute_cubrid_backupdb (BACKUP_HANDLE* backup_handle)
{
    BACKUP_OPTION* backup_opt;

    char* argv[16];
    int idx = 0;

//    char cubrid[PATH_MAX];
    char cub_admin[PATH_MAX];

    char thread_count[11];
    char sleep_msecs[25];

    char* db_name;

#if 0
    // for INFO
    char backup_cmd[8192] = {0};
    int cmd_len = 0;
    int i;
#endif

    backup_opt = &backup_mgr->default_backup_option;

/*
    snprintf (cubrid, PATH_MAX, "%s/bin/cubrid", backup_mgr->cubrid_home);

    if (IS_FAILURE (check_path_length_limit (cubrid)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
*/
    //argv[idx ++] = cubrid;
    argv[idx ++] = "cubrid";

    argv[idx ++] = "backupdb";

    /* --destination-path */
    argv[idx ++] = "-D";
    argv[idx ++] = backup_handle->fifo_path;

    /* --remove-archive */
    if (backup_handle->remove_archive == true)
    {
        argv[idx ++] = "-r";
    }

    /* --level */
    argv[idx ++] = "-l";

    if (backup_handle->backup_level == BACKUP_FULL_LEVEL)
    {
        argv[idx ++] = "0";
    }
    else if (backup_handle->backup_level == BACKUP_BIG_INCREMENT_LEVEL)
    {
        argv[idx ++] = "1";
    }
    else if (backup_handle->backup_level == BACKUP_SMALL_INCREMENT_LEVEL)
    {
        argv[idx ++] = "2";
    }

    /* --SA-mode */
    if (backup_handle->sa_mode == true)
    {
        argv[idx ++] = "-S";
    }

    /* --no-check */
    if (backup_handle->no_check == true)
    {
        argv[idx ++] = "--no-check";
    }

    /* --thread-count */
    if (backup_opt->thread_count != 0)
    {
        snprintf (thread_count, 11, "%d", backup_opt->thread_count);

        argv[idx ++] = "-t";
        argv[idx ++] = thread_count;
    }

    /* --compress / --no-compress: 11.3+ backupdb compresses (LZ4) BY DEFAULT,
     * so omitting -z does NOT give an uncompressed stream — compress=false
     * must pass --no-compress explicitly or the option silently means LZ4. */
    if (backup_handle->compress == true)
    {
        argv[idx ++] = "-z";
    }
    else
    {
        argv[idx ++] = "--no-compress";
    }

    /* --except-active-log */
    if (backup_opt->except_active_log == true)
    {
        argv[idx ++] = "-e";
    }

    /* --sleep-msecs */
    if (backup_opt->sleep_msecs != 0)
    {
        snprintf (sleep_msecs, 25, "--sleep-msecs=%d", backup_opt->sleep_msecs);

        argv[idx ++] = sleep_msecs;
    }

    db_name = backup_handle->db_name;

    argv[idx ++] = db_name;

    argv[idx] = '\0';

    // Changed cub_admin -> cubrid per tech request: to leave a record in cubrid_utility.log
    //snprintf (cub_admin, PATH_MAX, "%s/bin/cub_admin", backup_mgr->cubrid_home);
    snprintf (cub_admin, PATH_MAX, "%s/bin/cubrid", backup_mgr->cubrid_home);

    if (IS_FAILURE (check_path_length_limit (cub_admin)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

#if 0
    for (i = 0; i < idx - 1; i ++)
    {
        cmd_len += snprintf (backup_cmd + cmd_len, "%s", argv[i]);
        backup_cmd[cmd_len] = ' '; // for space
        cmd_len ++;
    }

    backup_cmd[cmd_len - 1] = '\n';

    PRINT_LOG_INFO (backup_cmd);
    //printf ("%s\n", backup_cmd);
#endif

    if (-1 == execv (cub_admin, (char * const *)argv))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
void kill_process_group (pid_t pgid)
{
    struct sigaction act, old_act;

    act.sa_handler = SIG_IGN;
    sigemptyset (&act.sa_mask);
    act.sa_flags   = 0;

    //printf ("pid: %d, pgid: %d\n", getpid (), getpgid (getpid ()));

    // because the backup-api library itself must not die
    sigaction (SIGTERM, &act, &old_act);

    killpg (pgid, SIGTERM);

    sigaction (SIGTERM, &old_act, NULL);

#if 0
    // To leave "FAILURE: Has been interrupted." in cubrid_utility.log we
    // tried to use SIGINT, but cub_admin registers a SIGINT handler, so
    // cub_admin does not die immediately.
    // Leaving the "FAILURE: Has been interrupted." log itself means it terminated normally (handled).
    sigaction (SIGINT, &act, &old_act);

    killpg (pgid, SIGINT);

    sigaction (SIGINT, &old_act, NULL);
#endif
}

void zombie_handler()
{
    /*
    int status;
    int spid;
    spid = wait(&status);
    printf("child process wait succeeded \n");
    printf("================================\n");
    printf("PID         : %d\n", spid);
    printf("Exit Value  : %d\n", WEXITSTATUS(status));
    printf("Exit Stat   : %d\n", WIFEXITED(status));
    */
}

static
int check_backup_process_status (BACKUP_HANDLE* backup_handle, pid_t backup_pid)
{
    sigset_t sa_mask;
    struct timespec wait_timeout;

    int status;

    sigemptyset (&sa_mask);
    sigaddset (&sa_mask, SIGCHLD);
 
    wait_timeout.tv_sec  = 1;
    wait_timeout.tv_nsec = 0;

    // We register an empty handler for SIGCHLD because otherwise
    // the sigtimedwait () below fails to detect cubrid's termination.
    // As a result the cubrid utility ends up in a <defunct> state, and
    // cubrid_backup_read () does not learn that the backup thread has terminated.
    // We registered zombie_handler for testing, and as intended it
    // raises SIGCHLD when the cubrid utility exits, so we can catch it.
    // The reason why is not yet understood.
    signal(SIGCHLD, (void *)zombie_handler);

    // We currently fork cub_admin directly, but since the log is written by the cubrid utility,
    // backup-related records do not appear in the cubrid_utility.log referenced by the tech division.
    // So switch to forking cubrid instead, and on kill take everything down, descendants included.
    while (true)   
    {
        //sigemptyset (&sa_mask);
        //sigaddset (&sa_mask, SIGCHLD);
        //int ret;

        //ret = sigtimedwait (&sa_mask, NULL, &wait_timeout);
        //if (-1 == ret)
        if (-1 == sigtimedwait (&sa_mask, NULL, &wait_timeout))
        {
            /* timeout */
            if (errno == EAGAIN && backup_handle->is_cancel != true)
            {
                continue;
            }

            //printf ("must hit here 4\n");
            kill_process_group (getpgid (backup_pid));

            break;
        }
        else
        {
            //printf ("must hit here 1, ret val => %d, SIGCHLD => %d\n", ret, SIGCHLD);
            if (-1 == waitpid (backup_pid, &status, 0))
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            /* backup process(cubrid) return or exit () */
            if (WIFEXITED (status))
            {
                /*
                 * backup process return value:
                 * 0 - backup success
                 * 1 - backup failure
                 */
                if (WEXITSTATUS(status))
                {
                    PRINT_LOG_ERR (ERR_INFO);
                    goto error;
                }
            }
            /* backup process is dead by signal */
            else if (WIFSIGNALED(status)) /* signal */
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }
            /* backup process is stopped */
            else if (WIFSTOPPED(status))
            {
                kill_process_group (getpgid (backup_pid));

                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            break;
        }
    }

    //printf ("must hit here 2\n");
    return SUCCESS;

error:

    waitpid (backup_pid, NULL, WNOHANG);

    return FAILURE;
}

static void buf_mark_error (BACKUP_HANDLE* backup_handle);

static
void* execute_backup (void* handle)
{
    BACKUP_HANDLE* backup_handle = NULL;
    pid_t backup_pid = -1;

    if (IS_NULL (handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    // for kill process group
    setpgid (getpid (), getpid ());

    backup_handle = (BACKUP_HANDLE *)handle;

    backup_pid = fork ();

    if (backup_pid == -1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else if (backup_pid == 0) /* child process */
    {
        /* Die with the client: if the client is killed before backupdb opens
         * the FIFO write end, that open(O_WRONLY) blocks forever (no reader
         * will ever come; EPIPE only fires on write) — an unkillable orphan.
         * PDEATHSIG survives execv, so the kernel reaps us in every phase. */
        prctl (PR_SET_PDEATHSIG, SIGKILL);
        if (getppid () == 1)   /* parent already died between fork and prctl */
        {
            _exit (1);
        }

        if (IS_FAILURE (execute_cubrid_backupdb (backup_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else /* parent process */
    {
        if (IS_FAILURE (check_backup_process_status (backup_handle, backup_pid)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    // thread -> backup process
    // - The thread terminating does not mean backup_process has terminated.
    // - This thread --fork--> backup process structure likely needs review and improvement.
    //   - We took this structure because the cubrid backup utility has a part that
    //     validates the thread id that launched it.
    //   - Running the backup on a thread (linking libcubridsa.so) (a thread per backup, or one thread):
    //     on cubrid_backup_finalize () the boot_shutdown_client_at_exit () (registered via atexit ())
    //     function triggers a coredump.
    //     The analysis: the backup runs on a created thread while cubrid_backup_finalize () is
    //     called by the main thread, so the two thread ids differ, which causes it.
    //     A separate thread or process must be created to prevent a hang at the user level.
    //printf ("must hit here 3\n");
    set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_EXIT);

    pthread_exit (NULL);

error:

    set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_EXIT_WITH_ERROR);

    /* If backupdb died without ever opening the FIFO write end, the drain is
     * parked in poll() (a never-opened FIFO raises no event) and the reader in
     * not_empty — neither can observe this failure. Mark the buffer failed
     * (wakes the reader) and poke cancel_efd (wakes the drain) so the API
     * surfaces FAILURE instead of hanging. Parent thread only (not the fork
     * child: post-fork locks are unsafe there). */
    if (backup_handle != NULL && backup_pid != 0 && backup_handle->buffering_enabled)
    {
        buf_mark_error (backup_handle);

        if (backup_handle->cancel_efd != -1)
        {
            uint64_t one = 1;
            (void) write (backup_handle->cancel_efd, &one, sizeof (one));
        }
    }

    pthread_exit (NULL);
}

/* ───────────────────────── tiered buffer (Phase 1: memory tier) ───────────────────────── */

static
size_t mem_ring_free (BACKUP_HANDLE* backup_handle)
{
    return backup_handle->mem_cap - backup_handle->mem_len;
}

/* Lock-free wrap-aware copy of n bytes from the memory ring starting at
 * from_head into out. Does NOT touch shared state: the caller snapshots the head
 * under buf_lock, copies WITHOUT the lock (in this single-consumer ring the
 * committed region [head..head+len) is never overwritten by the drain, which
 * writes only free space), then advances head/len under buf_lock. n must be
 * <= mem_len observed at snapshot time. */
static
void mem_ring_copy (BACKUP_HANDLE* backup_handle, char* out, size_t from_head, size_t n)
{
    size_t first_run;

    if (n == 0)
    {
        return;
    }

    first_run = backup_handle->mem_cap - from_head;   /* contiguous run to buffer end */
    if (first_run > n)
    {
        first_run = n;
    }

    memcpy (out, backup_handle->mem_buf + from_head, first_run);

    if (n > first_run)                                /* wrap to buffer start */
    {
        memcpy (out + first_run, backup_handle->mem_buf, n - first_run);
    }
}

/* Append into the memory ring tail (mirror of mem_ring_copy). Caller holds
 * buf_lock. Writes min(len, free) bytes, wrap-aware, and returns that count.
 * Used only to place a held data-phase probe chunk. */
static
size_t mem_ring_put (BACKUP_HANDLE* backup_handle, const char* in, size_t len)
{
    size_t copy_len, tail_offset, first_run;

    copy_len = backup_handle->mem_cap - backup_handle->mem_len;        /* free */
    if (copy_len > len)
    {
        copy_len = len;
    }
    if (copy_len == 0)
    {
        return 0;
    }

    tail_offset = (backup_handle->mem_head + backup_handle->mem_len) % backup_handle->mem_cap;
    first_run   = backup_handle->mem_cap - tail_offset;          /* contiguous run to end */
    if (first_run > copy_len)
    {
        first_run = copy_len;
    }

    memcpy (backup_handle->mem_buf + tail_offset, in, first_run);

    if (copy_len > first_run)                      /* wrap to buffer start */
    {
        memcpy (backup_handle->mem_buf, in + first_run, copy_len - first_run);
    }

    backup_handle->mem_len += copy_len;

    return copy_len;
}

/* ── disk tier (Phase 2): fixed-size circular spool file ── */

static
long long disk_ring_free (BACKUP_HANDLE* backup_handle)
{
    return backup_handle->disk_cap - backup_handle->disk_len;
}

/* Lock-free wrap-aware read of n bytes from the disk spool ring starting at
 * from_head into out. Returns 0 on success, -1 on I/O error. Does NOT touch
 * shared state (same snapshot/commit discipline as mem_ring_copy): caller
 * snapshots disk_head under buf_lock, reads WITHOUT the lock, then advances
 * disk_head/disk_len under buf_lock. n must be <= disk_len at snapshot time. */
static
int disk_ring_read (BACKUP_HANDLE* backup_handle, char* out, long long from_head, size_t n)
{
    size_t first_run, offset;

    if (n == 0)
    {
        return 0;
    }

    first_run = (size_t) (backup_handle->disk_cap - from_head);   /* contiguous run to file end */
    if (first_run > n)
    {
        first_run = n;
    }

    offset = 0;
    while (offset < first_run)                       /* [head .. end) */
    {
        ssize_t read_bytes = pread (backup_handle->disk_fd, out + offset, first_run - offset, (off_t) (from_head + offset));
        if (read_bytes < 0) { if (errno == EINTR) continue; return -1; }
        if (read_bytes == 0) { return -1; }
        offset += (size_t) read_bytes;
    }

    offset = 0;
    while (offset < n - first_run)                   /* wrap: [0 .. n-first_run) */
    {
        ssize_t read_bytes = pread (backup_handle->disk_fd, out + first_run + offset, (n - first_run) - offset, (off_t) offset);
        if (read_bytes < 0) { if (errno == EINTR) continue; return -1; }
        if (read_bytes == 0) { return -1; }
        offset += (size_t) read_bytes;
    }

    return 0;
}

/* Reserve physical space for the spool so runtime pwrite never hits ENOSPC.
 * Returns 0 on success, -1 if space cannot be reserved (caller degrades). */
static
int reserve_spool_space (int fd, long long size)
{
    if (fallocate (fd, 0, 0, (off_t) size) == 0)
    {
        return 0;
    }

    if (errno == EOPNOTSUPP || errno == ENOSYS)
    {
        /* filesystem (e.g. tmpfs) lacks fallocate: force allocation via zero-fill */
        char   zero_buf[65536];
        long long offset = 0;

        memset (zero_buf, 0, sizeof (zero_buf));

        while (offset < size)
        {
            size_t chunk = (size - offset < (long long) sizeof (zero_buf)) ? (size_t) (size - offset) : sizeof (zero_buf);
            size_t written = 0;

            while (written < chunk)
            {
                ssize_t write_bytes = pwrite (fd, zero_buf + written, chunk - written, (off_t) (offset + (long long) written));
                if (write_bytes < 0) { if (errno == EINTR) continue; return -1; }
                written += (size_t) write_bytes;
            }

            offset += (long long) chunk;
        }

        return 0;
    }

    return -1;   /* ENOSPC / EIO / other */
}

/* Create the per-(db,level,pid) spool file, unlink-on-open by default, and
 * reserve its space. On success sets h->disk_fd and h->disk_cap. */
static
int create_spool_file (BACKUP_HANDLE* backup_handle)
{
    BACKUP_OPTION* opt = &backup_mgr->default_backup_option;

    char path[PATH_MAX];
    int  fd;
    int  flags = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC;

    snprintf (path, PATH_MAX, "%s/cubrid_bkbuf_%s_L%d_%d.spool",
              opt->buffer_disk_path, backup_handle->db_name, (int) backup_handle->backup_level, (int) getpid ());

    if (IS_FAILURE (check_path_length_limit (path)))
    {
        PRINT_LOG_WARN ("spool path too long\n");
        goto error;
    }

    fd = open (path, flags, 0600);

    if (fd == -1 && errno == EEXIST)   /* stale file from a prior same-pid crash */
    {
        unlink (path);
        fd = open (path, flags, 0600);
    }

    if (fd == -1)
    {
        PRINT_LOG_WARN ("spool open failed\n");
        goto error;
    }

    if (opt->buffer_disk_keep_spool != true)
    {
        unlink (path);   /* space reclaimed on close / process exit (incl. crash) */
    }

    if (reserve_spool_space (fd, opt->buffer_disk_limit) != 0)
    {
        close (fd);
        goto error;
    }

    backup_handle->disk_fd  = fd;
    backup_handle->disk_cap = opt->buffer_disk_limit;

    return SUCCESS;

error:

    return FAILURE;
}

/* Mark the buffer path failed and wake both waiters. Caller must NOT hold buf_lock. */
static
void buf_mark_error (BACKUP_HANDLE* backup_handle)
{
    pthread_mutex_lock (&backup_handle->buf_lock);
    backup_handle->buf_error = true;
    pthread_cond_broadcast (&backup_handle->not_empty);
    pthread_cond_broadcast (&backup_handle->not_full);
    pthread_mutex_unlock (&backup_handle->buf_lock);
}

/* Resolve a FIFO read()==0 into normal EOF vs early/abnormal EOF. Keyed on
 * backup_thread_state, but cancel also ends in THREAD_STATE_EXIT, so is_cancel
 * must independently force an error. Caller must NOT hold buf_lock. */
static
void drain_classify_eof (BACKUP_HANDLE* backup_handle)
{
    bool err = true;
    int  tries = 0;

    for (;;)
    {
        THREAD_STATE thread_state = backup_handle->backup_thread_state;

        if (backup_handle->is_cancel || backup_handle->stop)
        {
            err = true;
            break;
        }

        if (thread_state == THREAD_STATE_EXIT)
        {
            err = false;                     /* clean end */
            break;
        }

        if (thread_state == THREAD_STATE_EXIT_WITH_ERROR)
        {
            err = true;                      /* backupdb crashed/failed */
            break;
        }

        if (++tries > 200)                   /* ~2s fail-safe if state never publishes */
        {
            err = true;
            break;
        }

        {
            struct pollfd cancel_poll;
            cancel_poll.fd = backup_handle->cancel_efd;
            cancel_poll.events = POLLIN;
            cancel_poll.revents = 0;
            poll (&cancel_poll, 1, 10);
            if (cancel_poll.revents != 0)    /* cancel arrived while waiting */
            {
                err = true;
                break;
            }
        }
    }

    pthread_mutex_lock (&backup_handle->buf_lock);
    if (err)
    {
        backup_handle->buf_error = true;
    }
    else
    {
        backup_handle->producer_eof = true;
    }
    pthread_cond_broadcast (&backup_handle->not_empty);
    pthread_mutex_unlock (&backup_handle->buf_lock);
}

/* ── observational log-phase parser (DRAIN THREAD ONLY) ───────────────────────
 * Peeks the backup byte-stream framing to detect entry into the log-copy phase
 * (the section the server writes while holding LOG_CS). It NEVER alters, drops,
 * reorders, or gates which bytes get committed — its ONLY output is h->log_phase,
 * which selects the tiered buffer's use_disk policy. Any parse anomaly disables
 * it → single-mode fallback (worst case == pre-feature behaviour; a desync can
 * never corrupt the byte stream). */

static
void parser_disable (BACKUP_HANDLE* backup_handle, const char* reason)
{
    if (backup_handle->parser.state == PS_DISABLED)
    {
        return;
    }

    backup_handle->parser.state = PS_DISABLED;

    if (backup_handle->parser.global_header != NULL)
    {
        free (backup_handle->parser.global_header);
        backup_handle->parser.global_header = NULL;
    }

    /* parser_on drives the (drain-local) tier decision; mirror under buf_lock so
     * a concurrent reader/logger observes a consistent value. */
    pthread_mutex_lock (&backup_handle->buf_lock);
    backup_handle->parser_on = false;
    pthread_mutex_unlock (&backup_handle->buf_lock);

    PRINT_LOG_WARN ("log-phase parser disabled (%s); tiered buffer degraded to single-mode\n", reason);
}

/* Arm the reserved spool (drain-local log_phase, mirrored to phase_published
 * under buf_lock for observability).
 * Boundary = first post-data negative-volid FILE_START = first needed archive
 * (-20), copied outside the harmful LOG_CS window. Non-contractual: relies on
 * server stream order, not an API guarantee. Sizing: buffer_disk_limit must cover
 * the whole post-data log backlog (archives+info+active) — undersizing fills the
 * reserve during the harmless archive copy, then blocks on active log. */
static
void enter_log_phase (BACKUP_HANDLE* backup_handle)
{
    backup_handle->log_phase = true;

    pthread_mutex_lock (&backup_handle->buf_lock);
    backup_handle->phase_published = true;
    pthread_mutex_unlock (&backup_handle->buf_lock);

    PRINT_LOG_INFO ("log-phase boundary detected (~%lld bytes in); reserved spool armed\n",
                    (long long) backup_handle->bytes_total);
}

/* Gate the accumulated global header: accept ONLY {START marker, magic v2,
 * version 2, LZ4} — the single walkable combination. */
static
int gate_ok (BACKUP_HANDLE* backup_handle)
{
    CUB_BKUP_HEADER* header = (CUB_BKUP_HEADER *) backup_handle->parser.global_header;

    if (header->iopageid != CUB_BK_START_PAGE_ID)
    {
        return 0;
    }
    if (memcmp (header->magic, CUB_BK_MAGIC, sizeof (CUB_BK_MAGIC) - 1) != 0)
    {
        return 0;
    }
    if (header->bk_hdr_version != CUB_BK_HDR_VERSION)
    {
        return 0;
    }
    if (header->zip_method == CUB_BK_ZIP_LZ4)          /* self-delimiting [buf_len][payload] */
    {
        backup_handle->parser.compressed = 1;
    }
    else if (header->zip_method == CUB_BK_ZIP_NONE)    /* fixed-stride bkpagesize+OVERHEAD units */
    {
        backup_handle->parser.compressed = 0;
    }
    else                                               /* ZLIB(2)/LZO1X(1): unsupported framing */
    {
        return 0;
    }
    if (header->bkpagesize <= 0 || header->bkpagesize > CUB_BK_MAX_BKPAGESIZE)
    {
        return 0;
    }

    backup_handle->parser.backup_page_size = header->bkpagesize;
    return 1;
}

/* volid whitelist: a data volume [0, MAX] or a known system negative volid.
 * Floor is DWB(-22) for robustness (the log/archive floor proper is -20). */
static
int volid_valid (int volid)
{
    if (volid >= 0 && volid <= CUB_LOG_MAX_DBVOLID)
    {
        return 1;
    }
    return (volid <= CUB_BK_START_PAGE_ID && volid >= CUB_LOG_DWB_VOLID);   /* -2 .. -22 */
}

/* Consume the bytes just read from the FIFO, [buf, buf+n), advancing the framing
 * walker across ANY read-chunk boundary (fields/skips persist across calls). The
 * only bytes ever inspected are the one-time global header plus ≤18 B per file
 * (tag + nbytes + volid); every compressed page is skipped by its self-declared
 * length without being read or decompressed. */
static
void parser_observe (BACKUP_HANDLE* backup_handle, const char* data, size_t chunk_len)
{
    BK_PARSER* parser = &backup_handle->parser;
    size_t offset = 0;

    while (offset < chunk_len)
    {
        size_t avail_bytes = chunk_len - offset;

        switch (parser->state)
        {
            case PS_DONE:            /* log phase latched: the tail is all log */
            case PS_DISABLED:        /* fallback policy in force               */
                return;

            case PS_GATE:            /* accumulate the global header, then gate */
            {
                size_t take_bytes = (avail_bytes < parser->need_bytes) ? avail_bytes : parser->need_bytes;
                memcpy (parser->global_header + parser->got_bytes, data + offset, take_bytes);
                parser->got_bytes  += take_bytes;
                parser->need_bytes -= take_bytes;
                offset             += take_bytes;
                if (parser->need_bytes == 0)
                {
                    if (!gate_ok (backup_handle))
                    {
                        parser_disable (backup_handle, "gate");
                        return;
                    }
                    free (parser->global_header);
                    parser->global_header = NULL;
                    parser->skip_bytes    = (long long) CUB_BK_HEADER_IO_SIZE - (long long) CUB_BK_HEADER_STRUCT;
                    parser->state         = PS_SKIP;   /* consume header padding, then walk tags */
                }
                break;
            }

            case PS_SKIP:            /* discard payload / unit-tail / header pad */
            {
                size_t take_bytes = (avail_bytes < (size_t) parser->skip_bytes) ? avail_bytes : (size_t) parser->skip_bytes;
                parser->skip_bytes -= (long long) take_bytes;
                offset             += take_bytes;
                if (parser->skip_bytes == 0)
                {
                    parser->state      = PS_TAG;
                    parser->got_bytes  = 0;
                    parser->need_bytes = CUB_BK_TAG_SIZE;
                }
                break;
            }

            case PS_TAG:             /* peek the 4-byte frame lead */
            {
                int32_t tag;
                size_t take_bytes = (avail_bytes < parser->need_bytes) ? avail_bytes : parser->need_bytes;
                memcpy (parser->header_accum + parser->got_bytes, data + offset, take_bytes);
                parser->got_bytes  += take_bytes;
                parser->need_bytes -= take_bytes;
                offset             += take_bytes;
                if (parser->need_bytes != 0)
                {
                    break;           /* tag straddles reads; resume next call */
                }
                memcpy (&tag, parser->header_accum, CUB_BK_TAG_SIZE);   /* native int; same-host pipe */
                if (tag == CUB_BK_FILE_START_PAGE_ID)     /* -4 FILE_START: same 4120B unit in both modes */
                {
                    parser->got_bytes  = CUB_BK_TAG_SIZE; /* keep tag; gather through volid */
                    parser->need_bytes = CUB_BK_FS_PEEK - CUB_BK_TAG_SIZE;
                    parser->state      = PS_FS_HDR;
                }
                else if (parser->compressed)              /* LZ4: tag>=0 is buf_len of [buf_len][payload] */
                {
                    if (tag < 0 || tag == 0 || tag > parser->backup_page_size + CUB_BK_PAGE_OVERHEAD)
                    {                                     /* -2/-3/-5/-6 never appear raw mid-LZ4-stream */
                        parser_disable (backup_handle, (tag < 0) ? "unexpected-neg-tag" : "buf_len");
                        return;
                    }
                    parser->skip_bytes = tag;
                    parser->state      = PS_SKIP;         /* skip payload without reading it */
                }
                else                                      /* NONE: fixed-stride bkpagesize+OVERHEAD units */
                {
                    if (tag >= 0 || tag == CUB_BK_FILE_END_PAGE_ID)   /* data page (pageid>=0) or FILE_END(-5) */
                    {
                        parser->skip_bytes = (long long) parser->backup_page_size
                                             + CUB_BK_PAGE_OVERHEAD - CUB_BK_TAG_SIZE;
                        parser->state      = PS_SKIP;     /* stride past the rest of the page-sized unit */
                    }
                    else                                  /* -2/-3/-6 not expected before the boundary */
                    {
                        parser_disable (backup_handle, "unexpected-neg-tag");
                        return;
                    }
                }
                break;
            }

            case PS_FS_HDR:          /* accumulate through volid, then classify */
            {
                int64_t nbytes;
                int16_t volid;
                size_t take_bytes = (avail_bytes < parser->need_bytes) ? avail_bytes : parser->need_bytes;
                memcpy (parser->header_accum + parser->got_bytes, data + offset, take_bytes);
                parser->got_bytes  += take_bytes;
                parser->need_bytes -= take_bytes;
                offset             += take_bytes;
                if (parser->need_bytes != 0)
                {
                    break;           /* header straddles reads; resume next call */
                }
                memcpy (&nbytes, parser->header_accum + CUB_BK_WIRE_NBYTES_OFF, 8);
                memcpy (&volid,  parser->header_accum + CUB_BK_WIRE_VOLID_OFF,  2);
                if (nbytes < 0 || !volid_valid (volid))
                {
                    parser_disable (backup_handle, "fs-hdr");
                    return;
                }
                if (volid >= 0)
                {
                    parser->saw_data_volume = 1;          /* a data volume has streamed */
                }
                else if (parser->saw_data_volume)
                {
                    enter_log_phase (backup_handle);      /* boundary: first neg volid after a data volume */
                    parser->state = PS_DONE;
                    return;
                }
                /* pre-data negative (TDE/volinfo) or a data volume: skip the rest of
                 * this FILE_START unit and resume tag-walking. */
                parser->skip_bytes = (long long) CUB_BK_FILE_UNIT - (long long) CUB_BK_FS_PEEK;
                parser->state      = PS_SKIP;
                break;
            }
        }
    }
}

/* Drain thread: continuously read the FIFO into the tiered buffer so the pipe
 * stays empty and backupdb never blocks/spins in the LOG_CS window. */
static
void* drain_backup_fifo (void* arg)
{
    BACKUP_HANDLE* backup_handle = (BACKUP_HANDLE *)arg;

    sigset_t block_set;
    struct pollfd poll_fds[2];
    char* scratch_buf = NULL;
    size_t io_size;
    size_t scratch_cap;       /* scratch size: max(io_size, pipe capacity)       */
    int    probe_usable;      /* data-phase boundary probe is usable this run     */
    size_t carry_offset = 0;  /* pending data-phase probe chunk held in scratch_buf */
    size_t carry_length = 0;

    /* Keep SIGCHLD steered to backup_thread's sigtimedwait(); if delivered here
     * the child would never be reaped and EOF never detected. */
    sigemptyset (&block_set);
    sigaddset (&block_set, SIGCHLD);
    pthread_sigmask (SIG_BLOCK, &block_set, NULL);

    /* disk-tier scratch. Sized to the LARGER of io_size and the kernel pipe
     * capacity, so a single boundary "probe" read can span the whole pipe
     * backlog and always capture the log-phase FILE_START that backupdb has
     * committed while blocked under LOG_CS. */
    io_size     = (size_t) backup_mgr->io_size;
    scratch_cap = io_size;
    {
        int pipe_cap = fcntl (backup_handle->fifo_fd, F_GETPIPE_SZ);
        long long desired_cap = (pipe_cap > 0)
                         ? (long long) pipe_cap
                         : (long long) backup_mgr->default_backup_option.fifo_size;
        if (desired_cap > (long long) scratch_cap)
        {
            scratch_cap = (size_t) desired_cap;
        }
    }
    if (backup_handle->disk_cap > 0)
    {
        scratch_buf = malloc (scratch_cap);
        if (scratch_buf == NULL)
        {
            /* Degrade to memory-only instead of failing the whole backup. The
             * scratch is only used for disk staging + the boundary probe, both
             * gated on disk_cap; zeroing it makes probe_ok false below, which
             * disables the parser via the existing guard. */
            PRINT_LOG_WARN ("drain scratch alloc failed; disk tier disabled (memory-only)\n");
            pthread_mutex_lock (&backup_handle->buf_lock);
            backup_handle->disk_cap = 0;
            pthread_mutex_unlock (&backup_handle->buf_lock);
        }
    }

    /* The probe commits its (possibly pipe-sized) chunk to the reserved spool in
     * one shot, so it is only usable when the reserve can hold it. This is a
     * static capacity test; live parser_on is re-checked at the probe guard so a
     * mid-run parser_disable cleanly reverts to single-mode. */
    probe_usable = (backup_handle->disk_cap >= (long long) scratch_cap);

    /* If the reserve is smaller than one pipe's worth, the probe can never commit
     * a boundary chunk and would withhold the reserve without ever detecting the
     * boundary (worse than single-mode). Revert to single-mode fallback (spill_ok
     * becomes true; the reserve is used as plain overflow). */
    if (backup_handle->parser_on && !probe_usable)
    {
        parser_disable (backup_handle, "reserve-smaller-than-pipe");
    }

    for (;;)
    {
        char*  read_dst;
        size_t read_room, mem_tail_offset, contig_run, mem_free_bytes;
        ssize_t read_len;
        int    use_disk;
        int    is_probe = 0;
        long long disk_tail_offset = 0, disk_room = 0;

        /* Flush a pending data-phase probe chunk into the memory ring before any
         * new read. These bytes are older than anything read next (order intact)
         * and never touch the reserved disk. */
        if (carry_length > 0)
        {
            size_t put_len;

            pthread_mutex_lock (&backup_handle->buf_lock);
            while (mem_ring_free (backup_handle) == 0 && !backup_handle->stop)
            {
                backup_handle->wait_count++;
                pthread_cond_wait (&backup_handle->not_full, &backup_handle->buf_lock);
            }
            if (backup_handle->stop)
            {
                pthread_mutex_unlock (&backup_handle->buf_lock);
                break;
            }
            put_len = mem_ring_put (backup_handle, scratch_buf + carry_offset, carry_length);
            carry_offset += put_len;
            carry_length -= put_len;
            backup_handle->bytes_total += (long long) put_len;
            if (backup_handle->mem_len > backup_handle->mem_high_water)
            {
                backup_handle->mem_high_water = backup_handle->mem_len;
            }
            pthread_cond_signal (&backup_handle->not_empty);
            pthread_mutex_unlock (&backup_handle->buf_lock);
            continue;                            /* keep flushing until carry drained */
        }

        poll_fds[0].fd = backup_handle->fifo_fd;    poll_fds[0].events = POLLIN; poll_fds[0].revents = 0;
        poll_fds[1].fd = backup_handle->cancel_efd; poll_fds[1].events = POLLIN; poll_fds[1].revents = 0;

        if (poll (poll_fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            buf_mark_error (backup_handle);
            break;
        }

        if (poll_fds[1].revents != 0 || backup_handle->stop)        /* cancel / shutdown */
        {
            break;
        }

        pthread_mutex_lock (&backup_handle->buf_lock);

        /* Choose the tier (order-preserving: once spilling, or mem full, new bytes
         * go to disk — they are newer than all mem data) and WAIT until THAT tier
         * has room. Waiting on "both full" would wrongly proceed when the chosen
         * tier (disk, while spilling) is full but the other (mem) has space,
         * yielding a 0-byte read that looks like EOF. */
        for (;;)
        {
            int spill_ok;

            if (backup_handle->stop)
            {
                break;
            }

            /* 2-mode reserved spool: in the DATA phase (parser on, log phase not
             * yet reached) the disk reserve is withheld — mem-only, then WAIT
             * (harmless backpressure; LOG_CS not held). Once the LOG phase is
             * reached (or the parser is off = single-mode fallback) the reserve is
             * armed. */
            spill_ok = (!backup_handle->parser_on) || backup_handle->log_phase;

            use_disk = (backup_handle->disk_cap > 0)
                       && (backup_handle->disk_len > 0 || (mem_ring_free (backup_handle) == 0 && spill_ok));

            if (use_disk ? (disk_ring_free (backup_handle) > 0) : (mem_ring_free (backup_handle) > 0))  /* Tier-3 WAIT */
            {
                break;
            }

            /* DATA phase, mem full, reserve still empty: rather than park (which
             * would strand the boundary FILE_START in a full pipe while backupdb
             * blocks under LOG_CS), probe one pipe-sized chunk into the scratch to
             * reach and classify the boundary WITHOUT waiting for the reader. */
            if (backup_handle->parser_on && probe_usable && !backup_handle->log_phase
                && backup_handle->disk_len == 0 && carry_length == 0)
            {
                is_probe = 1;
                break;
            }

            backup_handle->wait_count++;
            pthread_cond_wait (&backup_handle->not_full, &backup_handle->buf_lock);
        }

        if (backup_handle->stop)
        {
            pthread_mutex_unlock (&backup_handle->buf_lock);
            break;
        }

        if (is_probe)
        {
            read_dst  = scratch_buf;                         /* span the pipe backlog */
            read_room = scratch_cap;
            backup_handle->lookahead_count++;
        }
        else if (use_disk)
        {
            disk_tail_offset = (backup_handle->disk_head + backup_handle->disk_len) % backup_handle->disk_cap;
            disk_room = backup_handle->disk_cap - disk_tail_offset;   /* contiguous run to file end */
            if (disk_room > backup_handle->disk_cap - backup_handle->disk_len)   /* bounded by total free */
            {
                disk_room = backup_handle->disk_cap - backup_handle->disk_len;
            }
            if (disk_room > (long long) io_size)             /* one chunk at a time */
            {
                disk_room = (long long) io_size;
            }
            read_dst  = scratch_buf;                         /* stage in private scratch */
            read_room = (size_t) disk_room;
        }
        else
        {
            /* reserve the contiguous free run [tail..end); drain owns it until commit */
            mem_tail_offset = (backup_handle->mem_head + backup_handle->mem_len) % backup_handle->mem_cap;
            contig_run      = backup_handle->mem_cap - mem_tail_offset;
            mem_free_bytes  = backup_handle->mem_cap - backup_handle->mem_len;
            read_room       = (contig_run < mem_free_bytes) ? contig_run : mem_free_bytes;
            read_dst        = backup_handle->mem_buf + mem_tail_offset;   /* read straight into the ring */
        }

        pthread_mutex_unlock (&backup_handle->buf_lock);

        read_len = read (backup_handle->fifo_fd, read_dst, read_room);   /* lock-free: target is drain-private */

        if (read_len < 0)
        {
            is_probe = 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                continue;
            }
            buf_mark_error (backup_handle);
            break;
        }

        if (read_len == 0)
        {
            /* EOF from kernel. Spurious if backupdb has not opened the write end
             * yet (state still RUNNING/NO_SPAWN) — wait briefly and retry. */
            THREAD_STATE thread_state = backup_handle->backup_thread_state;

            is_probe = 0;

            if (thread_state == THREAD_STATE_EXIT || thread_state == THREAD_STATE_EXIT_WITH_ERROR || backup_handle->is_cancel)
            {
                drain_classify_eof (backup_handle);
                break;
            }

            {
                struct pollfd cancel_poll;
                cancel_poll.fd = backup_handle->cancel_efd;
                cancel_poll.events = POLLIN;
                cancel_poll.revents = 0;
                poll (&cancel_poll, 1, 20);          /* avoid busy-spin before writer connects */
            }

            if (backup_handle->stop)
            {
                break;
            }

            continue;
        }

        /* Peek the bytes just read (read-only; never mutated/dropped/reordered).
         * Runs before the commit bumps mem_len, so peeking mem_buf+tail races
         * nothing — the reader cannot see [tail..) yet. */
        if (backup_handle->parser_on)
        {
            parser_observe (backup_handle, read_dst, (size_t) read_len);
        }

        if (is_probe)
        {
            is_probe = 0;

            if (backup_handle->log_phase)
            {
                /* Boundary caught in this chunk. It is the FIRST disk write (probe
                 * fires only when disk_len==0) so disk_tail_offset==0 and the whole
                 * chunk (trailing data + boundary + leading log) fits contiguously
                 * (read_len <= scratch_cap <= disk_cap via probe_usable). Spill it
                 * in order. */
                ssize_t write_offset = 0;

                pthread_mutex_lock (&backup_handle->buf_lock);
                disk_tail_offset = (backup_handle->disk_head + backup_handle->disk_len) % backup_handle->disk_cap;   /* == 0 */
                pthread_mutex_unlock (&backup_handle->buf_lock);

                while (write_offset < read_len)
                {
                    ssize_t written = pwrite (backup_handle->disk_fd, scratch_buf + write_offset, (size_t) (read_len - write_offset), (off_t) (disk_tail_offset + write_offset));
                    if (written < 0) { if (errno == EINTR) continue; break; }
                    write_offset += written;
                }

                if (write_offset != read_len)
                {
                    buf_mark_error (backup_handle);
                    break;
                }

                pthread_mutex_lock (&backup_handle->buf_lock);
                backup_handle->disk_len += read_len;
                backup_handle->bytes_total += read_len;
                backup_handle->spilled = true;
                if (backup_handle->disk_len > backup_handle->disk_high_water)
                {
                    backup_handle->disk_high_water = backup_handle->disk_len;
                }
                pthread_cond_signal (&backup_handle->not_empty);
                pthread_mutex_unlock (&backup_handle->buf_lock);
            }
            else
            {
                /* Still the data phase: hold this chunk and flush it to the memory
                 * ring next iteration. Data-phase bytes NEVER touch the reserved
                 * disk, keeping the reserve intact. */
                carry_offset = 0;
                carry_length = (size_t) read_len;
            }

            continue;
        }

        if (use_disk)
        {
            ssize_t write_offset = 0;

            /* single region (room <= contiguous-to-end); loop only on partial write.
             * space is pre-reserved at setup, so pwrite does not hit ENOSPC here. */
            while (write_offset < read_len)
            {
                ssize_t written = pwrite (backup_handle->disk_fd, scratch_buf + write_offset, (size_t) (read_len - write_offset), (off_t) (disk_tail_offset + write_offset));
                if (written < 0) { if (errno == EINTR) continue; break; }
                write_offset += written;
            }

            if (write_offset != read_len)
            {
                buf_mark_error (backup_handle);
                break;
            }

            pthread_mutex_lock (&backup_handle->buf_lock);
            backup_handle->disk_len += read_len;
            backup_handle->bytes_total += read_len;
            backup_handle->spilled = true;
            if (backup_handle->disk_len > backup_handle->disk_high_water)
            {
                backup_handle->disk_high_water = backup_handle->disk_len;
            }
            pthread_cond_signal (&backup_handle->not_empty);
            pthread_mutex_unlock (&backup_handle->buf_lock);
        }
        else
        {
            pthread_mutex_lock (&backup_handle->buf_lock);
            backup_handle->mem_len += (size_t) read_len;
            backup_handle->bytes_total += read_len;
            if (backup_handle->mem_len > backup_handle->mem_high_water)
            {
                backup_handle->mem_high_water = backup_handle->mem_len;
            }
            pthread_cond_signal (&backup_handle->not_empty);
            pthread_mutex_unlock (&backup_handle->buf_lock);
        }
    }

    free (scratch_buf);

    /* final wake so a parked reader observes producer_eof/buf_error/stop */
    pthread_mutex_lock (&backup_handle->buf_lock);
    pthread_cond_broadcast (&backup_handle->not_empty);
    pthread_cond_broadcast (&backup_handle->not_full);
    pthread_mutex_unlock (&backup_handle->buf_lock);

    return NULL;
}

/* Reader side: pop oldest-first from the tiered buffer (replaces FIFO-direct
 * read when buffering is enabled). Preserves the is_backup_end/return contract. */
static
int pop_backup_data (BACKUP_HANDLE* backup_handle, char* buffer, unsigned int buffer_size,
                     unsigned int* data_len, bool* is_backup_end)
{
    size_t    mem_head_s, mem_copy, disk_copy, remaining;
    long long disk_head_s;

    pthread_mutex_lock (&backup_handle->buf_lock);

    while (backup_handle->mem_len == 0 && backup_handle->disk_len == 0 && !backup_handle->producer_eof && !backup_handle->buf_error)
    {
        pthread_cond_wait (&backup_handle->not_empty, &backup_handle->buf_lock);
    }

    if (backup_handle->buf_error)
    {
        pthread_mutex_unlock (&backup_handle->buf_lock);
        PRINT_LOG_ERR (ERR_INFO);
        return FAILURE;
    }

    if (backup_handle->mem_len == 0 && backup_handle->disk_len == 0 && backup_handle->producer_eof)
    {
        *is_backup_end = true;
        *data_len = 0;
        pthread_mutex_unlock (&backup_handle->buf_lock);
        return SUCCESS;
    }

    /* Plan the copy under the lock (mem is older -> drained first), then release
     * the lock and do the memcpy / spool pread WITHOUT it. In this single-consumer
     * ring the committed region [head..head+len) is never overwritten by the drain
     * (it writes only free space), so copying it lock-free is safe. This keeps the
     * drain emptying the FIFO instead of stalling on a slow-spool pread. */
    mem_head_s = backup_handle->mem_head;
    mem_copy   = (buffer_size < backup_handle->mem_len) ? buffer_size : backup_handle->mem_len;

    remaining   = buffer_size - mem_copy;
    disk_head_s = backup_handle->disk_head;
    disk_copy   = 0;
    if (remaining > 0 && backup_handle->disk_len > 0)            /* then newer disk data */
    {
        disk_copy = (remaining < (size_t) backup_handle->disk_len) ? remaining : (size_t) backup_handle->disk_len;
    }

    pthread_mutex_unlock (&backup_handle->buf_lock);

    /* lock-free copy of the planned bytes (regions are drain-immutable) */
    mem_ring_copy (backup_handle, buffer, mem_head_s, mem_copy);

    if (disk_copy > 0 && disk_ring_read (backup_handle, buffer + mem_copy, disk_head_s, disk_copy) < 0)
    {
        PRINT_LOG_ERR (ERR_INFO);
        return FAILURE;                                          /* nothing consumed yet; backup aborts */
    }

    /* commit: advance head/len and wake the drain */
    pthread_mutex_lock (&backup_handle->buf_lock);
    backup_handle->mem_head = (mem_head_s + mem_copy) % backup_handle->mem_cap;
    backup_handle->mem_len -= mem_copy;
    if (disk_copy > 0)                                           /* guard % against disk_cap==0 (mem-only) */
    {
        backup_handle->disk_head = (disk_head_s + (long long) disk_copy) % backup_handle->disk_cap;
        backup_handle->disk_len -= (long long) disk_copy;
    }
    pthread_cond_signal (&backup_handle->not_full);
    pthread_mutex_unlock (&backup_handle->buf_lock);

    *data_len = (unsigned int) (mem_copy + disk_copy);

    return SUCCESS;
}

int begin_backup (CUBRID_BACKUP_INFO* backup_info, void** handle)
{
    BACKUP_HANDLE* backup_handle;

    int state = 0;

    if (IS_NULL (backup_info) || IS_NULL (handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (alloc_handle (BACKUP_HANDLE_TYPE, (void **)&backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (IS_FAILURE (set_backup_info (backup_info, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (open_fifo (BACKUP_HANDLE_TYPE, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 2;

    set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_RUNNING);

    if (IS_FAILURE (pthread_create (&backup_handle->backup_thread, NULL, execute_backup, (void *)backup_handle)))
    {
        set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_EXIT_WITH_ERROR);

        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* ── tiered buffer setup (best-effort: on any failure degrade to the legacy
     * direct-FIFO read path via buffering_enabled=false — never fail begin) ── */
    if (backup_mgr->default_backup_option.buffer_memory_size > 0)
    {
        backup_handle->mem_cap = (size_t) backup_mgr->default_backup_option.buffer_memory_size;
        backup_handle->mem_buf = malloc (backup_handle->mem_cap);

        if (backup_handle->mem_buf == NULL)
        {
            PRINT_LOG_WARN ("buffer_memory_size alloc failed; buffering disabled\n");
            backup_handle->mem_cap = 0;
        }
        else
        {
            backup_handle->cancel_efd = eventfd (0, EFD_NONBLOCK | EFD_CLOEXEC);

            if (backup_handle->cancel_efd == -1)
            {
                PRINT_LOG_WARN ("eventfd failed; buffering disabled\n");
                free (backup_handle->mem_buf);
                backup_handle->mem_buf = NULL;
                backup_handle->mem_cap = 0;
            }
            else
            {
                sigset_t block_set, old_set;
                int rc;

                /* optional disk tier: create + reserve the spool BEFORE the drain
                 * starts (the drain writes disk_fd). Failure degrades to memory-only
                 * — it never fails begin. */
                if (backup_mgr->default_backup_option.buffer_disk_limit > 0)
                {
                    if (IS_FAILURE (create_spool_file (backup_handle)))
                    {
                        PRINT_LOG_WARN ("spool setup failed; disk tier disabled (memory-only)\n");
                        backup_handle->disk_fd  = -1;
                        backup_handle->disk_cap = 0;
                    }
                }

                /* Arm the observational log-phase parser — only when a disk reserve
                 * exists (memory-only has nothing to reserve, so the parser would be
                 * inert). MUST be set up BEFORE the drain thread starts, since the
                 * drain reads parser state. Alloc failure degrades to single-mode. */
                if (backup_handle->disk_cap > 0)
                {
                    backup_handle->parser.global_header = malloc ((size_t) CUB_BK_HEADER_STRUCT);

                    if (backup_handle->parser.global_header == NULL)
                    {
                        PRINT_LOG_WARN ("parser header alloc failed; log-phase detection disabled\n");
                        backup_handle->parser.state = PS_DISABLED;
                        backup_handle->parser_on = false;
                    }
                    else
                    {
                        backup_handle->parser.state            = PS_GATE;
                        backup_handle->parser.need_bytes       = (size_t) CUB_BK_HEADER_STRUCT;
                        backup_handle->parser.got_bytes        = 0;
                        backup_handle->parser.skip_bytes       = 0;
                        backup_handle->parser.saw_data_volume  = 0;
                        backup_handle->parser.backup_page_size = 0;
                        backup_handle->parser.compressed       = 0;
                        backup_handle->parser_on               = true;
                    }
                }

                /* start the drain with SIGCHLD blocked so it never steals the
                 * child-exit signal from backup_thread's sigtimedwait(). */
                sigemptyset (&block_set);
                sigaddset (&block_set, SIGCHLD);
                pthread_sigmask (SIG_BLOCK, &block_set, &old_set);

                rc = pthread_create (&backup_handle->drain_thread, NULL,
                                     drain_backup_fifo, (void *)backup_handle);

                pthread_sigmask (SIG_SETMASK, &old_set, NULL);

                if (IS_FAILURE (rc))
                {
                    PRINT_LOG_WARN ("drain thread create failed; buffering disabled\n");
                    close (backup_handle->cancel_efd);
                    backup_handle->cancel_efd = -1;
                    free (backup_handle->mem_buf);
                    backup_handle->mem_buf = NULL;
                    backup_handle->mem_cap = 0;

                    /* no drain to run the parser; drop its buffer now (finalize
                     * also guards this) and disable it. */
                    if (backup_handle->parser.global_header != NULL)
                    {
                        free (backup_handle->parser.global_header);
                        backup_handle->parser.global_header = NULL;
                    }
                    backup_handle->parser.state = PS_DISABLED;
                    backup_handle->parser_on = false;
                }
                else
                {
                    backup_handle->drain_started     = true;
                    backup_handle->buffering_enabled = true;
                }
            }
        }
    }

    if (IS_FAILURE (pthread_mutex_unlock (&backup_handle->backup_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    *(BACKUP_HANDLE **)handle = backup_handle;

    return SUCCESS;

error:

    switch (state)
    {
        case 2:
            backup_handle->is_cancel = true;
            pthread_join (backup_handle->backup_thread, NULL);
            close_fifo (BACKUP_HANDLE_TYPE, backup_handle);
        case 1:
            free_handle (BACKUP_HANDLE_TYPE, backup_handle);
        default:
            break;
    }

    return FAILURE;
}

int end_backup (BACKUP_HANDLE* backup_handle)
{
    int state = 0;
    int close_rc;

    if (IS_NULL (backup_handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    // Perform handle validation before acquiring the mutex.
    // Reason: passing a garbage (non-NULL) value as the handle can cause
    // 1. hang
    // 2. seg fault
    if (IS_FAILURE (validate_handle (BACKUP_HANDLE_TYPE, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* Signal the drain/reader BEFORE taking backup_mutex: a reader parked in
     * pop_backup_data() holds backup_mutex across its cond_wait, so waking it
     * via buf_lock first is what lets the lock below be acquired (no deadlock). */
    if (backup_handle->buffering_enabled)
    {
        pthread_mutex_lock (&backup_handle->buf_lock);
        backup_handle->stop = true;
        if (!backup_handle->producer_eof)
        {
            backup_handle->buf_error = true;   /* premature end = truncated backup */
        }
        pthread_cond_broadcast (&backup_handle->not_empty);
        pthread_cond_broadcast (&backup_handle->not_full);
        pthread_mutex_unlock (&backup_handle->buf_lock);

        if (backup_handle->cancel_efd != -1)
        {
            uint64_t one = 1;
            (void) write (backup_handle->cancel_efd, &one, sizeof (one));
        }
    }

    if (IS_FAILURE (pthread_mutex_lock (&backup_handle->backup_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (backup_handle->backup_thread_state == THREAD_STATE_RUNNING)
    {
        backup_handle->is_cancel = true;

        if (IS_FAILURE (pthread_join (backup_handle->backup_thread, NULL)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    /* Join the drain BEFORE close_fifo() (the drain reads fifo_fd). It was
     * already signalled to stop above, so this returns promptly. */
    if (backup_handle->drain_started)
    {
        pthread_join (backup_handle->drain_thread, NULL);
        backup_handle->drain_started = false;
    }

    if (backup_handle->buffering_enabled)
    {
        PRINT_LOG_INFO ("tiered buffer summary: log_phase=%d spilled=%d mem_high_water=%zu "
                        "disk_high_water=%lld wait_count=%lu lookahead=%lu bytes_total=%lld\n",
                        (int) backup_handle->phase_published, (int) backup_handle->spilled,
                        backup_handle->mem_high_water, backup_handle->disk_high_water,
                        backup_handle->wait_count, backup_handle->lookahead_count,
                        backup_handle->bytes_total);
    }

    /* Even if close_fifo fails (e.g. remove_fifo/unlink error), still run
     * free_handle below so the tiered-buffer resources on this reused static
     * handle are released. close_fifo already reset fifo_fd to -1, so finalize
     * will not double-close it. */
    close_rc = close_fifo (BACKUP_HANDLE_TYPE, backup_handle);
    if (IS_FAILURE (close_rc))
    {
        PRINT_LOG_ERR (ERR_INFO);
    }

    if (IS_FAILURE (free_handle (BACKUP_HANDLE_TYPE, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_unlock (&backup_handle->backup_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return IS_FAILURE (close_rc) ? FAILURE : SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&backup_handle->backup_mutex);
        default:
            break;
    }

    return FAILURE;
}

static
int read_fifo (int fifo_fd, int io_size, char* buffer, int* read_len)
{
    int retval;
    int read_size;

    fd_set read_fds;

    FD_ZERO (&read_fds);
    FD_SET (fifo_fd, &read_fds);

    struct timeval wait_timeout;

    wait_timeout.tv_sec  = 2;
    wait_timeout.tv_usec = 0;

    retval = select (fifo_fd + 1, &read_fds, NULL, NULL, &wait_timeout);

    if (retval == -1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else if (retval == 0)
    {
        *read_len = 0;
    }
    else
    {
        if (FD_ISSET (fifo_fd, &read_fds))
        {
            read_size = read (fifo_fd, buffer, io_size);

            if (read_size == -1)
            {
                PRINT_LOG_ERR (ERR_INFO);
                goto error;
            }

            *read_len = read_size;
        }
        else
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int read_data (BACKUP_HANDLE* backup_handle, char* buffer, unsigned int buffer_size, unsigned int* data_len, bool* is_backup_end)
{
    int io_size = 0;
    int read_len = 0;
    int total_read_len = 0;
    int read_count = 0;
    int i;

    /* buffering active: pop from the tiered buffer instead of the FIFO directly.
     * When disabled (conf =0, or begin-time degrade) the legacy path below runs. */
    if (backup_handle->buffering_enabled)
    {
        return pop_backup_data (backup_handle, buffer, buffer_size, data_len, is_backup_end);
    }

    if (backup_handle->fifo_fd == -1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    io_size = backup_mgr->io_size;

    read_count = buffer_size / io_size;

    for (i = 0; i < read_count; i ++)
    {
        if (backup_handle->backup_thread_state == THREAD_STATE_EXIT_WITH_ERROR)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (read_fifo (backup_handle->fifo_fd, io_size, buffer + total_read_len, &read_len)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        total_read_len += read_len;

        if (total_read_len == 0)
        {
            if (backup_handle->backup_thread_state == THREAD_STATE_EXIT)
            {
                *is_backup_end = true;

                break;
            }
        }
    }

    io_size = buffer_size % io_size;

    if (io_size != 0 && *is_backup_end != true) // refactoring
    {
        if (backup_handle->backup_thread_state == THREAD_STATE_EXIT_WITH_ERROR)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (read_fifo (backup_handle->fifo_fd, io_size, buffer + total_read_len, &read_len)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        total_read_len += read_len;

        if (total_read_len == 0)
        {
            if (backup_handle->backup_thread_state == THREAD_STATE_EXIT)
            {
                *is_backup_end = true;
            }
        }
    }

    *data_len = total_read_len;

    return SUCCESS;

error:

    return FAILURE;
}

int read_backup_data (BACKUP_HANDLE* backup_handle, void* buffer, unsigned int buffer_size, unsigned int* data_len, bool* is_backup_end)
{
    int state = 0;

    if (IS_NULL (backup_handle) || IS_NULL (buffer) || IS_ZERO (buffer_size) || IS_NULL (data_len))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (validate_handle (BACKUP_HANDLE_TYPE, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&backup_handle->backup_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (IS_FAILURE (read_data (backup_handle, buffer, buffer_size, data_len, is_backup_end)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_unlock (&backup_handle->backup_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&backup_handle->backup_mutex);
        default:
            break;
    }

    return FAILURE;
}


static
int open_restore_file (RESTORE_HANDLE* restore_handle)
{
    char restore_file[PATH_MAX];

    /* ex) demodb_bk0v000 */
    snprintf (restore_file, PATH_MAX, "%s/%s_bk%dv000", restore_handle->backup_file_path,
                                                        restore_handle->db_name,
                                                        restore_handle->backup_level);

    if (IS_FAILURE (check_path_length_limit (restore_file)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

#if 0
    // If the file exists, don't check separately; just overwrite it
    if (IS_SUCCESS (access (restore_file, F_OK)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
#endif

    restore_handle->restore_fd = open (restore_file, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);

    if (restore_handle->restore_fd == -1)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

static
int close_restore_file (RESTORE_HANDLE* restore_handle)
{
    if (restore_handle->restore_fd != -1)
    {
        close (restore_handle->restore_fd);

        restore_handle->restore_fd = -1;

        restore_handle->backup_file_path[0] = '\0';
    }

    return SUCCESS;
}

static
int execute_restore_to_file (RESTORE_HANDLE* restore_handle)
{
    if (IS_FAILURE (open_restore_file (restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

int begin_restore (CUBRID_RESTORE_INFO* restore_info, void** handle)
{
    RESTORE_HANDLE* restore_handle;

    int state = 0;

    if (IS_NULL (restore_info) || IS_NULL (handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (alloc_handle (RESTORE_HANDLE_TYPE, (void **)&restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (IS_FAILURE (set_restore_info (restore_info, restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (restore_info->restore_type == RESTORE_TO_DB)
    {
        /* Not supported yet */
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else if (restore_info->restore_type == RESTORE_TO_FILE)
    {
        if (IS_FAILURE (execute_restore_to_file (restore_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    if (IS_FAILURE (pthread_mutex_unlock (&restore_handle->restore_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    *(RESTORE_HANDLE **)handle = restore_handle;

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            free_handle (RESTORE_HANDLE_TYPE, restore_handle);
        default:
            break;
    }

    return FAILURE;
}

int end_restore (RESTORE_HANDLE* restore_handle)
{
    int state = 0;

    if (IS_NULL (restore_handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (validate_handle (RESTORE_HANDLE_TYPE, restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&restore_handle->restore_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (restore_handle->restore_type == RESTORE_TO_DB)
    {
        /* Not supported yet */
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else if (restore_handle->restore_type == RESTORE_TO_FILE)
    {
        close_restore_file (restore_handle);
    }

    if (IS_FAILURE (free_handle (RESTORE_HANDLE_TYPE, restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&restore_handle->restore_mutex);
        default:
            break;
    }

    return FAILURE;
}

static
int write_data_to_file (RESTORE_HANDLE* restore_handle, int backup_level, void* buffer, unsigned int data_len)
{
    int retval;

    if (restore_handle->backup_level != backup_level)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    retval = write (restore_handle->restore_fd, buffer, data_len);

    if (retval != data_len)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

int write_backup_data (RESTORE_HANDLE* restore_handle, int backup_level, void* buffer, unsigned int data_len)
{
    int state = 0;

    if (IS_NULL (restore_handle) || IS_NULL (buffer))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (validate_handle (RESTORE_HANDLE_TYPE, restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_lock (&restore_handle->restore_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    state = 1;

    if (backup_level < BACKUP_FULL_LEVEL ||
        backup_level > BACKUP_SMALL_INCREMENT_LEVEL)
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (restore_handle->restore_type == RESTORE_TO_DB)
    {
        /* Not supported yet */
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }
    else if (restore_handle->restore_type == RESTORE_TO_FILE)
    {
        if (IS_FAILURE (write_data_to_file (restore_handle, backup_level, buffer, data_len)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    if (IS_FAILURE (pthread_mutex_unlock (&restore_handle->restore_mutex)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            pthread_mutex_unlock (&restore_handle->restore_mutex);
        default:
            break;
    }

    return FAILURE;
}
