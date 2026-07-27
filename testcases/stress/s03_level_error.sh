#!/bin/bash
# ============================================================================
#  stress suite 03 — per-level error injection + probe torture
#  source: logphase_stress/test_lvlerr_s1.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s03_level_error.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── G1: error injection at backup levels 1/2  +  S1: probe/carry torture ──────
# G1: on a DB with a prior L0, rerun kill-backupdb / kill-server / undersized-
#     spool at L1 and L2 — same expected outcomes as L0.
# S1: minimum ring (mem = io_size) + consumer stalled through the DATA phase =>
#     probe/carry cycle under max pressure; expect lookahead>0, reserve untouched
#     until boundary, completion + restore hash match (J1 too).
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/lvle}; DB=${DB:-lvle}; PORT=${PORT:-1599}
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

wconf(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=true
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=$1
buffer_disk_limit=$2
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E
}
# Liveness gate: without it a dead DB makes csql print the SAME error text for the
# "before" and "after" capture, so their hashes match and a broken run reads as OK.
live(){ timeout 5 csql -u dba -N -c "SELECT 1 FROM db_root" $DB 2>/dev/null | grep -qE '^[[:space:]]*[0-9]+'; }
wait_db(){ local _end=$((SECONDS+60)); while [ $SECONDS -lt $_end ]; do live && return 0; sleep 1; done; nok "DB not live after 60s ($1)"; return 1; }
# When the DB is down emit a unique marker so the comparison FAILS loudly instead
# of two identical error strings comparing equal.
dump(){ live || { echo "__DB_NOT_LIVE__$(date +%s%N)"; return; }; csql -u dba -N -c "SELECT a,b FROM t ORDER BY a" $DB 2>/dev/null | grep -vE 'rows selected|Committed|^[[:space:]]*$'; }
wait_to(){ local pid=$1 to=$2 i=0
  while kill -0 $pid 2>/dev/null; do i=$((i+1)); [ $i -ge $((to*10)) ] && { kill -9 $pid 2>/dev/null; WRC=124; return; }; sleep 0.1; done
  wait $pid; WRC=$?; }
spawn_catch(){ local L=$1 out=$2; : > $LOG 2>/dev/null; rm -f $out
  $W/tc05 $DB $L $out 2000 4096 > $out.out 2>&1 & CPID=$!
  BPID=""; for i in $(seq 1 300); do BPID=$(pgrep -f "backupdb.*$DB"|head -1); [ -n "$BPID" ] && return 0
    kill -0 $CPID 2>/dev/null || return 1; sleep 0.02; done; return 1; }

R "0. DB + L0 baseline (needed before incremental levels)"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=300M --log-volume-size=100M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
wait_db "server start"
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));"; echo "INSERT INTO t VALUES (1,'s');";
  for i in $(seq 1 18); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done;
  echo "UPDATE t SET b=RPAD(TO_CHAR(a),190,CHR(65+MOD(a,26)));"; } | csql -u dba $DB >/dev/null 2>&1
wconf 1MB 256MB
$W/tc05 $DB 0 $W/bk/${DB}_bk0v000 0 1048576 > $W/bk/l0.out 2>&1
grep -q '\[OK\]' $W/bk/l0.out && ok "L0 baseline done" || { nok "L0 failed"; exit 1; }

