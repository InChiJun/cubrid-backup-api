## Overview
* C interfaces to interwork with 3rd party backup solutions
* a header file and a shared library are provided. The name of the files are as followings:
  * cubrid_backup_api.h
  * libcubridbackupapi.so
## Structures
### CUBRID_BACKUP_INFO
* a structure that can set required information when requesting a cubrid backup
#### Declaration in the header file
<pre>
<code>
typedef struct cubrid_backup_info CUBRID_BACKUP_INFO;
struct cubrid_backup_info
{
    int backup_level;
    int remove_archive;
    int sa_mode;
    int no_check;
    int compress;
    const char* db_name;
};
</code>
</pre>
#### Description of struct members
|member|description|
|-|-|
|backup_level|the backup level supported by cubrid</br>&nbsp;&nbsp;0 - full backup</br>&nbsp;&nbsp;1 - first incremental backup</br>&nbsp;&nbsp;2 - second incremental backup|
|remove_archive|remove archive log volumes that will no longer be used by subsequent backups after the current backup is complete</br>&nbsp;&nbsp;0 - no delete (default)</br>&nbsp;&nbsp;1 - delete|
|sa_mode|the backup execution mode supported by cubrid</br>&nbsp;&nbsp;0 - on-line backup; client/server mode (default)</br>&nbsp;&nbsp;1 - off-line backup; stand-alone mode|
|no_check|perform a consistency check to the backup data</br>&nbsp;&nbsp;0 - consistency check (default)</br>&nbsp;&nbsp;1 - no consistency check|
|compress|perform data compression to the backup data</br>&nbsp;&nbsp;0 - no compression (default)</br>&nbsp;&nbsp;1 - compression|
|db_name|target database name to backup|
### CUBRID_RESTORE_INFO
* a structure that can set required information when requesting a cubrid restore
#### Declaration in the header file
<pre>
<code>
typedef struct cubrid_restore_info CUBRID_RESTORE_INFO;
struct cubrid_restore_info
{
    RESTORE_TYPE restore_type;
    int backup_level;
    const char* up_to_date; /* format: dd-mm-yyyy:hh:mm:ss */
    const char* backup_file_path;
    const char* db_name;
};
</code>
</pre>
#### Description of struct members
|member|description|
|-|-|
|restore_type|the restore type</br>&nbsp;&nbsp;RESTORE_TO_DB - not yet supported</br>&nbsp;&nbsp;RESTORE_TO_FILE - generate a backup volume under the path specified by the backup_file_path option|
|backup_level|the backup level of the backup volume used for restore|
|up_to_date|not yet supported|
|backup_file_path|the directory path to generate a backup volume when the restore_type option is set to the RESTORE_TO_FILE|
|db_name|target database name to restore|
### RESTORE_TYPE
* an enumeration used to set the restore type in CUBRID_RESTORE_INFO structure 
#### Declaration in the header file
<pre>
<code>
typedef enum restore_type RESTORE_TYPE;
enum restore_type
{
    RESTORE_TO_DB,
    RESTORE_TO_FILE
};
</code>
</pre>
#### Description of enum members
|member|description|
|-|-|
|RESTORE_TO_DB|not yet supported|
|RESTORE_TO_FILE|restore the backup data to a file|
## Functions
### cubrid_backup_initialize()
* an API function that should be called first to use cubrid-backup-api
#### Declaration in the header file
<pre>
<code>
int cubrid_backup_initialize (void);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
### cubrid_backup_finalize()
* an API function paired with the cubrid_backup_initialize() function. it is called to terminate the use of cubrid-backup-api
#### Declaration in the header file
<pre>
<code>
int cubrid_backup_finalize (void);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
### cubrid_backup_begin()
* an API function called to perform backup using cubrid-backup-api
#### Declaration in the header file
<pre>
<code>
int cubrid_backup_begin (CUBRID_BACKUP_INFO* backup_info, void** backup_handle);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|backup_info|in|refer to the description of the CUBRID_BACKUP_INFO data structure|
|backup_handle|out|a backup handle that internally identifies a backup</br>used as an input argument when calling cubrid_backup_read() and cubrid_backup_end()|
### cubrid_backup_end()
* an API function called to end a backup started with cubrid_backup_begin()
#### Declaration in the header file
<pre>
<code>
int cubrid_backup_end (void* backup_handle);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|backup_handle|in|the backup handle received when calling cubrid_backup_begin()|
### cubrid_backup_read()
* an API function called to read backup data
#### Declaration in the header file
<pre>
<code>
int cubrid_backup_read (void* backup_handle, void* buffer, unsigned int buffer_size, unsigned int* data_len);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|1|success</br>&nbsp;- need to read the rest of the backup data by calling cubrid_backup_read() again|
|0|success</br>&nbsp;- backup complete|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|backup_handle|in|the backup handle received when calling cubrid_backup_begin()|
|buffer|out|user buffer to save the backup data|
|buffer_size|in|user buffer size|
|data_len|out|actual backup data size copied to the user buffer|
### cubrid_restore_begin()
* an API function called to restore backed up data to DB or a file
#### Declaration in the header file
<pre>
<code>
int cubrid_restore_begin (CUBRID_RESTORE_INFO* restore_info, void** restore_handle);
</code>
</pre>
#### Return:
|return|description|
|-|-|
|0|success|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|restore_info|in|refer to the description of the CUBRID_RESTORE_INFO data structure|
|restore_handle|out|a restore handle that internally identifies restore</br>used as an input argument when calling cubrid_restore_write() and cubrid_restore_end()|
### cubrid_restore_end()
* an API function called to terminate the restore started with cubrid_restore_begin()
#### Declaration in the header file
<pre>
<code>
int cubrid_restore_end (void* restore_handle);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|restore_handle|in|the restore handle received when calling cubrid_restore_begin()|
### cubrid_restore_write()
#### Declaration in the header file
* an API function passing backed up data to API for database restore
<pre>
<code>
int cubrid_restore_write (void* restore_handle, int backup_level, void* buffer, unsigned int data_len);
</code>
</pre>
#### Returns:
|return|description|
|-|-|
|0|success|
|-1|failure|
#### Parameters:
|parameter|in/out|description|
|-|-|-|
|restore_handle|in|the restore handle received when calling cubrid_restore_begin()|
|backup_level|in|the backup level of the backed up data to restore|
|buffer|in|user buffer with backup data|
|data_len|in|actual data size in the user buffer|
## API function call transition diagram
### Backup
![backup](https://cubrid-wiki.atlassian.net/wiki/download/thumbnails/229703681/%EA%B7%B8%EB%A6%BC1.png?version=1&modificationDate=1536662949758&cacheVersion=1&api=v2&width=375&height=400)
### Restore
![restore](https://cubrid-wiki.atlassian.net/wiki/download/thumbnails/229703681/%EA%B7%B8%EB%A6%BC2.png?version=1&modificationDate=1536662902581&cacheVersion=1&api=v2&width=375&height=400)
## Sample code
### Backup
<pre>
<code>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "cubrid_backup_api.h"
 
int main ()
{
    CUBRID_BACKUP_INFO backup_info;
    void* backup_handle;
 
    char backup_buffer[4096];
 
    unsigned int data_len;
    unsigned int total_data_len = 0;
 
    int backup_fd;
 
    int retval;
 
    backup_fd = open ("demodb_bk0v000", O_CREAT | O_WRONLY);
    if (backup_fd == -1)
    {
        printf ("[ERROR] failed to open backup file\n");
        return -1;
    }
 
    retval = cubrid_backup_initialize ();
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_initialize ()\n");
        return -1;
    }
 
    backup_info.backup_level = 0;
    backup_info.db_name = "demodb";
 
    retval = cubrid_backup_begin (&backup_info, &backup_handle);
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_begin ()\n");
        return -1;
    }
 
    while (1)
    {
        retval = cubrid_backup_read (backup_handle, backup_buffer, 4096, &data_len);
 
        if (retval < 0)
        {
            printf ("[ERROR] cubrid_backup_read ()\n");
            return -1;
        }
 
        if (data_len != 0)
        {
            write (backup_fd, backup_buffer, data_len);
 
            total_data_len += data_len;
        }
 
        if (retval == 0) // backup end
        {
            break;
        }
    }
 
    printf ("backup data size ==> %d\n", total_data_len);
 
    retval = cubrid_backup_end (backup_handle);
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_end ()\n");
        return -1;
    }
 
    retval = cubrid_backup_finalize ();
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_finalize ()\n");
        return -1;
    }
 
    close (backup_fd);
 
    return 0;
}
</code>
</pre>
<pre>
<code>
gcc -o backup_sample -L. -lcubridbackupapi -lpthread backup_sample.c
</code>
</pre>
### Restore
<pre>
<code>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "cubrid_backup_api.h"
 
