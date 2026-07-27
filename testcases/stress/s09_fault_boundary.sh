#!/bin/bash
# ============================================================================
#  stress suite 09 — wrong-boundary fault injection
#  source: logphase_stress/phase3_faultinject.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s09_fault_boundary.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# GAP-CLOSER #6: prove the observational-parser SAFETY property directly.
# The parser's only output (log_phase) selects spill policy, never the bytes
# delivered. So a DELIBERATELY WRONG boundary must still restore byte-identically.
# We inject a test-only hook into a COPY of the feature source (feature branch
# stays clean): force log_phase=true at offset 0 (before any data volume — a
# maximally wrong boundary) and disable the real parser. Then back up + restore a
# self-validating DB with the WRONG-phase .so and assert 0 corrupted, and compare
# to a control build (no injection). Both must be fully intact.
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/p3}; DB=${DB:-p3}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
SRC="$API"
export LD_LIBRARY_PATH=$W/libinj:$C3/lib
SVCONF=$C3/conf/cubrid.conf; CONF=$C3/conf/cubrid_backup.conf
rm -rf $W; mkdir -p $W/libinj $W/libctl $W/hdr $W/spool $W/bk $W/db $W/injsrc
ORIG=$W/confbak; mkdir -p $ORIG; cp $SVCONF $ORIG/sv.bak; [ -f $CONF ] && cp $CONF $ORIG/bk.bak || rm -f $ORIG/bk.bak 2>/dev/null
trap 'cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; cubrid master stop >/dev/null 2>&1; cp $ORIG/sv.bak "$SVCONF"; [ -f $ORIG/bk.bak ] && cp $ORIG/bk.bak "$CONF" || rm -f "$CONF"' EXIT
cd $W/db

# 1) copy feature source and inject the test-only wrong-phase hook into the COPY
cp -r $SRC/src $W/injsrc/src
# insert the hook right after the anchor line (outer drain loop body, runs first iteration)
awk -v hook="        if (getenv(\"CUB_BK_FORCE_PHASE_EARLY\") && !backup_handle->log_phase) { enter_log_phase (backup_handle); backup_handle->parser_on = false; }" \
  '{print} /long long disk_tail_offset = 0, disk_room = 0;/{print hook}' \
  $SRC/src/backup_core.c > $W/injsrc/src/backup_core.c
_inj=$(grep -c CUB_BK_FORCE_PHASE_EARLY $W/injsrc/src/backup_core.c)
echo "### hook injected: $_inj site(s)"
# Without this the awk anchor can silently stop matching after a refactor: the
# "injected" build would equal the control and the suite would PASS having
# proved nothing.
[ "$_inj" -eq 1 ] || { echo "  [NOK] wrong-phase hook NOT injected ($_inj sites) - test would be vacuous"; exit 1; }
grep -q 'stdlib.h' $W/injsrc/src/backup_core.c || sed -i '1i #include <stdlib.h>' $W/injsrc/src/backup_core.c

# 2) build INJECTED .so (from copy) and CONTROL .so (from clean feature source)
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $W/injsrc/src/include -O2 -o $W/libinj/libcubridbackupapi.so \
  $W/injsrc/src/backup_api.c $W/injsrc/src/backup_core.c $W/injsrc/src/backup_manager.c $W/injsrc/src/handle_manager.c -lpthread || { echo INJ_BUILD_FAIL; exit 1; }
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $SRC/src/include -O2 -o $W/libctl/libcubridbackupapi.so \
  $SRC/src/backup_api.c $SRC/src/backup_core.c $SRC/src/backup_manager.c $SRC/src/handle_manager.c -lpthread || { echo CTL_BUILD_FAIL; exit 1; }
cp $SRC/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $SRC/testcases/backup_tc05.c -L $W/libinj -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }

