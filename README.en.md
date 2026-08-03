**Languages:** [한국어](README.md) · English (this document)

# cubrid-backup-api

- [1. Overview](#1-overview)
- [2. What each public API does](#2-what-each-public-api-does)
- [3. Building and linking](#3-building-and-linking)
- [4. Structures and functions](#4-structures-and-functions)
- [5. Examples](#5-examples)
- [6. Configuration parameters](#6-configuration-parameters)
- [7. Example programs](#7-example-programs)
- [Appendix: repository layout](#appendix-repository-layout)

---

## 1. Overview

`cubrid-backup-api` is a C library that lets a third-party backup solution **move CUBRID backups in and out through API calls**. Instead of running `cubrid backupdb` yourself and then locating and copying the files it produced, you receive the backup stream through function calls and send it straight to tape, object storage, the network, or wherever you need it.

There are two directions.

**Backup — pulling the stream**

1. The library creates a FIFO (named pipe) and runs `cubrid backupdb -D <fifo>` as a child process.
2. `backupdb` writes the backup image into that pipe.
3. Your application receives the bytes with `cubrid_backup_read()` and sends them wherever it needs to.

The bytes you receive are exactly the bytes `backupdb` wrote. What you store through this API is therefore identical to a backup image produced by running `cubrid backupdb` directly.

**Restore — rebuilding the backup image**

Hand the bytes you archived back to `cubrid_restore_write()` and the library rebuilds them into a backup volume file, with the name and location CUBRID expects.

```
<backup_file_path>/<db_name>_bk<level>v000
```

> **Despite the name, this does not recover the database.** All `cubrid_restore_*` does is put the backup volume file back on disk; the recovery itself is performed afterwards by `cubrid restoredb`.
>
> If your application sent the bytes to tape or remote storage during backup, there is no backup file left on the local disk. `cubrid restoredb`, on the other hand, requires a local file. That is why this step — turning archived bytes back into a file — is needed.

### Required files

| File | Description |
|---|---|
| `cubrid_backup_api.h` | Public header |
| `libcubridbackupapi.so` | Shared library |
| `$CUBRID/conf/cubrid_backup.conf` | Behaviour configuration file (optional, [chapter 6](#6-configuration-parameters)) |

`$CUBRID` is the environment variable pointing at the CUBRID installation directory.

### Environment and constraints

- **Linux/POSIX only.** There are no Windows guards.
- **At most one backup and one restore may be in progress at a time, and a backup and a restore cannot run together.** Sequentially, you may repeat `begin`~`end` cycles as many times as you like in the same process (see [5.3](#53-restoring-an-incremental-chain)).
- The FIFO path does not include a PID, so **two processes backing up the same database at the same level on one host will collide.**

---

## 2. What each public API does

| Function | Role |
|---|---|
| `cubrid_backup_initialize()` | Starts using the library. Reads the configuration file and prepares internal state. |
| `cubrid_backup_begin()` | Starts one backup. Launches the `backupdb` child process and builds the handle that `cubrid_backup_read()` needs. |
| `cubrid_backup_read()` | Reads data from the backup stream. |
| `cubrid_backup_end()` | Ends or cancels the backup. Releases the handle and its resources. |
| `cubrid_restore_begin()` | Starts one restore. Creates the backup volume file to write and returns a handle. |
| `cubrid_restore_write()` | Writes archived backup data into that file. |
| `cubrid_restore_end()` | Ends the restore. Closes the file and releases the handle. |
| `cubrid_backup_finalize()` | Stops using the library. Pairs with `cubrid_backup_initialize()`. |

`initialize()` and `finalize()` are shared by both backup and restore.

**Backup call order**

```
cubrid_backup_initialize()
        │
        ├── cubrid_backup_begin()
        │        ├── cubrid_backup_read()   ← repeat until it returns 0
        │        └── cubrid_backup_end()
        │
        └── cubrid_backup_finalize()
```

**Restore call order**

```
cubrid_backup_initialize()
        │
        ├── cubrid_restore_begin()          ← may be repeated per level
        │        ├── cubrid_restore_write()   ← repeat in archived order
        │        └── cubrid_restore_end()
        │
        └── cubrid_backup_finalize()
```

---

## 3. Building and linking

### Building the library

```sh
./build.sh            # 64-bit release (RelWithDebInfo)
./build.sh -m debug   # debug build
```

The artifacts are produced at the path below, and a `.tar.gz` of the same content is written to `build_x86_64_<mode>/`.

```
build_x86_64_<mode>/_install/cubrid-backup-api/
    ├── cubrid_backup_api.h
    ├── libcubridbackupapi.so            → libcubridbackupapi.so.<major>.<minor> (symbolic link)
    └── libcubridbackupapi.so.<major>.<minor>    the actual library file
```

> `build.sh` runs `git clean -ffdx` when it starts. **New files must be `git add`ed or they are deleted during the build.**

### Runtime prerequisites

The library runs `$CUBRID/bin/cubrid` as a child process, so the following must be in place **when your application runs**.

- **The `$CUBRID` environment variable** — the CUBRID installation directory. The library uses it to locate the `cubrid` binary it executes as well as the configuration and log files. If it is not set, `cubrid_backup_initialize()` fails.
- **The target database** — the database you name in `db_name` must already exist.
- **Server state** — an online backup (`sa_mode=false`, the default) requires the target database server to be running. An offline backup (`sa_mode=true`) requires it to be stopped.

### Compiling and linking your application

Place the header and the library where your application can reference them and link against it. `pthread` is required as well.

```sh
gcc -o backup_sample backup_sample.c -I<header path> -L<library path> -lcubridbackupapi -lpthread
```

> **Put the source file before the `-l` options.** GNU ld resolves symbols in a single pass, so listing the library first fails to link with undefined-symbol errors.

With a Makefile:

```make
CC      = gcc
CFLAGS  = -I./include
LDFLAGS = -L./lib -lcubridbackupapi -lpthread

backup_sample: backup_sample.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
```

### Running

The dynamic library has to be findable.

```sh
LD_LIBRARY_PATH=./lib ./backup_sample
```

To make it permanent, register the path under `/etc/ld.so.conf.d/` and run `ldconfig`, or ship the `.so` inside your application package and set `RPATH`.

### Integration note — changes left on the calling process

When a backup starts, the library applies the following two changes to **the calling process itself**, and does not undo them after the backup finishes.

- **Process group change** — to be able to clean up child processes as a group, it makes the calling process a process-group leader (`setpgid`). If your application, or a supervisor above it, signals by process group, that behaviour can be affected.
- **`SIGCHLD` handler replacement** — to detect the exit of the child `cubrid` process, the library installs its own handler. If your application manages child processes of its own, **its existing `SIGCHLD` handler is overwritten.**

If your application manages its own child processes, consider running backups in a separate process.

---

## 4. Structures and functions

### 4.1 Structures and types

#### CUBRID_BACKUP_INFO

Holds the backup request information.

```c
typedef struct cubrid_backup_info CUBRID_BACKUP_INFO;
struct cubrid_backup_info
{
    int         backup_level;
    int         remove_archive;
    int         sa_mode;
    int         no_check;
    int         compress;
    const char* db_name;
};
```

| Member | Values | Description |
|---|---|---|
| `backup_level` | `0` / `1` / `2` | `0` = full backup, `1` = first incremental, `2` = second incremental |
| `remove_archive` | `-1` / `0` / `1` | Whether to delete archive logs no longer needed after the backup. `0` = keep, `1` = delete |
| `sa_mode` | `-1` / `0` / `1` | Backup execution mode. `0` = online (client/server), `1` = offline (standalone) |
| `no_check` | `-1` / `0` / `1` | Consistency check of the backup data. `0` = check, `1` = skip |
| `compress` | `-1` / `0` / `1` | Compression of the backup data. `0` = none, `1` = compressed (LZ4) |
| `db_name` | string | Name of the database to back up |

> **`-1` means "use the value from the configuration file".** Four members support it — `remove_archive`, `sa_mode`, `no_check` and `compress` — and the same-named entry in `cubrid_backup.conf` applies. Passing `0` or `1` overrides the configuration file.
>
> **Always initialise every member before passing the structure.** If you do not, `cubrid_backup_begin()` usually fails, but that is not guaranteed. If the garbage values of those four members happen to fall inside the valid range (`-1`~`1`), validation passes and **the backup runs with options you did not intend**, and if `db_name` is not a valid pointer **it can crash.**

#### CUBRID_RESTORE_INFO

Holds the restore request information.

```c
typedef struct cubrid_restore_info CUBRID_RESTORE_INFO;
struct cubrid_restore_info
{
    RESTORE_TYPE restore_type;
    int          backup_level;
    const char*  up_to_date;
    const char*  backup_file_path;
    const char*  db_name;
};
```

| Member | Values | Description |
|---|---|---|
| `restore_type` | `RESTORE_TO_FILE` | Restore method. See `RESTORE_TYPE` below |
| `backup_level` | `0` / `1` / `2` | Level of the backup data being restored |
| `up_to_date` | `NULL` | Point in time. Not supported yet. |
| `backup_file_path` | string | The **directory** in which to create the backup volume file. It must already exist. |
| `db_name` | string | Name of the target database |

#### RESTORE_TYPE

Enumeration that selects the restore method.

```c
typedef enum restore_type RESTORE_TYPE;
enum restore_type
{
    RESTORE_TO_DB,
    RESTORE_TO_FILE
};
```

| Value | Description |
|---|---|
| `RESTORE_TO_DB` | Restore directly into the database. Not supported yet. |
| `RESTORE_TO_FILE` | Write the backup data into a backup volume file. |

### 4.2 Functions

#### cubrid_backup_initialize()

Starts using the library. Call it once per process, before any other function. The configuration file is read and validated at this point.

```c
int cubrid_backup_initialize (void);
```

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

**Note** — **a single unrecognised entry in the configuration file makes this fail.** A typo in an entry name is the most common cause. A missing file is not a failure; every entry then takes its default.

#### cubrid_backup_begin()

Starts one backup. The `cubrid backupdb` child process is launched at this point.

```c
int cubrid_backup_begin (CUBRID_BACKUP_INFO* backup_info, void** backup_handle);
```

| Parameter | In/out | Description |
|---|---|---|
| `backup_info` | in | Backup request information. See [CUBRID_BACKUP_INFO](#cubrid_backup_info) |
| `backup_handle` | out | Handle to pass to `cubrid_backup_read()` and `cubrid_backup_end()` |

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

**Note** — fails if a backup is already in progress. If it cannot acquire buffer-related resources it does **not** fail; it degrades and continues (see [6.4](#64-verifying-that-the-configuration-took-effect)).

#### cubrid_backup_read()

Reads data from the backup stream. Call it repeatedly until it returns `0`.

```c
int cubrid_backup_read (void* backup_handle, void* buffer,
                        unsigned int buffer_size, unsigned int* data_len);
```

| Parameter | In/out | Description |
|---|---|---|
| `backup_handle` | in | Handle from `cubrid_backup_begin()` |
| `buffer` | out | User buffer to receive the backup data |
| `buffer_size` | in | Size of the buffer |
| `data_len` | out | Number of bytes actually placed in the buffer |

| Return | Meaning |
|---|---|
| `1` | Success. More data remains; call again. |
| `0` | Success. The backup is complete. `data_len` is `0` in this case. |
| `-1` | Failure |

**Note** — ignoring `-1` means **mistaking a truncated backup image for a good one.** Always check it.

#### cubrid_backup_end()

Ends the backup and releases the handle and its resources.

```c
int cubrid_backup_end (void* backup_handle);
```

| Parameter | In/out | Description |
|---|---|---|
| `backup_handle` | in | Handle from `cubrid_backup_begin()` |

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

**Note** — calling it before the stream has been read to the end is treated as a **cancellation**. The data received so far is not a complete backup image in that case.

Even when it acts as a cancellation, this function returns **`0`** as long as the cleanup itself succeeded. Its return value therefore tells you nothing about whether the backup is complete. Judge completeness by **whether `cubrid_backup_read()` returned `0`**.

#### cubrid_restore_begin()

Starts one restore. The file `<backup_file_path>/<db_name>_bk<backup_level>v000` is created at this point.

```c
int cubrid_restore_begin (CUBRID_RESTORE_INFO* restore_info, void** restore_handle);
```

| Parameter | In/out | Description |
|---|---|---|
| `restore_info` | in | Restore request information. See [CUBRID_RESTORE_INFO](#cubrid_restore_info) |
| `restore_handle` | out | Handle to pass to `cubrid_restore_write()` and `cubrid_restore_end()` |

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

**Note** — this function only creates the backup volume file; it does not recover the database. The recovery itself is performed with `cubrid restoredb` once the file is ready.

If a file already exists at that path, **its contents are truncated and overwritten.** The `backup_file_path` directory must already exist, and the file is created with permissions `0600`. Fails if a backup is in progress.

#### cubrid_restore_write()

Writes archived backup data into the backup volume file.

```c
int cubrid_restore_write (void* restore_handle, int backup_level,
                          void* buffer, unsigned int data_len);
```

| Parameter | In/out | Description |
|---|---|---|
| `restore_handle` | in | Handle from `cubrid_restore_begin()` |
| `backup_level` | in | Backup level of the data. **Must equal `CUBRID_RESTORE_INFO.backup_level`.** |
| `buffer` | in | User buffer holding the backup data |
| `data_len` | in | Number of bytes in the buffer |

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

**Note** — `backup_level` is **not** an argument that selects the output file; it is a guard against mistakes. If it differs from the handle's level the call fails. So **one handle means one level and one file**, and to restore a full+incremental chain you must run a separate `begin`~`end` cycle per level. The data must be handed over **in exactly the order it was read during backup**.

#### cubrid_restore_end()

Ends the restore and closes the file.

```c
int cubrid_restore_end (void* restore_handle);
```

| Parameter | In/out | Description |
|---|---|---|
| `restore_handle` | in | Handle from `cubrid_restore_begin()` |

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

#### cubrid_backup_finalize()

Stops using the library. Call it once per process, paired with `cubrid_backup_initialize()`. Call it even if you only used the restore functions.

```c
int cubrid_backup_finalize (void);
```

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Failure |

---

## 5. Examples

### 5.1 Full backup

```c
#include <stdio.h>
#include "cubrid_backup_api.h"

int main (void)
{
    CUBRID_BACKUP_INFO info;
    void *handle = NULL;
    char  buf[4096];
    unsigned int len = 0;
    int   rc;
    FILE *fp;

    info.backup_level   = 0;      /* full backup */
    info.remove_archive = -1;     /* -1 = take the value from cubrid_backup.conf */
    info.sa_mode        = -1;
    info.no_check       = -1;
    info.compress       = -1;
    info.db_name        = "demodb";

    fp = fopen ("demodb_bk0v000", "wb");
    if (fp == NULL)                                 { return 1; }
    if (cubrid_backup_initialize () == -1)          { return 1; }
    if (cubrid_backup_begin (&info, &handle) == -1) { return 1; }

    do
    {
        rc = cubrid_backup_read (handle, buf, sizeof (buf), &len);

        if (rc == -1)                             /* always check for failure */
        {
            cubrid_backup_end (handle);           /* clean up even on failure */
            cubrid_backup_finalize ();
            fclose (fp);
            return 1;
        }

        if (len > 0)  { fwrite (buf, 1, len, fp); }
    } while (rc != 0);                            /* 0 = backup complete */

    fclose (fp);
    if (cubrid_backup_end (handle) == -1)   { return 1; }
    if (cubrid_backup_finalize () == -1)    { return 1; }

    return 0;
}
```

In real use, the `fwrite` is replaced by a transfer to tape or object storage. The example writes to a local file only to make it easy to check.

> **Call `cubrid_backup_end()` even when an error occurs.** In a long-running program that repeats backups in one process, a handle left behind because `end()` was skipped can make every later `cubrid_backup_begin()` fail.
>
> **This example runs without any buffer configuration.** The buffer entries default to off, so if the receiving side might be slow in your environment, apply the buffer configuration from [chapter 6](#6-configuration-parameters) as well.

### 5.2 Restore

Rebuild archived bytes into a backup volume file.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "cubrid_backup_api.h"

int main (void)
{
    CUBRID_RESTORE_INFO info;
    void *handle = NULL;
    char  buf[4096];
    size_t len;
    FILE *src;

    info.restore_type     = RESTORE_TO_FILE;
    info.backup_level     = 0;
    info.up_to_date       = NULL;              /* not supported */
    info.backup_file_path = "./restore_dir";   /* must already exist */
    info.db_name          = "demodb";

    mkdir ("./restore_dir", S_IRWXU);

    src = fopen ("demodb_bk0v000", "rb");      /* the archived backup data */
    if (src == NULL)                                  { return 1; }
    if (cubrid_backup_initialize () == -1)            { return 1; }
    if (cubrid_restore_begin (&info, &handle) == -1)  { return 1; }

    while ((len = fread (buf, 1, sizeof (buf), src)) > 0)
    {
        /* the second argument must equal info.backup_level */
        if (cubrid_restore_write (handle, 0, buf, len) == -1)
        {
            cubrid_restore_end (handle);
            cubrid_backup_finalize ();
            fclose (src);
            return 1;
        }
    }

    fclose (src);
    if (cubrid_restore_end (handle) == -1)  { return 1; }
    if (cubrid_backup_finalize () == -1)    { return 1; }

    return 0;
}
```

Running this produces `./restore_dir/demodb_bk0v000`. Recovering the database itself is the next step.

```sh
cubrid restoredb -B ./restore_dir -l 0 demodb
```

> `-B` names the directory holding the backup volume and `-l` the level. This command overwrites the contents of the target database.

### 5.3 Restoring an incremental chain

Each level has its own handle, so repeat `begin`~`end` once per level.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "cubrid_backup_api.h"

/* Restore one level. 0 on success, -1 on failure. */
static int restore_one_level (int level, const char *src_path)
{
    CUBRID_RESTORE_INFO info;
    void  *handle = NULL;
    char   buf[4096];
    size_t len;
    FILE  *src;

    src = fopen (src_path, "rb");
    if (src == NULL) { return -1; }

    info.restore_type     = RESTORE_TO_FILE;
    info.backup_level     = level;
    info.up_to_date       = NULL;
    info.backup_file_path = "./restore_dir";
    info.db_name          = "demodb";

    if (cubrid_restore_begin (&info, &handle) == -1)
    {
        fclose (src);
        return -1;
    }

    while ((len = fread (buf, 1, sizeof (buf), src)) > 0)
    {
        /* the second argument must equal info.backup_level */
        if (cubrid_restore_write (handle, level, buf, len) == -1)
        {
            cubrid_restore_end (handle);
            fclose (src);
            return -1;
        }
    }

    fclose (src);
    return cubrid_restore_end (handle);
}

int main (void)
{
    mkdir ("./restore_dir", S_IRWXU);

    if (cubrid_backup_initialize () == -1) { return 1; }

    if (restore_one_level (0, "tape/demodb_bk0v000") == -1) { return 1; }
    if (restore_one_level (1, "tape/demodb_bk1v000") == -1) { return 1; }
    if (restore_one_level (2, "tape/demodb_bk2v000") == -1) { return 1; }

    if (cubrid_backup_finalize () == -1) { return 1; }

    return 0;
}
```

Once all three files are in place, give `restoredb` the final level.

```sh
cubrid restoredb -B ./restore_dir -l 2 demodb
```

---

## 6. Configuration parameters

The configuration file is `$CUBRID/conf/cubrid_backup.conf`, written under a `[backup]` section. If the file is absent, every entry takes its default. Sizes are written as `64KB`, `256MB`, `1GB`.

> **An unrecognised entry is not ignored — it makes `cubrid_backup_initialize()` fail.** Spell entry names exactly.
>
> The build does not install a configuration file. Create one yourself; `testcases/cubrid_backup.conf` holds an example.
>
> The file also syntactically recognises a `[restore]` section (`partial_recovery`, `use_database_location_path`), but those entries are unused and affect nothing today. Do not use them.

### 6.1 Parameter list

| Entry | Default | What it is for |
|---|---|---|
| `remove_archive` | `false` | Cleans up archive logs that are no longer needed once the backup finishes, to **keep log disk usage down**. |
| `sa_mode` | `false` | Used when the backup must run with the server stopped (offline backup). `true` runs it in standalone mode. |
| `no_check` | `false` | Skips the consistency check of the backup data to **shorten backup time**. Skipping it delays when a problem would be detected. |
| `thread_count` | `0` | Number of parallel threads `backupdb` uses, to **balance backup duration against server load**. `0` lets the server decide. |
| `compress` | `false` | Compresses the backup data with LZ4 to **reduce transfer volume and storage**. |
| `except_active_log` | `false` | Excludes the active log from the backup. |
| `sleep_msecs` | `0` | Inserts a deliberate delay during the backup to **lower load on a server in service**. |
| `fifo_size` | `64KB` | The **pipe size** between the library and `backupdb`, used to receive larger chunks at a time. Clamped to `[64KB, 1MB]`. Applies even when the buffer is not used. |
| `buffer_memory_size` **(buffer)** | `0` | **The entry that turns the buffer on and sets its size.** `0` means no buffer — the application reads the pipe directly. |
| `buffer_disk_limit` **(buffer)** | `0` | The **maximum size the buffer may extend onto disk** once memory is full. `0` means memory only. |
| `buffer_disk_path` **(buffer)** | *(empty)* | The **directory in which to create the file** the buffer uses when it extends onto disk. Must exist and be writable when `buffer_disk_limit` is greater than `0`. |
| `buffer_disk_keep_spool` **(buffer)** | `false` | Diagnostic entry. `true` keeps the disk file the buffer used instead of deleting it. |

The four entries marked **(buffer)** exist for the buffer that holds the backup data. They all default to off and work together, so read [6.2](#62-notes-on-the-buffer-parameters) before using them.

`remove_archive`, `sa_mode`, `no_check` and `compress` apply only when the same-named member of `CUBRID_BACKUP_INFO` is `-1`. Passing `0` or `1` in the member overrides the configuration file. The remaining entries have no corresponding member in the API structure, so the configuration file value always applies.

`thread_count` is meaningful for online backups (`sa_mode=false`). In standalone mode the server fixes the parallelism at 1, so the value has no effect.

> ⚠️ **The behaviour of `compress` changed in this version.** `backupdb` 11.3 and later **compresses with LZ4 by default** when no compression option is given. That is why `compress=false` previously still produced a compressed image. The library now passes "no compression" explicitly, so `compress=false` really means uncompressed.
> **With the default configuration, the backup image format changes from LZ4 to uncompressed.** Backup size and duration change with it, so re-check any capacity planning based on previous image sizes. To keep compression, set `compress=true` explicitly.

### 6.2 Notes on the buffer parameters

The entries covered here are `buffer_memory_size`, `buffer_disk_limit`, `buffer_disk_path` and `buffer_disk_keep_spool`.

The pipe is drained only when the application calls `cubrid_backup_read()`. So if the side receiving the backup data is slow, the pipe fills up and `backupdb` waits in that state, which can hold up other work on the server. The buffer entries exist **to break that coupling by draining the pipe on the application's behalf.** A dedicated thread pulls data out of the pipe into a buffer, and the application reads from the buffer. **They default to off, so turn them on only when you need them.**

**When using the disk extension**

- **`buffer_disk_limit` is meaningful only when `buffer_memory_size` is greater than `0`.** Specifying the disk extension without a memory buffer makes `cubrid_backup_initialize()` fail.
- **Space is reserved up front when the backup starts.** At `cubrid_backup_initialize()` time, if free space is less than `buffer_disk_limit`, only a warning is logged and initialization still succeeds. If the reservation actually fails when the backup starts, that is not treated as a failure either — the buffer falls back to memory only.
- **Pointing it at a RAM-backed filesystem such as `tmpfs` makes that space consume RAM, not disk.**
- **Do not leave `buffer_disk_keep_spool=true` on permanently.** The file left behind is the size of the whole `buffer_disk_limit`, not the amount actually used. Its name contains a PID, so a new file accumulates every time a backup runs in a new process, and nothing deletes them. The contents are the backup data itself (permissions `0600`). Turn it off and delete the leftover files once you have finished investigating.

**Choosing sizes**

- `buffer_memory_size` — size it against the delay you want the buffer to absorb, roughly `(delay to ride out) × (throughput of the receiving side)`. This memory stays allocated until the backup ends. **There is a minimum.** It must be at least the I/O unit computed from the filesystem holding the library's work directory (typically 32 KB), and that value can differ per host, so **a setting accepted on one server can be rejected on another.**
- `buffer_disk_limit` — size it against the amount memory alone cannot hold. How much you actually need can be read from the `disk_high_water` value described in [6.4](#64-verifying-that-the-configuration-took-effect).

### 6.3 Configuration examples

**1. Without the buffer**

Leave the configuration file out, or do not specify `buffer_memory_size`. The application reads the pipe directly.

**2. Memory buffer only**

```ini
[backup]
fifo_size=1MB
buffer_memory_size=64MB
```

**3. Memory plus disk buffer**

```ini
[backup]
fifo_size=1MB
buffer_memory_size=64MB
buffer_disk_limit=256MB
buffer_disk_path=/var/tmp/cubrid_backup_spool
buffer_disk_keep_spool=false
```

### 6.4 Verifying that the configuration took effect

A backup succeeds even when the configuration is wrong, so **you have to check the log to see whether the buffer is actually on.** The log file is `$CUBRID/log/cubrid_backup.log`.

`cubrid_backup_begin()` **does not fail when it cannot acquire buffer resources.** If a memory allocation or the disk file creation does not succeed, it logs a warning, degrades and continues. The backup image is fine, but the buffer is off.

When a backup runs through to `cubrid_backup_end()` normally, this line is written:

```
tiered buffer summary: log_phase=1 spilled=1 mem_high_water=67108864 disk_high_water=104857600 wait_count=12 lookahead=3 bytes_total=8927412224
```

| Item | Meaning |
|---|---|
| the line is **absent** | The buffer was off. Re-check the configuration. |
| `spilled` | `1` means memory overflowed and the disk extension was actually used. |
| `mem_high_water` | The fullest the memory buffer got, in bytes. Close to `buffer_memory_size` means there is no headroom. |
| `disk_high_water` | The fullest the disk extension got, in bytes. The measured basis for choosing `buffer_disk_limit`. |
| `wait_count` | How often the dedicated thread had to wait because the receiving side was slow. Grounds for increasing the buffer. |
| `bytes_total` | Total bytes handed to the application. Should match the size of what you stored. |
| `log_phase`, `lookahead` | Diagnostic values reflecting internal optimisation state. You do not need to interpret them. |

### 6.5 Common problems

| Symptom | Cause and remedy |
|---|---|
| `cubrid_backup_initialize()` returns `-1` | An unrecognised entry in the configuration file (a typo). Or `buffer_memory_size` is below the minimum, or `buffer_disk_limit` is set while `buffer_memory_size` is `0`. |
| `cubrid_backup_begin()` returns `-1` | Most likely `CUBRID_BACKUP_INFO` was not initialised. Set the four members (`remove_archive`/`sa_mode`/`no_check`/`compress`) explicitly. Or a backup is already in progress. |
| `cubrid_restore_write()` returns `-1` | The second argument differs from `CUBRID_RESTORE_INFO.backup_level`. |
| The backup works but there is no `tiered buffer summary:` | The buffer is off. Either `buffer_memory_size` is `0` or it was degraded for lack of resources. Check the warning lines in the log. |
| The backup suddenly got much bigger | The `compress` behaviour change (see [6.1](#61-parameter-list)). Set `compress=true` if you want compression. |
| Two backup files differ when compared byte by byte | That is expected. The backup stream contains values that differ per run, such as the run time. To check whether two backups match, restore each and compare the data. |

---

## 7. Example programs

`testcases/` contains working examples — code that actually uses the structures and functions described in chapter 4.

| File | Contents |
|---|---|
| `backup_tc01.c` | Full and incremental backup. Source behind the example in 5.1. |
| `backup_tc02.c` ~ `backup_tc04.c` | Argument combinations and exceptional cases |
| `backup_tc05.c` | Imitates a slow consumer to drive the buffer from memory onto disk, then compares the restored result |
| `restore_tc01.c` ~ `restore_tc03.c` | Restore. Source behind the example in 5.2. |
| `parser_ut.c` | Internal unit tests (no server needed) |
| `conf_test.sh` | Checks that each `cubrid_backup.conf` entry is reflected in the actual `backupdb` arguments (run by `run_test.sh`) |

### Building and running

```sh
bash testcases/run_test.sh
```

Builds the API and the example programs, creates a 100 MB `testdb`, and runs the backup and restore scenarios in order. It finishes within a few minutes and prints `ALL PASSED` or `FAILED TEST SUMMARY`. `expect` must be installed.

> ⚠️ **Run this only against a throw-away CUBRID installation.** The script calls `cubrid service stop`, which **stops every database on that installation**, deletes `$CUBRID/conf/cubrid_backup.conf`, empties `$CUBRID/log/`, and drops `testdb`. Running it on a shared development server will destroy other people's configuration and logs.

How to run each program individually is documented in its own `usage`.

```sh
./backup_tc01  demodb 0 ./backup_dir/demodb_bk0v000
./restore_tc01 demodb 0 ./backup_dir/demodb_bk0v000 0 ./restore_dir
```

### Real-environment verification suite

```sh
CUBRID=/path/to/CUBRID bash testcases/stress/run_stress.sh [quick|standard|full]
```

Eleven suites (`s01`~`s11`) cover configuration-value combinations, error injection, per-level and restore accuracy, large-volume endurance, commit-latency measurement and more.

> ⚠️ **Run these only against a throw-away CUBRID installation.** This suite also creates and drops databases and modifies configuration and log files. The large-volume suites can use several hundred GB of disk. See [`testcases/stress/README.md`](testcases/stress/README.md) for what each suite does.

---

## Appendix: repository layout

```
src/               API implementation and headers
testcases/         example programs, run_test.sh, sample conf
testcases/stress/  real-environment verification suite (s01~s11)
build.sh           package build script
VERSION            package version
README.md          Korean document
README.en.md       this document
```
