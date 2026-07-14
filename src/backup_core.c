#include <sys/wait.h>
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

        backup_handle->fifo_fd = open (backup_handle->fifo_path, O_RDONLY | O_NONBLOCK);

        if (backup_handle->fifo_fd == -1)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        /* Size the kernel pipe buffer (backupdb writes in ~1MB chunks). Applied
         * on the read end before backupdb opens the write end. Best-effort:
         * on failure keep the default size and continue — never fail the backup. */
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

    /* --compress */
    if (backup_handle->compress == true)
    {
        argv[idx ++] = "-z";
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

    // tech 요청으로 cub_admin -> cubrid 로 변경: cubrid_utility.log 에 기록 남기기 위해
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

    // backup-api library 자체는 죽으면 안돼기 때문에
    sigaction (SIGTERM, &act, &old_act);

    killpg (pgid, SIGTERM);

    sigaction (SIGTERM, &old_act, NULL);

#if 0
    // FAILURE: Has been interrupted. 를 cubrid_utility.log 에 남기기 위해
    // SIGINT 를 사용하려고 했으나, cub_admin 에서 SIG_INT 핸들러를 등록해두어서
    // cub_admin 이 바로 죽지 않는다.
    // FAILURE: Has been interrupted. 로그를 남긴다는 것 자체가 정상 종료(예외처리) 했다는 것이다.
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
    printf("자식프로세스 wait 성공 \n");
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

    // SIGCHLD에 대한 빈 핸들러를 등록한 이유는
    // 아래 sigtimedwait () 에서 cubrid의 종료를 감지하지 못하기 때문이다.
    // 따라서 cubrid 유틸은 <defunct> 상태가 되며,
    // cubrid_backup_read () 에서는 백업 쓰레드가 종료되었다는 사실을 모르게 된다.
    // 테스트 용으로 zombie_handler 를 등록했는데,
    // 원하는대로 cubrid 유틸 종료시 SIGCHLD를 던저주며, 이를 catch할 수 있게 됐다.
    // 이유는 아직 못찾았다.
    signal(SIGCHLD, (void *)zombie_handler);

    // 현재 cub_admin 을 직접 fork 하는 방식을 사용 중인데 log를 cubrid 유틸에서 남기기 때문에
    // 기술 본부에서 참조하는 cubrid_utility.log 로그에 백업 관련 기록이 남지 않는다.
    // 따러서 cubrid 를 fork 하는 방식으로 바꾸고 kill 시 다 죽이자 후손까지
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

static
void* execute_backup (void* handle)
{
    BACKUP_HANDLE* backup_handle;
    pid_t backup_pid;

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
    // - thread 종료되었다고 ... backup_process 종료된건 아니다.
    // - 이 thread --fork--> backup process 구조는 검증 후 개선이 필요할 듯 하다.
    //   - 이 구조를 취한건 cubrid backup 유틸리티에서 자신을 실행한 thread id
    //     를 검증하는 부분이 있기 때문이다.
    //   - thread 로 백업 수행(libcubridsa.so 링크) 후 백업 수행 (백업 마다 thread 생성 or 한 쓰레드)
    //     cubrid_backup_finalize () 호출 시 boot_shutdown_client_at_exit () (atexit () 등록 됨)
    //     함수에서 coredump 발생
    //     이유는 백업은 thread 생성해서 수행되고, cubrid_backup_finalize () 호출은 main thread가
    //     호출하기 때문에 두 thread id가 달라서 발생된다고 분석된 상태.
    //     별도의 thread나 process를 생성해야 하는 이유는 사용자 레벨에서의 hang 방지.
    //printf ("must hit here 3\n");
    set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_EXIT);

    pthread_exit (NULL);

error:

    set_thread_state (BACKUP_HANDLE_TYPE, backup_handle, THREAD_STATE_EXIT_WITH_ERROR);

    pthread_exit (NULL);
}

/* ───────────────────────── tiered buffer (Phase 1: memory tier) ───────────────────────── */

static
size_t mem_ring_free (BACKUP_HANDLE* h)
{
    return h->mem_cap - h->mem_len;
}

