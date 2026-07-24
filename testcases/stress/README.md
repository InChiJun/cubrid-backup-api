# Stress / real-environment verification suite

The opt-in, heavy counterpart to `testcases/run_test.sh` (the fast 100 MB regression gate). These are the scenarios that produced the project's verification results: they provision their own — sometimes very large — databases, take minutes to hours, and are **deliberately not** part of the quick gate.

Each suite is a self-contained script (`sNN_*.sh`) that can run on its own; `run_stress.sh` orchestrates the whole set.

---

## How to run

Prerequisite: a **test** CUBRID install (not a production server). Point `CUBRID` at it.

```sh
# whole set, default profile:
CUBRID=/path/to/_install/CUBRID bash run_stress.sh

# choose how much to run:
CUBRID=... bash run_stress.sh quick       # cheapest suites only
CUBRID=... bash run_stress.sh standard    # default: everything except the giants
CUBRID=... bash run_stress.sh full        # standard + the ~30GB and 150GB suites

# run specific suites only:
CUBRID=... bash run_stress.sh s01_param_sweep s09_fault_boundary

# run one suite directly (suites take their own args):
CUBRID=... bash s06_accuracy_5g.sh true   # true=LZ4, false=NONE
CUBRID=... bash s07_longtxn_diff.sh commit
```

**Profiles**

| Profile | Includes |
|---|---|
| `quick` | `s01`, `s08`, `s09` |
| `standard` *(default)* | `quick` + `s02`, `s03`, `s06`×(NONE,LZ4), `s07`×(abort,commit), `s10`, `s11` |
| `full` | `standard` + `s04` (~30 GB), `s05` (150 GB) |

**Environment variables**

| Var | Required | Default | Meaning |
|---|---|---|---|
| `CUBRID` | yes | — | Target CUBRID install (`.../build_x86_64_release/_install/CUBRID`). |
| `API` | no | repo root (`../..` from this dir) | Source tree the suites build the `.so` from. |
| `W` | no | `/tmp/cbstress/<sub>` | Per-suite work directory. |
| `OUT` | no | `/tmp/cbstress/_run` | Where `run_stress.sh` writes per-suite logs + the summary. |

**Results.** `run_stress.sh` gives each suite its own timeout and log, appends a `PASS`/`FAIL` verdict to a summary table, and continues even if one suite fails (so a single run captures the whole picture). A suite is `PASS` when its log shows `RESULT: ALL-PASS` (or a `VERDICT`/all-`[OK]`-no-`[NOK]`) **and** it exited 0; a non-zero exit, a timeout, or any `[NOK]` is a `FAIL`. Full logs live under `$OUT`.

Each suite backs up the target's `cubrid_backup.conf` / `cubrid.conf` on entry and restores them on exit, but still run against a throwaway instance.

---

## What each suite tests

| Suite | What it verifies | Args | Scale |
|---|---|---|---|
| `s01_param_sweep.sh` | Config sweep: every `fifo_size` / `buffer_memory_size` / `buffer_disk_limit` / `buffer_disk_path` / `buffer_disk_keep_spool` value — valid ones complete + restore-match + log the expected mode, invalid ones are rejected fail-fast. | — | small |
| `s02_error_inject.sh` | Error / abnormal paths: bad spool path rejected at init, mid-run disk-reservation failure degrades to memory-only + still completes, backup-handle reuse, server down at start, client killed mid-stream. | — | small–med |
| `s03_level_error.sh` | Kill `backupdb` / kill server / undersized spool at backup **levels 1 & 2**, plus a minimum-ring (`mem = io_size`) + stalled-during-data-phase consumer to torture the probe/carry path. | — | med |
| `s04_levels_30g.sh` | Per-level accuracy at ~30 GB with checkpoint/cleanup disabled (full log streaming): levels 0/1/2 each restore to a full sorted-dump `sha256` that matches the source. | — | **~30 GB** |
| `s05_150g.sh` | 150 GB uncompressed — the buffer actually carries ~150 GB — verified with an aggregate fingerprint (count/min/max) + a 1-in-1000 sampled sorted hash + restoredb page checksums. | — | **~150 GB** |
| `s06_accuracy_5g.sh` | Restore data accuracy by querying the restored DB directly: row counts, per-row content hash (corruption = 0), and per-level point-in-time isolation, incl. a **full 16.7M-row** scan on the large table and the incremental (SNAP) archive-chain restore. Deliberately includes Korean/multibyte payload to prove multibyte content round-trips. | `false`=NONE, `true`=LZ4 | large (5 GB) |
| `s07_longtxn_diff.sh` | Long transaction spanning archives, handled after backup: the restored result of the **pre-improvement** API equals the improved API's, row for row (direct proof the change didn't alter the backup image). | `abort` \| `commit` | med |
| `s08_logmatch.sh` | The log-phase boundary the API records lines up (within tolerance) with where the log actually starts in the on-disk backup; archived/active log bytes match the on-disk log. | — | med |
| `s09_fault_boundary.sh` | Forces the boundary to the wrong place and disables the real parser, yet the restored data is still byte-intact (with a normal control run) — proves the parser is observational and can't corrupt data. | — | med |
| `s10_commit_latency.sh` | Under a slow consumer, compares `LOG_CS` occupancy and worst-case commit latency with vs. without the buffer (disk reserve ≥ remaining log). | — | med |
| `s11_e2e_payload.sh` | Drives mem-ring wrap + disk spill + lock-free pop under a slow consumer for LZ4, NONE, and a buffering-off control, comparing a full sorted-dump `sha256` of source vs. each restored backup. | — | med |

`helpers/` — auxiliary sources used by suites: `errcli.c` (`s02`), `bkwalk.c` (`s08`).

---

## Notes

- These suites are ports of already-verified scenarios: only the paths were parameterized (`$CUBRID` / `$API` / `$W`); the test logic is unchanged from the originals.
- The one-off diagnostic / iteration scripts from development are **not** vendored here — only the canonical suites above.
