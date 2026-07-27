#ifndef _HANDLE_MANAGER_H_
#define _HANDLE_MANAGER_H_

#include <pthread.h>
#include <signal.h>
#include "backup_manager.h"
#include "cubrid_backup_format.h"

/* The maximum length of database name is 17 in English. */
#define MAX_DB_NAME_LEN 17

typedef enum handle_type HANDLE_TYPE;
enum handle_type
{
    BACKUP_HANDLE_TYPE,
    RESTORE_HANDLE_TYPE
};

typedef enum thread_state THREAD_STATE;
enum thread_state
{
    THREAD_STATE_NO_SPAWN,
    THREAD_STATE_RUNNING,
    THREAD_STATE_EXIT,
    THREAD_STATE_EXIT_WITH_ERROR
};

typedef enum backup_level BACKUP_LEVEL;
enum backup_level
{
    BACKUP_FULL_LEVEL = 0,
    BACKUP_BIG_INCREMENT_LEVEL,
    BACKUP_SMALL_INCREMENT_LEVEL
};

typedef struct backup_handle BACKUP_HANDLE;
struct backup_handle
{
    pthread_t backup_thread;
    pthread_mutex_t backup_mutex;

    volatile THREAD_STATE backup_thread_state;   /* read by drain for EOF classification */

    /* backup_thread join guard: the thread publishes its terminal state before
     * it finishes, so THREAD_STATE cannot tell whether a join is still owed. */
    bool backup_thread_started;

    bool is_cancel;

    BACKUP_LEVEL backup_level;
    bool remove_archive;
    bool sa_mode;
    bool no_check;
    bool compress;

    int fifo_fd;
    char fifo_path[PATH_MAX];

    char db_name[MAX_DB_NAME_LEN + 1];

    /* ── tiered buffer ── */
    bool buffering_enabled;   /* true when mem_buf alloc succeeds; false = legacy direct-FIFO path */
    bool drain_started;       /* drain_thread join guard (THREAD_STATE is for backup_thread only) */
    volatile bool stop;       /* unified stop signal (cancel/error/eof); read lock-free in drain */
    int  cancel_efd;          /* eventfd: wakes drain immediately during poll; -1 = unused */

    char*  mem_buf;           /* malloc(mem_cap), memory ring; NULL = not allocated */
    size_t mem_cap;
    size_t mem_len;
    size_t mem_head;
    size_t mem_tail;

    int       disk_fd;        /* spool file (Phase 2); -1 = unused */
    long long disk_cap;
    long long disk_len;
    long long disk_head;
    long long disk_tail;

    bool producer_eof;        /* drain: FIFO reached normal EOF */
    bool buf_error;           /* buffered path error/cancel */

    pthread_t       drain_thread;
    pthread_mutex_t buf_lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    /* observability */
    size_t        mem_high_water;
    long long     disk_high_water;
    bool          spilled;
    unsigned long wait_count;
    long long     wait_us_total;
    long long     bytes_total;

    /* ── observational log-phase parser (drain-thread-private) ──
     * parser_on / log_phase are written and read only by the drain thread
     * (parser peeks, then use_disk consumes next iteration: same thread ⇒ no
     * sync needed for the tier decision). phase_published mirrors log_phase under
     * buf_lock for cross-thread observability only. */
    BK_PARSER     parser;
    bool          parser_on;        /* run the parser + 2-mode reserved-spool policy */
    bool          log_phase;        /* log-copy phase reached; reserved spool armed   */
    bool          phase_published;  /* observability mirror of log_phase (buf_lock)   */
    unsigned long lookahead_count;  /* # of data-phase probe reads performed          */
};

typedef struct restore_handle RESTORE_HANDLE;
struct restore_handle
{
    pthread_mutex_t restore_mutex;

    int restore_type;

    BACKUP_LEVEL backup_level;

    int restore_fd;
    char backup_file_path[PATH_MAX];

    char db_name[MAX_DB_NAME_LEN + 1];
};

typedef struct handle_manager HANDLE_MANAGER;
struct handle_manager
{
    BACKUP_HANDLE backup_handle;
    RESTORE_HANDLE restore_handle;
};

extern HANDLE_MANAGER* handle_mgr;

int start_handle_manager (void);
int stop_handle_manager (void);
int alloc_handle (HANDLE_TYPE, void**);
int free_handle (HANDLE_TYPE, void*);
int validate_handle (HANDLE_TYPE, void*);
int set_thread_state (HANDLE_TYPE, void*, THREAD_STATE);

#endif