/* Copy oldest-first out of the memory ring into out[0..cap). Caller holds buf_lock. */
static
size_t mem_ring_pop (BACKUP_HANDLE* h, char* out, size_t cap)
{
    size_t n, first;

    n = (cap < h->mem_len) ? cap : h->mem_len;

    if (n == 0)
    {
        return 0;
    }

    first = h->mem_cap - h->mem_head;   /* contiguous run from head to end */
    if (first > n)
    {
        first = n;
    }

    memcpy (out, h->mem_buf + h->mem_head, first);

    if (n > first)                      /* wrap to buffer start */
    {
        memcpy (out + first, h->mem_buf, n - first);
    }

    h->mem_head = (h->mem_head + n) % h->mem_cap;
    h->mem_len -= n;

    return n;
}

/* Append into the memory ring tail (mirror of mem_ring_pop). Caller holds
 * buf_lock. Writes min(len, free) bytes, wrap-aware, and returns that count.
 * Used only to place a held data-phase probe chunk (§4.4). */
static
size_t mem_ring_put (BACKUP_HANDLE* h, const char* in, size_t len)
{
    size_t n, tail, first;

    n = h->mem_cap - h->mem_len;        /* free */
    if (n > len)
    {
        n = len;
    }
    if (n == 0)
    {
        return 0;
    }

    tail  = (h->mem_head + h->mem_len) % h->mem_cap;
    first = h->mem_cap - tail;          /* contiguous run to end */
    if (first > n)
    {
        first = n;
    }

    memcpy (h->mem_buf + tail, in, first);

    if (n > first)                      /* wrap to buffer start */
    {
        memcpy (h->mem_buf, in + first, n - first);
    }

    h->mem_len += n;

    return n;
}

/* ── disk tier (Phase 2): fixed-size circular spool file ── */

static
long long disk_ring_free (BACKUP_HANDLE* h)
{
    return h->disk_cap - h->disk_len;
}

/* Copy oldest-first out of the disk ring into out[0..cap). Caller holds buf_lock.
 * Returns bytes read, or -1 on I/O error. */
static
ssize_t disk_ring_pop (BACKUP_HANDLE* h, char* out, size_t cap)
{
    size_t n, first, off;

    n = (cap < (size_t) h->disk_len) ? cap : (size_t) h->disk_len;

    if (n == 0)
    {
        return 0;
    }

    first = (size_t) (h->disk_cap - h->disk_head);   /* contiguous run to file end */
    if (first > n)
    {
        first = n;
    }

    off = 0;
    while (off < first)                              /* [head .. end) */
    {
        ssize_t k = pread (h->disk_fd, out + off, first - off, (off_t) (h->disk_head + off));
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) { return -1; }
        off += (size_t) k;
    }

    off = 0;
    while (off < n - first)                          /* wrap: [0 .. n-first) */
    {
        ssize_t k = pread (h->disk_fd, out + first + off, (n - first) - off, (off_t) off);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) { return -1; }
        off += (size_t) k;
    }

    h->disk_head = (h->disk_head + (long long) n) % h->disk_cap;
    h->disk_len -= (long long) n;

    return (ssize_t) n;
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
        char   z[65536];
        long long off = 0;

        memset (z, 0, sizeof (z));

        while (off < size)
        {
            size_t chunk = (size - off < (long long) sizeof (z)) ? (size_t) (size - off) : sizeof (z);
            size_t w = 0;

            while (w < chunk)
            {
                ssize_t k = pwrite (fd, z + w, chunk - w, (off_t) (off + (long long) w));
                if (k < 0) { if (errno == EINTR) continue; return -1; }
                w += (size_t) k;
            }

            off += (long long) chunk;
        }

        return 0;
    }

    return -1;   /* ENOSPC / EIO / other */
}

/* Create the per-(db,level,pid) spool file, unlink-on-open by default, and
 * reserve its space. On success sets h->disk_fd and h->disk_cap. */
static
int create_spool_file (BACKUP_HANDLE* h)
{
    BACKUP_OPTION* opt = &backup_mgr->default_backup_option;

    char path[PATH_MAX];
    int  fd;
    int  flags = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC;

    snprintf (path, PATH_MAX, "%s/cubrid_bkbuf_%s_L%d_%d.spool",
              opt->buffer_disk_path, h->db_name, (int) h->backup_level, (int) getpid ());

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

    h->disk_fd  = fd;
    h->disk_cap = opt->buffer_disk_limit;

    return SUCCESS;

error:

    return FAILURE;
}