cp $SVCONF $ORIG/sv0; sed -i "s/^cubrid_port_id=.*/cubrid_port_id=1599/" $SVCONF
grep -q '^log_max_archives=' $SVCONF && sed -i "s/^log_max_archives=.*/log_max_archives=500/" $SVCONF || echo "log_max_archives=500" >> $SVCONF
# small mem buffer so the ring fills and the (forced) disk tier actually engages
cat > $CONF <<E
[backup]
remove_archive=false
compress=true
thread_count=1
buffer_memory_size=4MB
buffer_disk_limit=1GB
buffer_disk_path=$W/spool
E
SQL(){ csql -u dba -N -c "$1" $DB 2>/dev/null | grep -vE '^===|rows selected|Committed|^[[:space:]]*$'; }
q1(){ SQL "$1" | tail -1 | tr -d " '"; }

echo "### createdb + ~1M self-validating rows (enough to fill mem + spill under forced phase)"
cubrid createdb -r --db-volume-size=1G --log-volume-size=100M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(300), ck VARCHAR(32));";
  echo "INSERT INTO t VALUES (1, RPAD('x',290,'y'), NULL);";
  for i in $(seq 1 20); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b, NULL FROM t;"; done  # ~1M rows
  echo "UPDATE t SET ck=MD5(CAST(a AS VARCHAR)||'#'||b);"; } | csql -u dba $DB >/dev/null 2>&1
ROWS=$(q1 "SELECT COUNT(*) FROM t"); echo "### rows=$ROWS"

# helper: backup with a given lib + env, restore, full self-validation
run_case(){ local tag=$1 lib=$2 env=$3
  export LD_LIBRARY_PATH=$W/$lib:$C3/lib
  : > $C3/log/cubrid_backup.log 2>/dev/null
  env $env $W/tc05 $DB 0 $W/bk/${DB}_bk0v000 800 8192 > $W/bk_$tag.out 2>&1
  local armed=$(grep -c 'log-phase boundary detected' $C3/log/cubrid_backup.log)
  cubrid server stop $DB >/dev/null 2>&1; ls $W/db/${DB}* 2>/dev/null | xargs -r rm -f
  printf '1\n1\n1\n1\n' | cubrid restoredb -p -B $W/bk -l 0 $DB > $W/rest_$tag.out 2>&1
  cubrid server start $DB > $W/start_$tag.out 2>&1
  local n=$(q1 "SELECT COUNT(*) FROM t"); local bad=$(q1 "SELECT COUNT(*) FROM t WHERE COALESCE(ck,'Z')<>MD5(CAST(a AS VARCHAR)||'#'||b)")
  echo "  [$tag] boundary-log-lines=$armed  restored rows=$n  FULL-SCAN corrupted=$bad  (backup=$(stat -c%s $W/bk/${DB}_bk0v000 2>/dev/null)B)"
  echo "$bad|$n"
}

echo; echo "=== CONTROL: clean feature .so, real parser (correct boundary) ==="
CTL=$(run_case control libctl "" | tail -1)
echo "=== INJECTED: forced WRONG phase at offset 0, real parser disabled ==="
# restore the DB first (control wiped it)
cubrid server stop $DB >/dev/null 2>&1; ls $W/db/${DB}* 2>/dev/null | xargs -r rm -f
printf '1\n1\n1\n1\n' | cubrid restoredb -p -B $W/bk -l 0 $DB >/dev/null 2>&1; cubrid server start $DB >/dev/null 2>&1
# rebuild the table fresh (control left it restored, but rebuild to be safe & identical)
INJ=$(run_case injected libinj "CUB_BK_FORCE_PHASE_EARLY=1" | tail -1)

cbad=${CTL%%|*}; ibad=${INJ%%|*}; irows=${INJ##*|}
echo
if [ "$cbad" = 0 ] && [ "$ibad" = 0 ] && [ "$irows" = "$ROWS" ]; then
  echo "### VERDICT: PASS — forced WRONG boundary still restores byte-intact (corrupted=0, rows=$irows==$ROWS); control also 0."
else
  echo "  [NOK] forced-boundary restore not byte-intact"
  echo "### VERDICT: FAIL — control_corrupted=$cbad injected_corrupted=$ibad injected_rows=$irows expected=$ROWS"
  exit 1
fi
