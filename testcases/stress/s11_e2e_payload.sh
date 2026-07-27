#!/bin/bash
# ============================================================================
#  stress suite 11 — byte-integrity end-to-end
#  source: testcases/run_logphase_e2e_payload.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s11_e2e_payload.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# End-to-end PAYLOAD byte-integrity test for the tiered-buffer + log-phase parser.
#
# Beyond row COUNT: compares a sha256 of the full sorted table dump between the
# source DB and each restored buffered backup. Exercises the mem-ring wrap +
# disk-spill + lock-free pop path (M1) under a slow consumer, for both an LZ4
# (compress) and a NONE (no-compress) stream, plus a buffering-off control, and
# checks the parser arms on the real NONE stream too.
#
# Requires a running CUBRID target: set $CUBRID (e.g. an 11.3 _install/CUBRID).
# Uses an isolated port + dedicated db; saves/restores conf on exit.
set -u
: "${CUBRID:?set CUBRID to the target install (e.g. 11.3 build_x86_64_release/_install/CUBRID)}"
export PATH=$CUBRID/bin:$PATH
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/lpe2e}
DB=${DB:-lpe2e}
PORT=${PORT:-1599}
ROWS=${ROWS:-262144}
export LD_LIBRARY_PATH=$W/lib:$CUBRID/lib:${LD_LIBRARY_PATH:-}
CONF=$CUBRID/conf/cubrid_backup.conf
SVCONF=$CUBRID/conf/cubrid.conf
LOG=$CUBRID/log/cubrid_backup.log
R(){ echo "### $*"; }
fail=0

rm -rf $W; mkdir -p $W/lib $W/hdr $W/spool $W/db $W/bk; cd $W/db

R "build API .so + slow-consumer client from $API"
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread \
  || { echo "BUILD FAIL"; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/backup_tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || { echo "CLIENT FAIL"; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2">>"$SVCONF"; fi; }
setp cubrid_port_id $PORT

R "create $DB + load $ROWS rows, then vary every row's payload"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=300M --log-volume-size=100M $DB en_US >/dev/null 2>&1 \
  || { echo "CREATEDB FAIL"; cp $W/svconf.bak "$SVCONF"; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));";
  echo "INSERT INTO t VALUES (1, 'seed');";
  for i in $(seq 1 18); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done;
  echo "UPDATE t SET b=RPAD(TO_CHAR(a),190,CHR(65+MOD(a,26)));"; } | csql -u dba $DB >/dev/null 2>&1

# strip csql's nondeterministic status footer ("N rows selected (x sec) Committed")
# so the hash reflects only row data, not run-to-run timing.
dump(){ csql -u dba -N -c "SELECT a, b FROM t ORDER BY a" $1 2>/dev/null | grep -vE 'rows selected|Committed|^[[:space:]]*$'; }
H_SRC=$(dump $DB | sha256sum | awk '{print $1}')
NROW=$(csql -u dba -N -c "SELECT COUNT(*) FROM t" $DB 2>/dev/null | tr -d ' ' | grep -E '^[0-9]+$' | tail -1)
R "rows=$NROW  source payload sha256=$H_SRC"
[ "$NROW" = "$ROWS" ] || { echo "[NOK] load: rows $NROW != $ROWS"; fail=1; }

wconf(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=8
compress=$1
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=$2
buffer_disk_limit=$3
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E
}

# $1 label  $2 compress  $3 mem  $4 disk  $5 bkdir
run_case(){
  local label=$1 comp=$2 mem=$3 disk=$4 bk=$5
  : > $LOG 2>/dev/null; wconf $comp $mem $disk
  cubrid server stop $DB >/dev/null 2>&1; cubrid server start $DB >/dev/null 2>&1
  mkdir -p $bk
  $W/backup_tc05 $DB 0 $bk/${DB}_bk0v000 4000 4096 > $bk/out.txt 2>&1
  local armed=$(grep -c "log-phase boundary detected" $LOG 2>/dev/null)
  local summ=$(grep "tiered buffer summary" $LOG 2>/dev/null | head -1 | sed 's/.*tiered buffer/tiered buffer/')
  cubrid server stop $DB >/dev/null 2>&1; rm -rf $W/db/*
  expect -c "spawn cubrid restoredb -B $bk -l 0 $DB
             expect { \"The\" {send \"0\r\"; exp_continue} eof }" >/dev/null 2>&1
  cubrid server start $DB >/dev/null 2>&1
  local h=$(dump $DB | sha256sum | awk '{print $1}')
  if [ "$h" = "$H_SRC" ]; then
    echo "[OK] $label: payload sha256 MATCH  armed=$armed"
  else
    echo "[NOK] $label: payload MISMATCH  src=$H_SRC got=$h  armed=$armed"; fail=1
  fi
  [ -n "$summ" ] && echo "     $label: $summ"
}

R "=== case1: LZ4 (compress) + slow consumer, mem 1MB + disk 256MB (forces mem wrap + disk spill) ==="
run_case "LZ4-buffered"      true  1MB 256MB $W/bk/c1
R "=== case2: NONE (no-compress) + slow consumer, mem 1MB + disk 256MB ==="
run_case "NONE-buffered"     false 1MB 256MB $W/bk/c2
R "=== case3: buffering OFF control (legacy direct-FIFO) ==="
run_case "buffering-off-ctl" true  0   0     $W/bk/c3

R "=== cleanup ==="
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; cubrid master stop >/dev/null 2>&1
cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"
rm -rf $W/db/* $W/spool/*
R "RESULT: $([ $fail -eq 0 ] && echo ALL-PASS || echo FAIL)"
R DONE