/* Mark the buffer path failed and wake both waiters. Caller must NOT hold buf_lock. */
static
void buf_mark_error (BACKUP_HANDLE* h)
{
    pthread_mutex_lock (&h->buf_lock);
    h->buf_error = true;
    pthread_cond_broadcast (&h->not_empty);
    pthread_cond_broadcast (&h->not_full);
    pthread_mutex_unlock (&h->buf_lock);
}

/* Resolve a FIFO read()==0 into normal EOF vs early/abnormal EOF. Keyed on
 * backup_thread_state, but cancel also ends in THREAD_STATE_EXIT, so is_cancel
 * must independently force an error. Caller must NOT hold buf_lock. */
static
void drain_classify_eof (BACKUP_HANDLE* h)
{
    bool err = true;
    int  tries = 0;

    for (;;)
    {
        THREAD_STATE st = h->backup_thread_state;

        if (h->is_cancel || h->stop)
        {
            err = true;
            break;
        }

        if (st == THREAD_STATE_EXIT)
        {
            err = false;                     /* clean end */
            break;
        }

        if (st == THREAD_STATE_EXIT_WITH_ERROR)
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
            struct pollfd p;
            p.fd = h->cancel_efd;
            p.events = POLLIN;
            p.revents = 0;
            poll (&p, 1, 10);
            if (p.revents != 0)              /* cancel arrived while waiting */
            {
                err = true;
                break;
            }
        }
    }

    pthread_mutex_lock (&h->buf_lock);
    if (err)
    {
        h->buf_error = true;
    }
    else
    {
        h->producer_eof = true;
    }
    pthread_cond_broadcast (&h->not_empty);
    pthread_mutex_unlock (&h->buf_lock);
}

/* ── observational log-phase parser (DRAIN THREAD ONLY) ───────────────────────
 * Peeks the backup byte-stream framing to detect entry into the log-copy phase
 * (the section the server writes while holding LOG_CS). It NEVER alters, drops,
 * reorders, or gates which bytes get committed — its ONLY output is h->log_phase,
 * which selects the tiered buffer's use_disk policy. Any parse anomaly disables
 * it → single-mode fallback (worst case == pre-feature behaviour; a desync can
 * never corrupt the byte stream). See log_cs_parser_design.md. */

static
void parser_disable (BACKUP_HANDLE* h, const char* why)
{
    if (h->parser.st == PS_DISABLED)
    {
        return;
    }

    h->parser.st = PS_DISABLED;

    if (h->parser.gh != NULL)
    {
        free (h->parser.gh);
        h->parser.gh = NULL;
    }

    /* parser_on drives the (drain-local) tier decision; mirror under buf_lock so
     * a concurrent reader/logger observes a consistent value. */
    pthread_mutex_lock (&h->buf_lock);
    h->parser_on = false;
    pthread_mutex_unlock (&h->buf_lock);

    PRINT_LOG_WARN ("log-phase parser disabled (%s); tiered buffer degraded to single-mode\n", why);
}

/* Arm the reserved spool: drain-local write to log_phase, mirrored to phase_pub
 * under buf_lock for observability. */
static
void enter_log_phase (BACKUP_HANDLE* h)
{
    h->log_phase = true;

    pthread_mutex_lock (&h->buf_lock);
    h->phase_pub = true;
    pthread_mutex_unlock (&h->buf_lock);

    PRINT_LOG_INFO ("log-phase boundary detected (~%lld bytes in); reserved spool armed\n",
                    (long long) h->bytes_total);
}

/* Gate the accumulated global header: accept ONLY {START marker, magic v2,
 * version 2, LZ4} — the single walkable combination. */
