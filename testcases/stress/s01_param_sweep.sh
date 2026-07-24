#!/bin/bash
# ============================================================================
#  stress suite 01 — parameter sweep
#  source: logphase_stress/test_params.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s01_param_sweep.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── parameter sweep for the tiered-buffer conf params (CUBRID 11.3) ───────────
# For each param: valid values (backup completes + restore hash matches source +
# expected mode in log) and invalid values (conf parse FAIL-FAST at init, per
# set_size_value/validate rules). Modes asserted via cubrid_backup.log lines:
#   'tiered buffer summary'            -> buffering ran
#   'log-phase boundary detected'      -> parser armed (2-mode)
#   'reserve-smaller-than-pipe'        -> single-mode fallback
#   'F_SETPIPE_SZ failed'              -> pipe-size warn path
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/prm}; DB=${DB:-prm}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
export LD_LIBRARY_PATH=$W/lib:$C3/lib
CONF=$C3/conf/cubrid_backup.conf; SVCONF=$C3/conf/cubrid.conf; LOG=$C3/log/cubrid_backup.log
FAIL=0
R(){ echo "[$(date +%H:%M:%S)] ### $*"; }
ok(){ echo "  [OK] $*"; }
nok(){ echo "  [NOK] $*"; FAIL=1; }

rm -rf $W; mkdir -p $W/lib $W/hdr $W/spool $W/db $W/bk; cd $W/db
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread || { echo BUILD_FAIL; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
grep -q '^cubrid_port_id=' "$SVCONF" && sed -i "s/^cubrid_port_id=.*/cubrid_port_id=$PORT/" "$SVCONF" || echo "cubrid_port_id=$PORT" >> "$SVCONF"

cleanup(){ cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1
  cubrid master stop >/dev/null 2>&1
  cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"; }
trap cleanup EXIT

# wconf MEM DISK PATH KEEP FIFO
wconf(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=true
except_active_log=false
sleep_msecs=0
fifo_size=$5
buffer_memory_size=$1
buffer_disk_limit=$2
buffer_disk_path=$3
buffer_disk_keep_spool=$4
E
}

dump(){ csql -u dba -N -c "SELECT a,b FROM t ORDER BY a" $DB 2>/dev/null | grep -vE 'rows selected|Committed|^[[:space:]]*$'; }

R "0. create $DB + source hash"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=100M --log-volume-size=50M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));"; echo "INSERT INTO t VALUES (1,'s');";
  for i in $(seq 1 16); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done;
  echo "UPDATE t SET b=RPAD(TO_CHAR(a),190,CHR(65+MOD(a,26)));"; } | csql -u dba $DB >/dev/null 2>&1
H_SRC=$(dump | sha256sum | awk '{print $1}')
PIPE_MAX=$(cat /proc/sys/fs/pipe-max-size 2>/dev/null || echo 1048576)
R "   src=$H_SRC  pipe-max-size=$PIPE_MAX"

restore_hash(){ cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  printf '0\n0\n0\n0\n' | cubrid restoredb -B $1 -l 0 $DB > $W/restore.out 2>&1
  cubrid server start $DB >/dev/null 2>&1
  dump | sha256sum | awk '{print $1}'; }

# ok_case NAME: backup must complete; restore hash must match; J1 invariant:
# summary bytes_total (when buffering ran) must equal the backup file size.
ok_case(){ local name=$1; local out=$W/bk/$name
  : > $LOG 2>/dev/null
  timeout 180 $W/tc05 $DB 0 $out 200 65536 > $out.out 2>&1; local rc=$?
  if [ $rc -ne 0 ] || ! grep -q '\[OK\]' $out.out; then nok "$name backup failed (rc=$rc): $(tail -1 $out.out)"; return 1; fi
  local bt=$(grep -oE 'bytes_total=[0-9]+' $LOG | head -1 | cut -d= -f2)
  local fsz=$(stat -c%s $out)
  if [ -n "$bt" ]; then
    [ "$bt" = "$fsz" ] && ok "$name J1: bytes_total==file size ($fsz)" || nok "$name J1 VIOLATION: bytes_total=$bt file=$fsz"
  fi
  mkdir -p $W/d_$name; cp $out $W/d_$name/${DB}_bk0v000
  local h=$(restore_hash $W/d_$name); rm -rf $W/d_$name
  [ "$h" = "$H_SRC" ] && ok "$name backup+restore hash match" || nok "$name HASH MISMATCH"
}
# fail_case NAME: client must fail fast at init (invalid conf)
fail_case(){ local name=$1; local out=$W/bk/$name
  timeout 30 $W/tc05 $DB 0 $out 0 65536 > $out.out 2>&1; local rc=$?
  [ $rc -ne 0 ] && grep -q '\[NOK\]' $out.out && ok "$name rejected fast (rc=$rc)" \
    || nok "$name expected fail-fast, got rc=$rc: $(tail -1 $out.out)"
}
has(){ grep -q "$1" $LOG; }