int main ()
{
    CUBRID_RESTORE_INFO restore_info;
    void* restore_handle;
 
    char backup_data[4096];
 
    unsigned int data_len;
    unsigned int total_data_len = 0;
 
    int backup_fd;
 
    int retval;
 
    backup_fd = open ("demodb_bk0v000", O_RDONLY);
    if (backup_fd == -1)
    {
        printf ("[ERROR] failed to open backup file\n");
        return -1;
    }
 
    retval = mkdir ("./restore_dir", S_IRWXU);
    if (retval == -1)
    {
        printf ("[ERROR] failed to create restore directory\n");
        return -1;
    }
 
    retval = cubrid_backup_initialize ();
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_initialize ()\n");
        return -1;
    }
 
    restore_info.restore_type = RESTORE_TO_FILE;
    restore_info.backup_level = 0;
    restore_info.backup_file_path = "./restore_dir";
    restore_info.db_name = "demodb";
 
    retval = cubrid_restore_begin (&restore_info, &restore_handle);
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_restore_begin ()\n");
        return -1;
    }
 
    do
    {
        data_len = read (backup_fd, backup_data, 4096);
        if (data_len == -1)
        {
            printf ("[ERROR] failed to read backup file\n");
            return -1;
        }
        else if (data_len == 0)
        {
            break;
        }
 
        retval = cubrid_restore_write (restore_handle, 0, backup_data, data_len);
 
        if (retval < 0)
        {
            printf ("[ERROR] cubrid_restore_write ()\n");
            return -1;
        }
 
        total_data_len += data_len;
    } while (data_len != 0);
 
    printf ("restore data size ==> %d\n", total_data_len);
 
    retval = cubrid_restore_end (restore_handle);
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_restore_end ()\n");
        return -1;
    }
 
    retval = cubrid_backup_finalize ();
    if (retval < 0)
    {
        printf ("[ERROR] cubrid_backup_finalize ()\n");
        return -1;
    }
 
    close (backup_fd);
 
    return 0;
}
</code>
</pre>
<pre>
<code>
gcc -o restore_sample -L. -lcubridbackupapi -lpthread restore_sample.c
</code>
</pre>

