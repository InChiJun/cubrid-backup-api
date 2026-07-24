# cubrid-backup-api

*A C API that drives `cubrid backupdb` over a FIFO and streams the backup image to the caller — with an in-process **tiered buffer** and an observational **log-phase parser** that keep a slow backup consumer from stalling the server.*

**Languages:** English (this file) · [한국어](README.ko.md)

---

## Overview

`cubrid-backup-api` lets an application take a CUBRID backup programmatically. It launches `cubrid backupdb -D <fifo>`, drains the backup stream from the pipe, and hands the bytes to the caller through `cubrid_backup_read()` (and the mirror path for restore).

The core addition over a plain pipe drain is a **tiered buffer** (memory ring → disk spool) fed by a dedicated drain thread, plus an **observational log-phase parser** that watches the stream to tell the *data* portion from the *transaction-log* portion and picks the right spill policy. Both are **Linux/POSIX only**.

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

## Public API

The public header (`src/include/backup_api.h`, installed as `cubrid_backup_api.h`) is unchanged by this work:

```c
int cubrid_backup_initialize (void);
int cubrid_backup_begin (CUBRID_BACKUP_INFO *backup_info, void **backup_handle);
int cubrid_backup_read  (void *backup_handle, void *buffer,
                         unsigned int buffer_size, unsigned int *data_len);
int cubrid_backup_end   (void *backup_handle);

int cubrid_restore_begin (CUBRID_RESTORE_INFO *restore_info, void **restore_handle);
int cubrid_restore_write (void *restore_handle, int backup_level,
                          void *buffer, unsigned int data_len);
int cubrid_restore_end   (void *restore_handle);

int cubrid_backup_finalize (void);
```

The buffer is transparent to callers: read the stream until `cubrid_backup_read` reports end-of-backup, exactly as before.

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

## Verification status

The feature has been verified across the scenarios in the project's verification table — 11 categories, all passing: unit tests, config validation, error/abnormal injection, per-level accuracy at ~30&nbsp;GB and 150&nbsp;GB, restore accuracy on both NONE and LZ4 streams (full 16.7M-row content scan), before/after differential against the pre-improvement API, log-phase content match against the on-disk log, forced-wrong-boundary byte integrity, and `LOG_CS`-occupancy / commit-latency reduction under a slow consumer. The suites that reproduce this evidence live under [`testcases/stress/`](testcases/stress/) (see its README for the suite ↔ table mapping).

## Platform & limitations

- **Linux/POSIX only** — no Windows guards (uses `F_SETPIPE_SZ`, `fallocate`, `eventfd`, `unlink`-on-open).
- Buffering covers the **backup** path only (restore is unchanged).
- This is a short-term mitigation; moving the server's log copy out of `LOG_CS` is out of scope.

## Repository layout

```
src/               API implementation (backup_api / backup_core / backup_manager / handle_manager) + headers
testcases/         run_test.sh (fast gate) + tc/restore clients + parser_ut + conf_test + sample conf
testcases/stress/  opt-in heavy / real-environment suite (run_stress.sh + s01..s11 + helpers)
build.sh           package build script
VERSION            package version
```