R "=== A. fifo_size (conf clamp [64KB,1MB], INT_MAX reject) ==="
wconf 1MB 256MB $W/spool false 1KB;   ok_case A0_fifo1KB_clampup
has 'fifo_size below 64KB' && ok "A0 raise-to-64KB warn logged" || nok "A0 expected below-64KB warn"
wconf 1MB 256MB $W/spool false 64KB;  ok_case A1_fifo64KB
wconf 1MB 256MB $W/spool false 8MB;   ok_case A2_fifo8MB_clampdown
has 'fifo_size above 1MB' && ok "A2 clamp-to-1MB warn logged" || nok "A2 expected above-1MB clamp warn"
wconf 1MB 256MB $W/spool false abc;   fail_case A3_fifo_garbage
wconf 1MB 256MB $W/spool false -1;    fail_case A4_fifo_negative
wconf 1MB 256MB $W/spool false 3GB;   fail_case A5_fifo_gt_intmax

R "=== B. buffer_memory_size (min = io_size, set_size_value rejects) ==="
wconf 0 0 $W/spool false 1MB;         ok_case B1_mem0_legacy
has 'tiered buffer summary' && nok "B1 buffering ran with mem=0" || ok "B1 legacy path (no tiered summary)"
wconf 64KB 256MB $W/spool false 1MB;  ok_case B2_mem64KB_tiny
wconf 512MB 256MB $W/spool false 1MB; ok_case B3_mem512MB
has 'spilled=0' && ok "B3 no spill with big mem" || echo "  [info] B3 spilled anyway (slow consumer)"
wconf 5QB 256MB $W/spool false 1MB;   fail_case B4_mem_bad_suffix
wconf 16KB 256MB $W/spool false 1MB;  fail_case B5_mem_below_iosize

R "=== C. buffer_disk_limit ==="
wconf 1MB 0 $W/spool false 1MB;       ok_case C1_disk0_memonly
has 'log-phase boundary detected' && nok "C1 parser armed without disk tier" || ok "C1 parser off (memory-only)"
wconf 1MB 512KB $W/spool false 1MB;   ok_case C2_disk_lt_pipe
has 'reserve-smaller-than-pipe' && ok "C2 single-mode fallback logged" || nok "C2 expected reserve-smaller-than-pipe"
wconf 1MB 256MB $W/spool false 1MB;   ok_case C3_disk256MB_baseline
has 'log-phase boundary detected' && ok "C3 parser armed (2-mode)" || nok "C3 parser not armed"
wconf 1MB -5 $W/spool false 1MB;      fail_case C4_disk_negative
wconf 0 256MB $W/spool false 1MB;     fail_case C5_disk_without_mem

R "=== D. buffer_disk_keep_spool ==="
rm -f $W/spool/cubrid_bkbuf_* 2>/dev/null
wconf 1MB 256MB $W/spool true 1MB;    ok_case D1_keep_true
ls $W/spool/cubrid_bkbuf_${DB}_* >/dev/null 2>&1 && ok "D1 spool file kept" || nok "D1 spool file missing with keep=true"
rm -f $W/spool/cubrid_bkbuf_* 2>/dev/null
wconf 1MB 256MB $W/spool false 1MB;   ok_case D2_keep_false
ls $W/spool/cubrid_bkbuf_${DB}_* >/dev/null 2>&1 && nok "D2 spool file left with keep=false" || ok "D2 spool file removed"

R "=== E. compress=false (NONE) with buffering ==="
sed -i 's/^compress=true/compress=false/' "$CONF"
ok_case E1_none_buffered
has 'log-phase boundary detected' && ok "E1 parser armed on NONE stream" || nok "E1 parser not armed on NONE"

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