---

# Tiered buffer and log-phase parser

The sections below document the buffering added on top of the API described
above. They do not change the API contract: the calls, structures and sample
code above are unchanged, and the backup image is byte-for-byte the same.

## Why it exists — the problem

CUBRID copies the **log portion** of a backup (archive + active log) while holding `LOG_CS`, the global log critical section. In the API's original design the pipe was read **only** when the caller called `cubrid_backup_read()`, so there was no buffering:

- A slow downstream (tape, network upload, …) let the FIFO fill up.
- `backupdb` writes to the pipe on a non-blocking fd and **busy-spins on `EAGAIN` while holding `LOG_CS`** when the pipe is full.
- Holding `LOG_CS` delays commit log flush → server-wide stalls.

**Fix (short-term mitigation, server untouched):** put a buffer *inside the API* so downstream slowness is absorbed instead of being transmitted straight back into pipe congestion. The pipe is drained continuously, so `backupdb` passes through `LOG_CS` quickly; backpressure only occurs when the whole buffer (memory + disk reserve) is exhausted.

## How it works

- **Drain thread** — a dedicated thread `poll()`s the FIFO and copies bytes into the buffer as fast as the pipe delivers them (single-producer/single-consumer, no busy-spin; an `eventfd` gives immediate cancellation).
- **Memory tier** — a ring buffer sized by `buffer_memory_size`. Buffering is **opt-in**: the compiled default is `0` (off — the legacy direct-FIFO path); set it `> 0` (the shipped sample conf uses 64&nbsp;MB) to enable the drain thread + tiered buffer. The reader (`cubrid_backup_read`) pops from it in order.
- **Disk tier** — when `buffer_disk_limit > 0`, an anonymous spool file (created `O_CREAT|O_EXCL|O_CLOEXEC`, then immediately `unlink`ed, then `fallocate`d) provides overflow reserve beyond memory. Order is always preserved: `[memory (older) | disk (newer)]`.
- **`F_SETPIPE_SZ`** — the pipe is enlarged (clamped to `[64KB, 1MB]`) so a whole `backupdb` write chunk (~1&nbsp;MiB) fits in one go.
- **Observational log-phase parser** — it *peeks* the stream to detect the boundary where DB data volumes end and the transaction log begins. It **never mutates the data**; its only output is a `log_phase` flag that selects the spill policy. It understands the **LZ4** and **uncompressed (NONE)** stream formats; on an unsupported header (e.g. ZLIB), an unknown format version, or any self-check failure it **disables itself and passes the data through untouched**.
- **2-mode reserved spool** — with the parser armed, the disk reserve is held back for the *log* portion (where `LOG_CS` matters); before the boundary, data pages stay in memory. With the parser off, single-mode: the spool is used purely as memory overflow.