static
int gate_ok (BACKUP_HANDLE* h)
{
    CUB_BKUP_HEADER* g = (CUB_BKUP_HEADER *) h->parser.gh;

    if (g->iopageid != CUB_BK_START_PAGE_ID)
    {
        return 0;
    }
    if (memcmp (g->magic, CUB_BK_MAGIC, sizeof (CUB_BK_MAGIC) - 1) != 0)
    {
        return 0;
    }
    if (g->bk_hdr_version != CUB_BK_HDR_VERSION)
    {
        return 0;
    }
    if (g->zip_method != CUB_BK_ZIP_LZ4)   /* NONE/ZLIB framing differs → not walkable */
    {
        return 0;
    }
    if (g->bkpagesize <= 0 || g->bkpagesize > CUB_BK_MAX_BKPAGESIZE)
    {
        return 0;
    }

    h->parser.bkpagesize = g->bkpagesize;
    return 1;
}

/* volid whitelist: a data volume [0, MAX] or a known system negative volid.
 * Floor is DWB(-22) for robustness (the log/archive floor proper is -20). */
static
int volid_valid (int v)
{
    if (v >= 0 && v <= CUB_LOG_MAX_DBVOLID)
    {
        return 1;
    }
    return (v <= CUB_BK_START_PAGE_ID && v >= CUB_LOG_DWB_VOLID);   /* -2 .. -22 */
}

/* Consume the bytes just read from the FIFO, [buf, buf+n), advancing the framing
 * walker across ANY read-chunk boundary (fields/skips persist across calls). The
 * only bytes ever inspected are the one-time global header plus ≤18 B per file
 * (tag + nbytes + volid); every compressed page is skipped by its self-declared
 * length without being read or decompressed. */
