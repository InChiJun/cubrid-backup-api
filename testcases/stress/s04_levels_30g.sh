#!/bin/bash
# ============================================================================
#  stress suite 04 — per-level accuracy (~30GB)
#  source: logphase_stress/test_levels.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s04_levels_30g.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── backup-level (0/1/2) correctness + functionality at ~30GB scale (CUBRID 11.3) ──
# Same 30GB-scale recipe as run_dml_stress5: checkpoint+vacuum disabled pins
# first_arv_needed at DB creation, so EVERY level's backup streams the full
# archive set (~30GB) through the tiered buffer.
# Per level L in 0,1,2:
#   modify data -> record expected hash H[L] -> API backup level L (buffered)
#   -> check parser armed + summary in log
# Then restore -l 2 / 1 / 0 (volumes+logs wiped first => state at backup time)
# and compare each restored hash to H[L].
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/lvl}; DB=${DB:-lvt}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
export LD_LIBRARY_PATH=$W/lib:$C3/lib
CONF=$C3/conf/cubrid_backup.conf; SVCONF=$C3/conf/cubrid.conf; LOG=$C3/log/cubrid_backup.log
TARGET_ARCH=60; FAIL=0
R(){ echo "[$(date +%H:%M:%S)] ### $*"; }
ok(){ echo "  [OK] $*"; }
nok(){ echo "  [NOK] $*"; FAIL=1; }