> The parser is **observational only** — the backup image it produces is byte-for-byte identical whether the parser is on or off. Detecting the log phase relies on stream ordering and is a best-effort optimization signal, not an API contract.

## Internals — concurrency contract

The buffer is a single-producer (drain thread) / single-consumer (`cubrid_backup_read`) design. Getting this wrong hangs the server, so the contract is explicit:

- **Locks.** `buf_lock` is the only buffer lock — it guards the ring indices, `producer_eof` / `buf_error` / `stop`, and two condition variables (`not_empty`, `not_full`). `backup_mutex` serializes the consumer and protects teardown. Lock order is always `backup_mutex → buf_lock`, and `backup_mutex` is **never held across a `cond_wait`**.
- **Drain loop.** The drain thread `poll()`s the FIFO and a cancellation `eventfd` (no timeout polling, no busy-spin), reserves a contiguous free span (a ring wrap becomes two reads), `read()`s into the ring *outside* the lock (single copy, SPSC), then commits the byte count under `buf_lock` and signals `not_empty`.
- **Cancellation / teardown (deadlock-free).** `cubrid_backup_end` signals **first** under `buf_lock` alone — set `stop`, set `buf_error` if this is not a clean EOF (so a cancelled/truncated backup is never reported as success), broadcast both conditions — and writes the `eventfd`; **only then** does it take `backup_mutex` to join the drain thread and free resources. A consumer blocked in `cond_wait` wakes, sees `buf_error`, returns failure and releases `backup_mutex`, so teardown proceeds without a hang.
- **SIGCHLD isolation.** `SIGCHLD` is blocked in the drain thread so the backup worker's `sigtimedwait` reaps the `backupdb` child; otherwise end-of-stream would never be detected.
- **EOF classification.** A `read()` returning 0 is resolved under `buf_lock`: worker exited cleanly → `producer_eof`; exited with error or was cancelled → `buf_error`; not yet terminal → a short, bounded `eventfd` re-poll before deciding. The reader reports end-of-backup exactly once (a final zero-length `SUCCESS`), never together with data.

