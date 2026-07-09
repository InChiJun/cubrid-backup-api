#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "handle_manager.h"

HANDLE_MANAGER handle_manager;

HANDLE_MANAGER* handle_mgr = &handle_manager;

static
int initialize_handle_manager (void)
{
    if (IS_FAILURE (pthread_mutex_init (&handle_mgr->backup_handle.backup_mutex, NULL)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_mutex_init (&handle_mgr->restore_handle.restore_mutex, NULL)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* tiered-buffer sync objects: init once per process (the handle is reused
     * across begin/end, so these must NOT be re-init'd per backup). */
    if (IS_FAILURE (pthread_mutex_init (&handle_mgr->backup_handle.buf_lock, NULL)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_cond_init (&handle_mgr->backup_handle.not_empty, NULL)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (pthread_cond_init (&handle_mgr->backup_handle.not_full, NULL)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    /* Seed fd sentinels to -1 up front: the static handle is zero-initialized,
     * so without this a finalize before the first begin would close() fd 0. */
    handle_mgr->backup_handle.fifo_fd       = -1;
    handle_mgr->backup_handle.disk_fd       = -1;
    handle_mgr->backup_handle.cancel_efd    = -1;
    handle_mgr->backup_handle.mem_buf       = NULL;
    handle_mgr->backup_handle.drain_started = false;
    handle_mgr->restore_handle.restore_fd   = -1;

    return SUCCESS;

error:

    return FAILURE;
}

static
int finalize_handle_manager (void)
{
    pthread_mutex_destroy (&handle_mgr->backup_handle.backup_mutex);

    pthread_mutex_destroy (&handle_mgr->restore_handle.restore_mutex);

    pthread_cond_destroy (&handle_mgr->backup_handle.not_full);
    pthread_cond_destroy (&handle_mgr->backup_handle.not_empty);
    pthread_mutex_destroy (&handle_mgr->backup_handle.buf_lock);

    return SUCCESS;
}

static
int initialize_backup_handle (BACKUP_HANDLE* backup_handle)
{
    backup_handle->backup_thread_state = THREAD_STATE_NO_SPAWN;

    backup_handle->is_cancel = false;

    backup_handle->backup_level   = BACKUP_FULL_LEVEL;
    backup_handle->remove_archive = false;
    backup_handle->sa_mode        = false;
    backup_handle->no_check       = false;
    backup_handle->compress       = false;

    backup_handle->fifo_fd = -1;
    backup_handle->fifo_path[0] = '\0';

    backup_handle->db_name[0] = '\0';

    /* ── tiered buffer: reset only (handle is reused; never malloc/open here) ── */
    backup_handle->buffering_enabled = false;
    backup_handle->drain_started     = false;
    backup_handle->stop              = false;
    backup_handle->cancel_efd        = -1;

    backup_handle->mem_buf  = NULL;
    backup_handle->mem_cap  = 0;
    backup_handle->mem_len  = 0;
    backup_handle->mem_head = 0;
    backup_handle->mem_tail = 0;

    backup_handle->disk_fd   = -1;
    backup_handle->disk_cap  = 0;
    backup_handle->disk_len  = 0;
    backup_handle->disk_head = 0;
    backup_handle->disk_tail = 0;

    backup_handle->producer_eof = false;
    backup_handle->buf_error    = false;

    backup_handle->hw_mem        = 0;
    backup_handle->hw_disk       = 0;
    backup_handle->spilled       = false;
    backup_handle->wait_cnt      = 0;
    backup_handle->wait_us_total = 0;
    backup_handle->bytes_total   = 0;

    return SUCCESS;
}

static
int finalize_backup_handle (BACKUP_HANDLE* backup_handle)
{
    if (backup_handle->backup_thread_state == THREAD_STATE_RUNNING)
    {
        backup_handle->is_cancel = true;

        // thread 상태 변경 후 create 하기 때문에 생성 실패시 hang 발생할 수 있다.
        // (생성 실패하였는데, 상태는 THREAD_STATE_RUNNING 이기 때문에)
        // 상태를 먼저 변경하는 이유는
        // cubrid_backup_finalize () 호출 시 thread 상태가 THREAD_STATE_RUNNING 로 바뀌기 전이라면,
        // 이 부분을 pass 하고, 내부 handle을 free하기 때문에
        // 서버에서 아래와 같은 에러가 발생한다.
        // ERROR: Destination-path does not exist or is not a directory.
        // 
        // 이는 handle에 있던 -D (fifo) 경로를 아래 initialize_backup_handle () 함수에서
        // 초기화하기 때문이다.
        // 이 구조적인 문제는 다음 버전에서 개선하기로 한다.
        pthread_join (backup_handle->backup_thread, NULL);
    }

    /* tiered-buffer: wake a parked drain (cond_wait / poll) and join it BEFORE
     * the fifo is closed (the drain reads fifo_fd). No-op until a drain is
     * spawned (drain_started stays false in M-1..M-3). */
    if (backup_handle->drain_started)
    {
        pthread_mutex_lock (&backup_handle->buf_lock);
        backup_handle->stop      = true;
        backup_handle->buf_error = true;
        pthread_cond_broadcast (&backup_handle->not_empty);
        pthread_cond_broadcast (&backup_handle->not_full);
        pthread_mutex_unlock (&backup_handle->buf_lock);

        if (backup_handle->cancel_efd != -1)
        {
            uint64_t one = 1;
            (void) write (backup_handle->cancel_efd, &one, sizeof (one));
        }

        pthread_join (backup_handle->drain_thread, NULL);
        backup_handle->drain_started = false;
    }

    if (backup_handle->fifo_fd != -1)
    {
        close (backup_handle->fifo_fd);
        unlink (backup_handle->fifo_path);
    }

    /* spool fd close reclaims disk space (unlink-on-open); free the mem ring.
     * initialize_backup_handle() below resets these, keeping teardown idempotent. */
    if (backup_handle->disk_fd != -1)
    {
        close (backup_handle->disk_fd);
    }

    if (backup_handle->cancel_efd != -1)
    {
        close (backup_handle->cancel_efd);
    }

    if (backup_handle->mem_buf != NULL)
    {
        free (backup_handle->mem_buf);
    }

    initialize_backup_handle (backup_handle);

    return SUCCESS;
}

static
int initialize_restore_handle (RESTORE_HANDLE* restore_handle)
{
    restore_handle->restore_type = -1;

    restore_handle->backup_level = BACKUP_FULL_LEVEL;

    restore_handle->restore_fd = -1;
    restore_handle->backup_file_path[0] = '\0';

    restore_handle->db_name[0] = '\0';

    return SUCCESS;
}

static
int finalize_restore_handle (RESTORE_HANDLE* restore_handle)
{
    if (restore_handle->restore_fd != -1)
    {
        close (restore_handle->restore_fd);
    }

    initialize_restore_handle (restore_handle);

    return SUCCESS;
}

int start_handle_manager (void)
{
    if (IS_FAILURE (initialize_handle_manager ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

int stop_handle_manager (void)
{
    if (IS_FAILURE (free_handle (BACKUP_HANDLE_TYPE, &handle_mgr->backup_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (free_handle (RESTORE_HANDLE_TYPE, &handle_mgr->restore_handle)))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    if (IS_FAILURE (finalize_handle_manager ()))
    {
        PRINT_LOG_ERR (ERR_INFO);
        goto error;
    }

    return SUCCESS;

error:

    return FAILURE;
}

int alloc_handle (HANDLE_TYPE handle_type, void** handle)
{
    BACKUP_HANDLE* backup_handle;
    RESTORE_HANDLE* restore_handle;

    int state = 0;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = &handle_mgr->backup_handle;

        if (IS_FAILURE (pthread_mutex_trylock (&backup_handle->backup_mutex)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        state = 1;

        if (IS_FAILURE (initialize_backup_handle (backup_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        *(BACKUP_HANDLE **)handle = backup_handle;
    }
    else if (handle_type == RESTORE_HANDLE_TYPE)
    {
        restore_handle = &handle_mgr->restore_handle;

        if (IS_FAILURE (pthread_mutex_trylock (&restore_handle->restore_mutex)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        state = 1;

        if (IS_FAILURE (initialize_restore_handle (restore_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        *(RESTORE_HANDLE **)handle = restore_handle;
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            if (handle_type == BACKUP_HANDLE_TYPE)
            {
                pthread_mutex_unlock (&backup_handle->backup_mutex);
            }
            else if (handle_type == RESTORE_HANDLE_TYPE)
            {
                pthread_mutex_unlock (&restore_handle->restore_mutex);
            }
        default:
            break;
    }

    return FAILURE;
}

int free_handle (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;
    RESTORE_HANDLE* restore_handle;

    int retval;
    int state = 0;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = (BACKUP_HANDLE *)handle;

        retval = pthread_mutex_trylock (&backup_handle->backup_mutex);

        if (IS_FAILURE (retval) && retval != EBUSY)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        state = 1;

        if (IS_FAILURE (finalize_backup_handle (backup_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (pthread_mutex_unlock (&backup_handle->backup_mutex)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (handle_type == RESTORE_HANDLE_TYPE)
    {
        restore_handle = (RESTORE_HANDLE *)handle;

        retval = pthread_mutex_trylock (&restore_handle->restore_mutex);

        if (IS_FAILURE (retval) && retval != EBUSY)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        state = 1;

        if (IS_FAILURE (finalize_restore_handle (restore_handle)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }

        if (IS_FAILURE (pthread_mutex_unlock (&restore_handle->restore_mutex)))
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    return SUCCESS;

error:

    switch (state)
    {
        case 1:
            if (handle_type == BACKUP_HANDLE_TYPE)
            {
                pthread_mutex_unlock (&backup_handle->backup_mutex);
            }
            else if (handle_type == RESTORE_HANDLE_TYPE)
            {
                pthread_mutex_unlock (&restore_handle->restore_mutex);
            }
        default:
            break;
    }

    return FAILURE;
}

int validate_handle (HANDLE_TYPE handle_type, void* handle)
{
    BACKUP_HANDLE* backup_handle;
    RESTORE_HANDLE* restore_handle;

    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        backup_handle = &handle_mgr->backup_handle;

        if (backup_handle != handle)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }
    else if (handle_type == RESTORE_HANDLE_TYPE)
    {
        restore_handle = &handle_mgr->restore_handle;

        if (restore_handle != handle)
        {
            PRINT_LOG_ERR (ERR_INFO);
            goto error;
        }
    }

    return SUCCESS;

error:

    return FAILURE;
}

int set_thread_state (HANDLE_TYPE handle_type, void* handle, THREAD_STATE state_where)
{
    if (handle_type == BACKUP_HANDLE_TYPE)
    {
        ((BACKUP_HANDLE *)handle)->backup_thread_state = state_where;
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