for L in 1 2; do
  echo "UPDATE t SET b=RPAD('mod$L',190,'m') WHERE MOD(a,7)=$L;" | csql -u dba $DB >/dev/null 2>&1
  R "=== G1-L$L-a: kill backupdb mid-stream at level $L ==="
  wconf 1MB 256MB
  if spawn_catch $L $W/bk/g_a; then
    pkill -9 -f "backupdb.*$DB"; wait_to $CPID 60   # kill the WHOLE producer (cubrid backupdb = dispatcher+worker), not just head -1
    [ "$WRC" != "124" ] && [ "$WRC" -ne 0 ] && grep -q 'cubrid_backup_read' $W/bk/g_a.out \
      && ok "L$L kill-backupdb: read FAILURE surfaced (rc=$WRC)" || nok "L$L kill-backupdb: rc=$WRC"
  else nok "L$L could not catch producer"; fi
  left=1; for i in $(seq 1 100); do pgrep -f "backupdb.*$DB" >/dev/null || { left=0; break; }; sleep 0.1; done
  [ $left -eq 0 ] && ok "L$L no leftover (<=10s)" || nok "L$L leftover backupdb"

  R "=== G1-L$L-b: kill cub_server mid-stream at level $L ==="
  wconf 1MB 256MB
  if spawn_catch $L $W/bk/g_b; then
    SPID=$(pgrep -f "cub_server $DB"|head -1); [ -n "$SPID" ] && kill -9 $SPID
    wait_to $CPID 60
    [ "$WRC" != "124" ] && [ "$WRC" -ne 0 ] && ok "L$L kill-server: client failed loudly (rc=$WRC)" \
      || { sz=$(stat -c%s $W/bk/g_b 2>/dev/null||echo 0); [ "$sz" -gt 25000000 ] && ok "L$L stream finished pre-injection ($sz)" || nok "L$L rc=$WRC sz=$sz"; }
  else nok "L$L could not catch producer"; fi
  cubrid server start $DB >/dev/null 2>&1
wait_db "server start"

  R "=== G1-L$L-c: undersized spool (4MB) at level $L — complete + restorable ==="
  wconf 1MB 4MB
  : > $LOG 2>/dev/null
  $W/tc05 $DB $L $W/bk/${DB}_bk${L}v000 3000 8192 > $W/bk/g_c.out 2>&1 & wait_to $! 600
  [ "$WRC" -eq 0 ] && grep -q '\[OK\]' $W/bk/g_c.out && ok "L$L undersized-spool backup completed" || nok "L$L undersized failed (rc=$WRC)"
  grep -oE 'log_phase=[01]|lookahead=[0-9]+|wait_count=[0-9]+' $LOG | head -3 | sed "s/^/  [info] L$L /"
done

R "=== G1 restore: chain -l 2 must reproduce final state ==="
H_FIN=$(dump | sha256sum | awk '{print $1}')
cubrid server stop $DB >/dev/null 2>&1
rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
printf '0\n0\n0\n0\n0\n0\n' | cubrid restoredb -B $W/bk -l 2 $DB > $W/restore.out 2>&1
cubrid server start $DB >/dev/null 2>&1
wait_db "server start"
HR=$(dump | sha256sum | awk '{print $1}')
[ "$HR" = "$H_FIN" ] && ok "G1 -l 2 chain restore hash matches" || nok "G1 chain restore MISMATCH"

R "=== S1: probe/carry torture — mem=io_size(64KB), data-phase stalled consumer ==="
wconf 64KB 256MB
: > $LOG 2>/dev/null
H_S=$(dump | sha256sum | awk '{print $1}')
$W/tc05 $DB 0 $W/bk/s1 5000 4096 > $W/bk/s1.out 2>&1 & wait_to $! 900
[ "$WRC" -eq 0 ] && grep -q '\[OK\]' $W/bk/s1.out && ok "S1 completed under min-ring pressure" || nok "S1 failed (rc=$WRC)"
la=$(grep -oE 'lookahead=[0-9]+' $LOG | head -1 | cut -d= -f2)
[ "${la:-0}" -gt 0 ] && ok "S1 probes fired (lookahead=$la) — carry path exercised" || nok "S1 lookahead=0: torture did not engage probe"
bt=$(grep -oE 'bytes_total=[0-9]+' $LOG | head -1 | cut -d= -f2); fsz=$(stat -c%s $W/bk/s1)
[ "$bt" = "$fsz" ] && ok "S1 J1: bytes_total==file ($fsz)" || nok "S1 J1 VIOLATION bt=$bt fsz=$fsz"
mkdir -p $W/ds1; cp $W/bk/s1 $W/ds1/${DB}_bk0v000
cubrid server stop $DB >/dev/null 2>&1
rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
printf '0\n0\n0\n0\n' | cubrid restoredb -B $W/ds1 -l 0 $DB > $W/restore_s1.out 2>&1
cubrid server start $DB >/dev/null 2>&1
wait_db "server start"
HS_R=$(dump | sha256sum | awk '{print $1}')
[ "$HS_R" = "$H_S" ] && ok "S1 restore hash matches (probe/carry byte-exact)" || nok "S1 HASH MISMATCH"

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