static
void parser_observe (BACKUP_HANDLE* h, const char* buf, size_t n)
{
    BK_PARSER* p = &h->parser;
    size_t i = 0;

    while (i < n)
    {
        size_t avail = n - i;

        switch (p->st)
        {
            case PS_DONE:            /* log phase latched: the tail is all log */
            case PS_DISABLED:        /* fallback policy in force               */
                return;

            case PS_GATE:            /* accumulate the global header, then gate */
            {
                size_t take = (avail < p->need) ? avail : p->need;
                memcpy (p->gh + p->got, buf + i, take);
                p->got  += take;
                p->need -= take;
                i       += take;
                if (p->need == 0)
                {
                    if (!gate_ok (h))
                    {
                        parser_disable (h, "gate");
                        return;
                    }
                    free (p->gh);
                    p->gh   = NULL;
                    p->skip = (long long) CUB_BK_HEADER_IO_SIZE - (long long) CUB_BK_HEADER_STRUCT;
                    p->st   = PS_SKIP;         /* consume header padding, then walk tags */
                }
                break;
            }

            case PS_SKIP:            /* discard payload / unit-tail / header pad */
            {
                size_t take = (avail < (size_t) p->skip) ? avail : (size_t) p->skip;
                p->skip -= (long long) take;
                i       += take;
                if (p->skip == 0)
                {
                    p->st   = PS_TAG;
                    p->got  = 0;
                    p->need = CUB_BK_TAG_SIZE;
                }
                break;
            }

            case PS_TAG:             /* peek the 4-byte frame lead */
            {
                int32_t tag;
                size_t take = (avail < p->need) ? avail : p->need;
                memcpy (p->acc + p->got, buf + i, take);
                p->got  += take;
                p->need -= take;
                i       += take;
                if (p->need != 0)
                {
                    break;           /* tag straddles reads; resume next call */
                }
                memcpy (&tag, p->acc, CUB_BK_TAG_SIZE);   /* native int; same-host pipe */
                if (tag >= 0)                             /* compressed page: [buf_len][payload] */
                {
                    if (tag == 0 || tag > p->bkpagesize + CUB_BK_PAGE_OVERHEAD)
                    {
                        parser_disable (h, "buf_len");
                        return;
                    }
                    p->skip = tag;
                    p->st   = PS_SKIP;                    /* skip payload without reading it */
                }
                else if (tag == CUB_BK_FILE_START_PAGE_ID)   /* -4: the ONLY raw negative tag */
                {
                    p->got  = CUB_BK_TAG_SIZE;            /* keep tag; gather through volid */
                    p->need = CUB_BK_FS_PEEK - CUB_BK_TAG_SIZE;
                    p->st   = PS_FS_HDR;
                }
                else                                      /* -2/-3/-5/-6 never appear mid-stream */
                {
                    parser_disable (h, "unexpected-neg-tag");
                    return;
                }
                break;
            }

            case PS_FS_HDR:          /* accumulate through volid, then classify */
            {
                int64_t nbytes;
                int16_t volid;
                size_t take = (avail < p->need) ? avail : p->need;
                memcpy (p->acc + p->got, buf + i, take);
                p->got  += take;
                p->need -= take;
                i       += take;
                if (p->need != 0)
                {
                    break;           /* header straddles reads; resume next call */
                }
                memcpy (&nbytes, p->acc + CUB_BK_WIRE_NBYTES_OFF, 8);
                memcpy (&volid,  p->acc + CUB_BK_WIRE_VOLID_OFF,  2);
                if (nbytes < 0 || !volid_valid (volid))
                {
                    parser_disable (h, "fs-hdr");
                    return;
                }
                if (volid >= 0)
                {
                    p->saw_data_vol = 1;                  /* a data volume has streamed */
                }
                else if (p->saw_data_vol)
                {
                    enter_log_phase (h);                  /* boundary: first neg volid after data */
                    p->st = PS_DONE;
                    return;
                }
                /* pre-data negative (TDE/volinfo) or a data volume: skip the rest of
                 * this FILE_START unit and resume tag-walking. */
                p->skip = (long long) CUB_BK_FILE_UNIT - (long long) CUB_BK_FS_PEEK;
                p->st   = PS_SKIP;
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
    BACKUP_HANDLE* h = (BACKUP_HANDLE *)arg;

    sigset_t block_set;
    struct pollfd fds[2];
    char* tmp = NULL;
    size_t io_size;
    size_t stage_cap;         /* scratch size: max(io_size, pipe capacity)       */
    int    probe_ok;          /* data-phase boundary probe is usable this run     */
    size_t carry_off = 0;     /* pending data-phase probe chunk held in tmp       */
    size_t carry_len = 0;

    /* Keep SIGCHLD steered to backup_thread's sigtimedwait(); if delivered here
     * the child would never be reaped and EOF never detected. */
    sigemptyset (&block_set);
    sigaddset (&block_set, SIGCHLD);
    pthread_sigmask (SIG_BLOCK, &block_set, NULL);

    /* disk-tier scratch. Sized to the LARGER of io_size and the kernel pipe
     * capacity, so a single boundary "probe" read can span the whole pipe
     * backlog and always capture the log-phase FILE_START that backupdb has
     * committed while blocked under LOG_CS (design §4.6 / MAJOR-A). */
    io_size   = (size_t) backup_mgr->io_size;
    stage_cap = io_size;
    {
        int pcap = fcntl (h->fifo_fd, F_GETPIPE_SZ);
        long long want = (pcap > 0)
                         ? (long long) pcap
                         : (long long) backup_mgr->default_backup_option.fifo_size;
        if (want > (long long) stage_cap)
        {
            stage_cap = (size_t) want;
        }
    }
    if (h->disk_cap > 0)
    {
        tmp = malloc (stage_cap);
        if (tmp == NULL)
        {
            buf_mark_error (h);
            return NULL;
        }
    }

    /* The probe commits its (possibly pipe-sized) chunk to the reserved spool in
     * one shot, so it is only usable when the reserve can hold it. This is a
     * static capacity test; live parser_on is re-checked at the probe guard so a
     * mid-run parser_disable cleanly reverts to single-mode. */
    probe_ok = (h->disk_cap >= (long long) stage_cap);

    /* If the reserve is smaller than one pipe's worth, the probe can never commit
     * a boundary chunk — the data phase would then withhold the reserve AND never
     * detect the boundary, which is strictly WORSE than single-mode. Revert to
     * true single-mode fallback (spill_ok becomes true; the reserve is used as
     * plain overflow exactly like the pre-feature path). */
    if (h->parser_on && !probe_ok)
    {
        parser_disable (h, "reserve-smaller-than-pipe");
    }

    for (;;)
    {
        char*  dst;
        size_t room, tail, contig, avail;
        ssize_t n;
        int    use_disk;
        int    probe = 0;
        long long dtail = 0, droom = 0;

        /* Flush a pending data-phase probe chunk into the memory ring before any
         * new read. These bytes are older than anything read next (order intact)
         * and never touch the reserved disk. */
        if (carry_len > 0)
        {
            size_t k;

            pthread_mutex_lock (&h->buf_lock);
            while (mem_ring_free (h) == 0 && !h->stop)
            {
                h->wait_cnt++;
                pthread_cond_wait (&h->not_full, &h->buf_lock);
            }
            if (h->stop)
            {
                pthread_mutex_unlock (&h->buf_lock);
                break;
            }
            k = mem_ring_put (h, tmp + carry_off, carry_len);
            carry_off += k;
            carry_len -= k;
            h->bytes_total += (long long) k;
            if (h->mem_len > h->hw_mem)
            {
                h->hw_mem = h->mem_len;
            }
            pthread_cond_signal (&h->not_empty);
            pthread_mutex_unlock (&h->buf_lock);
            continue;                            /* keep flushing until carry drained */
        }

        fds[0].fd = h->fifo_fd;    fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = h->cancel_efd; fds[1].events = POLLIN; fds[1].revents = 0;

        if (poll (fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            buf_mark_error (h);
            break;
        }

        if (fds[1].revents != 0 || h->stop)        /* cancel / shutdown */
        {
            break;
        }

        pthread_mutex_lock (&h->buf_lock);

        /* Choose the tier (order-preserving: once spilling, or mem full, new bytes
         * go to disk — they are newer than all mem data) and WAIT until THAT tier
         * has room. Waiting on "both full" would wrongly proceed when the chosen
         * tier (disk, while spilling) is full but the other (mem) has space,
         * yielding a 0-byte read that looks like EOF. */
        for (;;)
        {
            int spill_ok;

            if (h->stop)
            {
                break;
            }

            /* 2-mode reserved spool: in the DATA phase (parser on, log phase not
             * yet reached) the disk reserve is withheld — mem-only, then WAIT
             * (harmless backpressure; LOG_CS not held). Once the LOG phase is
             * reached (or the parser is off = single-mode fallback) the reserve is
             * armed exactly as the original expression did. */
            spill_ok = (!h->parser_on) || h->log_phase;

            use_disk = (h->disk_cap > 0)
                       && (h->disk_len > 0 || (mem_ring_free (h) == 0 && spill_ok));

            if (use_disk ? (disk_ring_free (h) > 0) : (mem_ring_free (h) > 0))  /* Tier-3 WAIT */
            {
                break;
            }

            /* DATA phase, mem full, reserve still empty: rather than park (which
             * would strand the boundary FILE_START in a full pipe while backupdb
             * blocks under LOG_CS), probe one pipe-sized chunk into the scratch to
             * reach and classify the boundary WITHOUT waiting for the reader. */
            if (h->parser_on && probe_ok && !h->log_phase
                && h->disk_len == 0 && carry_len == 0)
            {
                probe = 1;
                break;
            }

            h->wait_cnt++;
            pthread_cond_wait (&h->not_full, &h->buf_lock);
        }

        if (h->stop)
        {
            pthread_mutex_unlock (&h->buf_lock);
            break;
        }

        if (probe)
        {
            dst  = tmp;                                      /* span the pipe backlog */
            room = stage_cap;
            h->lookahead_cnt++;
        }
        else if (use_disk)
        {
            dtail = (h->disk_head + h->disk_len) % h->disk_cap;
            droom = h->disk_cap - dtail;                     /* contiguous run to file end */
            if (droom > h->disk_cap - h->disk_len)           /* bounded by total free */
            {
                droom = h->disk_cap - h->disk_len;
            }
            if (droom > (long long) io_size)                 /* one chunk at a time */
            {
                droom = (long long) io_size;
            }
            dst  = tmp;                                      /* stage in private scratch */
            room = (size_t) droom;
        }
        else
        {
            /* reserve the contiguous free run [tail..end); drain owns it until commit */
            tail   = (h->mem_head + h->mem_len) % h->mem_cap;
            contig = h->mem_cap - tail;
            avail  = h->mem_cap - h->mem_len;
            room   = (contig < avail) ? contig : avail;
            dst    = h->mem_buf + tail;                      /* read straight into the ring */
        }

        pthread_mutex_unlock (&h->buf_lock);

        n = read (h->fifo_fd, dst, room);           /* lock-free: target is drain-private */

        if (n < 0)
        {
            probe = 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                continue;
            }
            buf_mark_error (h);
            break;
        }

        if (n == 0)
        {
            /* EOF from kernel. Spurious if backupdb has not opened the write end
             * yet (state still RUNNING/NO_SPAWN) — wait briefly and retry. */
            THREAD_STATE st = h->backup_thread_state;

            probe = 0;

            if (st == THREAD_STATE_EXIT || st == THREAD_STATE_EXIT_WITH_ERROR || h->is_cancel)
            {
                drain_classify_eof (h);
                break;
            }

            {
                struct pollfd p;
                p.fd = h->cancel_efd;
                p.events = POLLIN;
                p.revents = 0;
                poll (&p, 1, 20);                    /* avoid busy-spin before writer connects */
            }

            if (h->stop)
            {
                break;
            }

            continue;
        }

        /* Peek the bytes just read (read-only; never mutated/dropped/reordered).
         * Runs before the commit bumps mem_len, so peeking mem_buf+tail races
         * nothing — the reader cannot see [tail..) yet. */
        if (h->parser_on)
        {
            parser_observe (h, dst, (size_t) n);
        }

        if (probe)
        {
            probe = 0;

            if (h->log_phase)
            {
                /* Boundary caught in this chunk. It is the FIRST disk write (probe
                 * fires only when disk_len==0) so dtail==0 and the whole chunk
                 * (trailing data + boundary + leading log) fits contiguously
                 * (n <= stage_cap <= disk_cap via probe_ok). Spill it in order. */
                ssize_t w = 0;

                pthread_mutex_lock (&h->buf_lock);
                dtail = (h->disk_head + h->disk_len) % h->disk_cap;   /* == 0 */
                pthread_mutex_unlock (&h->buf_lock);

                while (w < n)
                {
                    ssize_t k = pwrite (h->disk_fd, tmp + w, (size_t) (n - w), (off_t) (dtail + w));
                    if (k < 0) { if (errno == EINTR) continue; break; }
                    w += k;
                }

                if (w != n)
                {
                    buf_mark_error (h);
                    break;
                }

                pthread_mutex_lock (&h->buf_lock);
                h->disk_len += n;
                h->bytes_total += n;
                h->spilled = true;
                if (h->disk_len > h->hw_disk)
                {
                    h->hw_disk = h->disk_len;
                }
                pthread_cond_signal (&h->not_empty);
                pthread_mutex_unlock (&h->buf_lock);
            }
            else
            {
                /* Still the data phase: hold this chunk and flush it to the memory
                 * ring next iteration. Data-phase bytes NEVER touch the reserved
                 * disk — that is what keeps the reserve intact (design §4.5). */
                carry_off = 0;
                carry_len = (size_t) n;
            }

            continue;
        }

        if (use_disk)
        {
            ssize_t w = 0;

            /* single region (room <= contiguous-to-end); loop only on partial write.
             * space is pre-reserved at setup, so pwrite does not hit ENOSPC here. */
            while (w < n)
            {
                ssize_t k = pwrite (h->disk_fd, tmp + w, (size_t) (n - w), (off_t) (dtail + w));
                if (k < 0) { if (errno == EINTR) continue; break; }
                w += k;
            }

            if (w != n)
            {
                buf_mark_error (h);
                break;
            }

            pthread_mutex_lock (&h->buf_lock);
            h->disk_len += n;
            h->bytes_total += n;
            h->spilled = true;
            if (h->disk_len > h->hw_disk)
            {
                h->hw_disk = h->disk_len;
            }
            pthread_cond_signal (&h->not_empty);
            pthread_mutex_unlock (&h->buf_lock);
        }
        else
        {
            pthread_mutex_lock (&h->buf_lock);
            h->mem_len += (size_t) n;
            h->bytes_total += n;
            if (h->mem_len > h->hw_mem)
            {
                h->hw_mem = h->mem_len;
            }
            pthread_cond_signal (&h->not_empty);
            pthread_mutex_unlock (&h->buf_lock);
        }
    }

    free (tmp);

    /* final wake so a parked reader observes producer_eof/buf_error/stop */
    pthread_mutex_lock (&h->buf_lock);
    pthread_cond_broadcast (&h->not_empty);
    pthread_cond_broadcast (&h->not_full);
    pthread_mutex_unlock (&h->buf_lock);

    return NULL;
}

/* Reader side: pop oldest-first from the tiered buffer (replaces FIFO-direct
 * read when buffering is enabled). Preserves the is_backup_end/return contract. */
static
int pop_backup_data (BACKUP_HANDLE* h, char* buffer, unsigned int buffer_size,
                     unsigned int* data_len, bool* is_backup_end)
{
    size_t copied;

    pthread_mutex_lock (&h->buf_lock);

    while (h->mem_len == 0 && h->disk_len == 0 && !h->producer_eof && !h->buf_error)
    {
        pthread_cond_wait (&h->not_empty, &h->buf_lock);
    }

    if (h->buf_error)
    {
        pthread_mutex_unlock (&h->buf_lock);
        PRINT_LOG_ERR (ERR_INFO);
        return FAILURE;
    }

    if (h->mem_len == 0 && h->disk_len == 0 && h->producer_eof)
    {
        *is_backup_end = true;
        *data_len = 0;
        pthread_mutex_unlock (&h->buf_lock);
        return SUCCESS;
    }

    copied = mem_ring_pop (h, buffer, buffer_size);   /* older data first */

    if (copied < buffer_size && h->disk_len > 0)      /* then newer disk data */
    {
        ssize_t d = disk_ring_pop (h, buffer + copied, buffer_size - copied);

        if (d < 0)
        {
            pthread_mutex_unlock (&h->buf_lock);
            PRINT_LOG_ERR (ERR_INFO);
            return FAILURE;
        }

        copied += (size_t) d;
    }

    *data_len = (unsigned int) copied;

    pthread_cond_signal (&h->not_full);
    pthread_mutex_unlock (&h->buf_lock);

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
                    backup_handle->parser.gh = malloc ((size_t) CUB_BK_HEADER_STRUCT);

                    if (backup_handle->parser.gh == NULL)
                    {
                        PRINT_LOG_WARN ("parser header alloc failed; log-phase detection disabled\n");
                        backup_handle->parser.st = PS_DISABLED;
                        backup_handle->parser_on = false;
                    }
                    else
                    {
                        backup_handle->parser.st           = PS_GATE;
                        backup_handle->parser.need         = (size_t) CUB_BK_HEADER_STRUCT;
                        backup_handle->parser.got          = 0;
                        backup_handle->parser.skip         = 0;
                        backup_handle->parser.saw_data_vol = 0;
                        backup_handle->parser.bkpagesize   = 0;
                        backup_handle->parser_on           = true;
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
                    if (backup_handle->parser.gh != NULL)
                    {
                        free (backup_handle->parser.gh);
                        backup_handle->parser.gh = NULL;
                    }
                    backup_handle->parser.st = PS_DISABLED;
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

    if (IS_NULL (backup_handle))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    // mutex 잡기전에 handle validation을 먼저 수행하도록 변경한다.
    // 사유는 쓰레기 (NULL 아닌) 값을 handle로 전달할 경우
    // 1. hang
    // 2. seg fault
    // 발생할 수 있다.
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
        PRINT_LOG_INFO ("tiered buffer summary: log_phase=%d spilled=%d hw_mem=%zu "
                        "hw_disk=%lld wait_cnt=%lu lookahead=%lu bytes_total=%lld\n",
                        (int) backup_handle->phase_pub, (int) backup_handle->spilled,
                        backup_handle->hw_mem, backup_handle->hw_disk,
                        backup_handle->wait_cnt, backup_handle->lookahead_cnt,
                        backup_handle->bytes_total);
    }

    if (IS_FAILURE (close_fifo (BACKUP_HANDLE_TYPE, backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
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
    // 파일이 존재하면 따로 검사하지 말고, overwrite 해버리자
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