## Configuration

Settings live in `$CUBRID/conf/cubrid_backup.conf` under `[backup]`. Absent keys fall back to the defaults below. An out-of-range `fifo_size` is clamped with a warning; genuinely invalid values (e.g. a non-zero `buffer_memory_size` below the I/O unit, or a disk tier with no memory tier) are rejected **fail-fast** at initialization.

| Key | Default | Meaning |
|---|---|---|
| `fifo_size` | `64KB` | Pipe capacity (`F_SETPIPE_SZ`); clamped to `[64KB, 1MB]`. |
| `buffer_memory_size` | `0` | Memory ring size. **Compiled default `0` = buffering OFF (opt-in)** — the legacy direct-FIFO path; set `> 0` to enable the drain/tiered buffer (must be ≥ the I/O unit when non-zero). The shipped sample `cubrid_backup.conf` sets `64MB` to turn it on. |
| `buffer_disk_limit` | `0` | Disk spool reserve cap. `0` = memory-only (no disk tier; parser stays off). Requires `buffer_memory_size > 0`. |
| `buffer_disk_path` | *(empty)* | Directory for the spool file. Must exist and be writable when `buffer_disk_limit > 0`. |
| `buffer_disk_keep_spool` | `false` | Keep the spool file after backup instead of removing it. |

Pre-existing `[backup]` keys (`remove_archive`, `sa_mode`, `no_check`, `thread_count`, `compress`, `except_active_log`, `sleep_msecs`) are unchanged.

Example — memory + disk tier with the parser armed:

```ini
[backup]
compress=true
fifo_size=1MB
buffer_memory_size=64MB
buffer_disk_limit=256MB
buffer_disk_path=/var/tmp/cubrid_backup_spool
buffer_disk_keep_spool=false
```

## Build

```sh
./build.sh            # 64-bit release (RelWithDebInfo)
./build.sh -m debug   # debug build
```

Output goes to `build_x86_64_<mode>/_install/cubrid-backup-api/`. The build links `pthread` explicitly and compiles with `_GNU_SOURCE` (needed for `F_SETPIPE_SZ`, `fallocate`, `eventfd`). `build.sh` runs `git clean -ffdx` first — **new files must be `git add`ed to survive the build.**

## Testing

Two tiers, kept separate on purpose:

- **Fast regression gate** — `testcases/run_test.sh`. Builds the API + test clients, provisions a 100&nbsp;MB `testdb`, and runs the functional cases plus the *light* additions for this feature — `parser_ut` (deterministic log-phase-parser / ring unit tests, no server) and `backup_tc05` (a slow consumer that drives memory → spill → WAIT, then a byte-for-byte restore compare). Finishes in a few minutes; prints `ALL PASSED` / `FAILED TEST SUMMARY`.

- **Heavy / real-environment suite** — [`testcases/stress/`](testcases/stress/) (opt-in). The scenarios behind the verification table: parameter sweep, error/abnormal injection, per-level accuracy (~30&nbsp;GB), 150&nbsp;GB, restore accuracy (NONE & LZ4, 5&nbsp;GB / 16.7M rows), long-transaction before/after differential, log-phase content match, forced-wrong-boundary integrity, and commit-latency / `LOG_CS` occupancy. Run with:

  ```sh
  CUBRID=/path/to/_install/CUBRID bash testcases/stress/run_stress.sh [quick|standard|full]
  ```

  See [`testcases/stress/README.md`](testcases/stress/README.md) for the full suite ↔ verification-table mapping.

## Platform & limitations

- **Linux/POSIX only** — no Windows guards (uses `F_SETPIPE_SZ`, `fallocate`, `eventfd`, `unlink`-on-open).
- Buffering covers the **backup** path only (restore is unchanged).
- This is a short-term mitigation; moving the server's log copy out of `LOG_CS` is out of scope.
