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

    /* Seed fd sentinels to -1 BEFORE any fallible init below: the static handle
     * is zero-initialized, so if an init fails (goto error -> finalize) the fd
     * fields must already be -1, else finalize would close() fd 0. */
    handle_mgr->backup_handle.fifo_fd       = -1;
    handle_mgr->backup_handle.disk_fd       = -1;
    handle_mgr->backup_handle.cancel_efd    = -1;
    handle_mgr->backup_handle.mem_buf       = NULL;
    handle_mgr->backup_handle.drain_started = false;
    handle_mgr->restore_handle.restore_fd   = -1;

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

    backup_handle->backup_thread_started = false;

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

    backup_handle->mem_high_water  = 0;
    backup_handle->disk_high_water = 0;
    backup_handle->spilled         = false;
    backup_handle->wait_count      = 0;
    backup_handle->wait_us_total   = 0;
    backup_handle->bytes_total     = 0;

    /* log-phase parser: reset only (global_header is malloc'd in begin_backup,
     * freed in finalize_backup_handle). Start disabled; begin_backup arms it. */
    backup_handle->parser.state            = PS_DISABLED;
    backup_handle->parser.need_bytes       = 0;
    backup_handle->parser.got_bytes        = 0;
    backup_handle->parser.skip_bytes       = 0;
    backup_handle->parser.saw_data_volume  = 0;
    backup_handle->parser.backup_page_size = 0;
    backup_handle->parser.compressed       = 0;
    backup_handle->parser.global_header    = NULL;
    backup_handle->parser_on               = false;
    backup_handle->log_phase               = false;
    backup_handle->phase_published         = false;
    backup_handle->lookahead_count         = 0;

    return SUCCESS;
}

static
int finalize_backup_handle (BACKUP_HANDLE* backup_handle)
{
    /* Join on "was created", not on THREAD_STATE: a state-gated join is skipped
     * on the normal path and leaks the thread. */
    if (backup_handle->backup_thread_started)
    {
        if (backup_handle->backup_thread_state == THREAD_STATE_RUNNING)
        {
            backup_handle->is_cancel = true;
        }

        // We create the thread after changing its state, so a creation failure can hang.
        // (creation failed, yet the state is already THREAD_STATE_RUNNING)
        // The reason we change the state first: if cubrid_backup_finalize () is called
        // before the thread state becomes THREAD_STATE_RUNNING, this part is skipped and
        // the internal handle is freed, so the server raises the error below.
        // ERROR: Destination-path does not exist or is not a directory.
        // 
        // That happens because the -D (fifo) path stored in the handle is reset by the
        // initialize_backup_handle () function below.
        // This structural problem is left to be improved in a future version.
        pthread_join (backup_handle->backup_thread, NULL);
        backup_handle->backup_thread_started = false;
    }

    /* tiered-buffer: wake a parked drain (cond_wait / poll) and join it BEFORE
     * the fifo is closed (the drain reads fifo_fd). No-op until a drain is
     * spawned. */
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

    /* free the parser's header-accumulation buffer (malloc'd in begin_backup) */
    if (backup_handle->parser.global_header != NULL)
    {
        free (backup_handle->parser.global_header);
        backup_handle->parser.global_header = NULL;
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

