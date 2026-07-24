#!/bin/bash
# ============================================================================
#  run_stress.sh — comprehensive driver for the HEAVY / real-environment
#                  tiered-buffer + log-phase-parser verification suite.
#
#  This is the opt-in counterpart to testcases/run_test.sh (the fast 100M-scale
#  regression gate). It runs the s01..s11 suites below — the same suites that
#  produced the project's verification table — each with its own
#  timeout + log file, appends a PASS/FAIL verdict to a summary table, and keeps
#  going even if one suite fails (so one run captures the full picture).
#
#  These suites provision their own (sometimes very large) DBs and take minutes
#  to HOURS. They are deliberately NOT part of run_test.sh.
#
#  Usage:
#    CUBRID=/path/to/_install/CUBRID bash run_stress.sh [PROFILE | suite ...]
#      PROFILE = quick     : cheapest suites only (params, logmatch, fault)
#                standard  : quick + errors, levels-error, accuracy(5GB),
#                            longtxn-diff, commit-latency, e2e   [DEFAULT]
#                full      : standard + the giants (levels ~30GB, 150GB)
#    Or pass explicit suite basenames, e.g.:
#      CUBRID=... bash run_stress.sh s01_param_sweep s09_fault_boundary
#
#  Env passthrough: CUBRID (required), API (default: repo root), W (per-suite
#  work dir; each suite defaults its own under /tmp/cbstress/<sub>).
# ============================================================================
set -u

: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
export LD_LIBRARY_PATH="$CUBRID/lib:${LD_LIBRARY_PATH:-}"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}

OUT=${OUT:-/tmp/cbstress/_run}; rm -rf "$OUT"; mkdir -p "$OUT"
SUM=$OUT/SUMMARY.txt; TBL=$OUT/TABLE.txt; : > "$SUM"; : > "$TBL"
log(){ echo "[$(date '+%H:%M:%S')] $*" | tee -a "$SUM"; }

# ── verdict LOGFILE : best-effort PASS/FAIL by scanning a suite log ──────────
# Suites use the [OK]/[NOK] + "RESULT: ALL-PASS" convention; a few (differential
# / latency / fault-injection) print their own VERDICT line. Unrecognized =>
# INCONCLUSIVE (review the log). Build/setup failures are called out explicitly.
verdict(){ local f=$1
  grep -qE 'BUILD_FAIL|CLIENT_FAIL|CREATEDB_FAIL|ERRCLI_FAIL|BKWALK_FAIL|BASE_EXTRACT_FAIL|FIX_BUILD_FAIL|BASE_BUILD_FAIL' "$f" 2>/dev/null \
    && { echo "FAIL(build/setup)"; return; }
  local nok; nok=$(grep -c '\[NOK\]' "$f" 2>/dev/null)
  [ "${nok:-0}" -gt 0 ] && { echo "FAIL(${nok} NOK)"; return; }
  grep -qE 'RESULT: *ALL-PASS' "$f" 2>/dev/null && { echo "PASS"; return; }
  grep -qE 'VERDICT.*(PASS|identical|byte-intact|match)|per-level correctness: PASS' "$f" 2>/dev/null && { echo "PASS"; return; }
  local ok; ok=$(grep -c '\[OK\]' "$f" 2>/dev/null)
  [ "${ok:-0}" -gt 0 ] && { echo "PASS(${ok} OK, no NOK)"; return; }
  echo "INCONCLUSIVE(see log)"
}

run_suite(){ local name=$1 to=$2; shift 2; local f=$OUT/$name.log
  log ">>> START $name (timeout ${to}s)"
  timeout "$to" bash "$SCRIPT_DIR/$name.sh" "$@" > "$f" 2>&1; local rc=$?
  local v; v=$(verdict "$f")
  if [ "$rc" = 124 ]; then
    v="FAIL(timeout ${to}s)"
  elif [ "$rc" -ne 0 ]; then
    # non-zero, non-timeout exit (set -u abort, segfault, bare `exit 1` with no
    # [NOK]/token): never let a stray [OK] count read as PASS — the suite died.
    case "$v" in PASS*) v="FAIL(rc=$rc after $v)";; esac
  fi
  log "<<< DONE  $name : $v (rc=$rc)  [log: $f]"
  printf '%-24s %s\n' "$name" "$v" >> "$TBL"
}

# ── suite catalogue: name  timeout  args... ─────────────────────────────────
# (ordered cheap -> expensive within each profile)
declare -a QUICK=(
  "s09_fault_boundary 2400"
  "s01_param_sweep    1800"
  "s08_logmatch       1500"
)
declare -a STD_EXTRA=(
  "s03_level_error    1500"
  "s02_error_inject   1800"
  "s07_longtxn_diff   2400 abort"
  "s07_longtxn_diff   2400 commit"
  "s06_accuracy_5g    6000 false"
  "s06_accuracy_5g    6000 true"
  "s10_commit_latency 1800"
  "s11_e2e_payload    1800"
)
declare -a GIANTS=(
  "s04_levels_30g     18000"
  "s05_150g           43200"
)

PROFILE=standard
declare -a EXPLICIT=()
for a in "$@"; do
  case "$a" in
    quick|standard|full) PROFILE=$a ;;
    s[0-9][0-9]_*) EXPLICIT+=("$a 6000") ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

declare -a PLAN=()
if [ "${#EXPLICIT[@]}" -gt 0 ]; then
  PLAN=("${EXPLICIT[@]}")
else
  PLAN=("${QUICK[@]}")
  [ "$PROFILE" != quick ] && PLAN+=("${STD_EXTRA[@]}")
  [ "$PROFILE" = full ]   && PLAN+=("${GIANTS[@]}")
fi

log "===== stress campaign start : profile=$PROFILE, ${#PLAN[@]} suite-runs ====="
log "      CUBRID=$CUBRID"
log "      API=$API"
for entry in "${PLAN[@]}"; do
  # shellcheck disable=SC2086
  run_suite $entry
done

echo | tee -a "$SUM"
log "===== FINAL TABLE ====="
cat "$TBL" | tee -a "$SUM"
PASS=$(grep -cE '  *PASS' "$TBL"); TOT=$(wc -l < "$TBL")
log "===== $PASS/$TOT suite-runs PASS (full logs under $OUT) ====="
[ "$PASS" = "$TOT" ]