rm -rf $W; mkdir -p $W/lib $W/hdr $W/spool $W/db $W/bk; cd $W/db
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread || { echo BUILD_FAIL; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id $PORT
setp log_max_archives 1000
setp checkpoint_interval_in_mins 100000
setp checkpoint_every_size 1024G
setp vacuum_disable yes

cleanup(){ cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1
  cubrid master stop >/dev/null 2>&1
  cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"
  rm -rf $W/spool/* ; }
trap cleanup EXIT

cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=true
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=64MB
buffer_disk_limit=8GB
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E

R "1. createdb $DB (1G vol / 512M log) + base data"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=1G --log-volume-size=512M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1 || { echo SERVER_START_FAIL; exit 1; }
{ echo "CREATE TABLE churn(a INT PRIMARY KEY, b VARCHAR(220));"; echo "INSERT INTO churn VALUES (1, RPAD('x',210,'y'));";
  for i in $(seq 1 20); do echo "INSERT INTO churn SELECT a+(SELECT MAX(a) FROM churn), b FROM churn;"; done; } | csql -u dba $DB >/dev/null 2>&1

R "2. pin archives to >= $TARGET_ARCH (~30GB stream per backup)"
pass=0
while :; do
  na=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
  [ "$na" -ge "$TARGET_ARCH" ] && break
  [ "$pass" -ge 4000 ] && { echo "  cap hit archives=$na"; break; }
  echo "UPDATE churn SET b=RPAD(CHR(65+MOD(a+$pass,26)),210,'z');" | csql -u dba $DB >/dev/null 2>&1
  pass=$((pass+1))
done
R "   archives=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l), db size=$(du -sh $W/db | awk '{print $1}')"

dump(){ csql -u dba -N -c "SELECT a,b FROM churn ORDER BY a" $DB 2>/dev/null | grep -vE 'rows selected|Committed|^[[:space:]]*$'; }

# level L backup: expected-hash snapshot -> buffered API backup -> log checks
declare -A H
do_level(){ local L=$1
  H[$L]=$(dump | sha256sum | awk '{print $1}')
  : > $LOG 2>/dev/null
  local t0=$(date +%s)
  $W/tc05 $DB $L $W/bk/${DB}_bk${L}v000 0 1048576 > $W/bk/l$L.out 2>&1
  local t1=$(date +%s)
  grep -q '\[OK\]' $W/bk/l$L.out \
    && ok "L$L backup completed: $(stat -c%s $W/bk/${DB}_bk${L}v000) bytes in $((t1-t0))s" \
    || nok "L$L backup failed: $(tail -1 $W/bk/l$L.out)"
  grep -q 'log-phase boundary detected' $LOG && ok "L$L parser armed" || nok "L$L parser NOT armed"
  grep 'tiered buffer summary' $LOG | head -1 | sed 's/.*tiered/  [info] L'"$L"' tiered/'
}

R "3. LEVEL 0 (full)"
do_level 0

R "4. modify set A (10% update + 1000 inserts) -> LEVEL 1"
{ echo "UPDATE churn SET b=RPAD('L1',210,'q') WHERE MOD(a,10)=3;";
  echo "INSERT INTO churn SELECT a+2000000, RPAD('newL1',210,'n') FROM churn WHERE a<=1000;"; } | csql -u dba $DB >/dev/null 2>&1
do_level 1

R "5. modify set B (different 10% + deletes) -> LEVEL 2"
{ echo "UPDATE churn SET b=RPAD('L2',210,'w') WHERE MOD(a,10)=7;";
  echo "DELETE FROM churn WHERE a>2000500 AND a<=2001000;"; } | csql -u dba $DB >/dev/null 2>&1
do_level 2

R "6. restore each level (volumes+logs wiped => state at that backup)"
s04_live(){ timeout 5 csql -u dba -N -c "SELECT 1 FROM db_root" $DB 2>/dev/null | grep -qE '^[[:space:]]*[0-9]+'; }
s04_wait_db(){ local _e=$((SECONDS+600)); while [ $SECONDS -lt $_e ]; do s04_live && return 0; sleep 2; done; return 1; }
for L in 2 1 0; do
  cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  t0=$(date +%s)
  printf '0\n0\n0\n0\n0\n0\n' | cubrid restoredb -B $W/bk -l $L $DB > $W/restore_l$L.out 2>&1
  rrc=$?
  # Separate "restore never ran" from "data differs"; otherwise an aborted
  # restore is reported as a hash MISMATCH, i.e. reads as data corruption.
  if grep -qE 'Unable to mount|Backup volume|not found|following' $W/restore_l$L.out; then
    nok "restore -l $L ABORTED (missing volume/archive during roll-forward); see restore_l$L.out"; continue
  fi
  if [ "$rrc" -ne 0 ]; then nok "restore -l $L failed rc=$rrc; see restore_l$L.out"; continue; fi
  cubrid server start $DB >/dev/null 2>&1
  s04_wait_db || { nok "restore -l $L: server not ready in 600s"; continue; }
  t1=$(date +%s)
  HR=$(dump | sha256sum | awk '{print $1}')
  if [ "$HR" = "${H[$L]}" ]; then ok "restore -l $L: hash matches expected state ($((t1-t0))s)"
  else nok "restore -l $L MISMATCH (rc=$rrc): got=$HR want=${H[$L]} ; $(tail -2 $W/restore_l$L.out | tr '\n' ' ')"; fi
done

R "7. NO-CHANGE incremental (L1 right after the -l 0 restore, zero DML)"
# reviewer finding: a changeless incremental may stream no data-volume FILE_START,
# so the parser must never arm (log_phase=0) — that must be SAFE, not an error.
H_NC=$(dump | sha256sum | awk '{print $1}')
: > $LOG 2>/dev/null
mkdir -p $W/bknc; cp $W/bk/${DB}_bk0v000 $W/bknc/
$W/tc05 $DB 1 $W/bknc/${DB}_bk1v000 0 1048576 > $W/bknc/nc.out 2>&1
grep -q '\[OK\]' $W/bknc/nc.out && ok "NC L1 (no changes) completed: $(stat -c%s $W/bknc/${DB}_bk1v000) bytes" || nok "NC L1 failed"
grep -q 'parser disabled' $LOG && nok "NC L1 parser DISABLED (should be never-armed, not disabled)" || ok "NC L1 no parser-disable warn"
grep -oE 'log_phase=[01]' $LOG | head -1 | sed 's/^/  [info] NC L1 /'
cubrid server stop $DB >/dev/null 2>&1
rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
printf '0\n0\n0\n0\n0\n0\n' | cubrid restoredb -B $W/bknc -l 1 $DB > $W/restore_nc.out 2>&1
cubrid server start $DB >/dev/null 2>&1
HNC_R=$(dump | sha256sum | awk '{print $1}')
[ "$HNC_R" = "$H_NC" ] && ok "NC L1 restore hash matches (changeless incremental safe)" || nok "NC L1 restore MISMATCH"

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
