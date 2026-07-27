#!/bin/bash
# ============================================================================
#  stress suite 02 — error / abnormal injection
#  source: logphase_stress/test_errors2.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s02_error_inject.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── error-injection suite v2 (fixes v1 harness flaws) ─────────────────────────
# v1 findings: with buffering on, backupdb finishes in <1s regardless of consumer
# speed, so "kill at 3MB consumed" hit an already-exited producer (vacuous pass /
# false NOK). v2 kills the instant the target process appears and ASSERTS the
# injection actually landed. E5 split per code design: conf-time invalid path is
# FAIL-FAST (validate_dir at conf load), runtime spool failure is DEGRADE.
# Each case starts from a verified-healthy DB.
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/errt2}; DB=${DB:-errt2}; PORT=${PORT:-1599}
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
gcc -I $W/hdr -O2 -o $W/errcli $SCRIPT_DIR/helpers/errcli.c -L $W/lib -lcubridbackupapi || { echo ERRCLI_FAIL; exit 1; }

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
buffer_memory_size=1MB
buffer_disk_limit=$1
buffer_disk_path=$2
buffer_disk_keep_spool=false
E
}

dump(){ csql -u dba -N -c "SELECT a,b FROM t ORDER BY a" $DB 2>/dev/null | grep -vE 'rows selected|Committed|^[[:space:]]*$'; }

build_db(){ cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
  cubrid createdb -r --db-volume-size=300M --log-volume-size=100M $DB en_US >/dev/null 2>&1 || return 1
  cubrid server start $DB >/dev/null 2>&1 || return 1
  { echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));"; echo "INSERT INTO t VALUES (1,'s');";
    for i in $(seq 1 18); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done;
    echo "UPDATE t SET b=RPAD(TO_CHAR(a),190,CHR(65+MOD(a,26)));"; } | csql -u dba $DB >/dev/null 2>&1
  H_SRC=$(dump | sha256sum | awk '{print $1}'); }

# settle: reap stragglers from the previous case, then wait for a healthy server
ensure_db(){ pkill -9 -f "backupdb.*$DB" 2>/dev/null; sleep 1
  for i in $(seq 1 20); do csql -u dba -c "SELECT 1" $DB >/dev/null 2>&1 && return 0; sleep 0.5; done
  echo "  [info] DB unhealthy -> rebuilding"; build_db || { nok "DB rebuild failed"; exit 1; }; }

# wait for pid with timeout; sets WRC (124 = hung and killed)
wait_to(){ local pid=$1 to=$2 i=0
  while kill -0 $pid 2>/dev/null; do i=$((i+1)); [ $i -ge $((to*10)) ] && { kill -9 $pid 2>/dev/null; WRC=124; return; }; sleep 0.1; done
  wait $pid; WRC=$?; }

# spawn a backup and return the moment backupdb is ALIVE (sets CPID, BPID)
spawn_catch_producer(){ : > $LOG 2>/dev/null; rm -f $1
  $W/tc05 $DB 0 $1 2000 4096 > $1.out 2>&1 & CPID=$!
  BPID=""
  for i in $(seq 1 300); do
    BPID=$(pgrep -f "backupdb.*$DB" | head -1); [ -n "$BPID" ] && return 0
    kill -0 $CPID 2>/dev/null || return 1     # client already died
    sleep 0.02
  done
  return 1; }

restore_and_hash(){ # $1 = dir containing ${DB}_bk0v000
  cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  printf '0\n0\n0\n0\n' | cubrid restoredb -B $1 -l 0 $DB > $W/restore.out 2>&1
  cubrid server start $DB >/dev/null 2>&1
  local h=""
  for i in $(seq 1 30); do
    h=$(dump | sha256sum | awk '{print $1}')
    [ -n "$(dump | head -1)" ] && break     # server up & data visible
    sleep 1
  done
  echo "$h"; }

R "0. build DB (~110MB stream: widens the producer-alive window)"
build_db || { echo BUILD_DB_FAIL; exit 1; }
R "   src=$H_SRC"

R "=== E1: kill -9 backupdb WHILE WRITING ==="
ensure_db; wconf 256MB $W/spool
if spawn_catch_producer $W/bk/e1; then
  kill -9 $BPID && ok "E1 injection landed (backupdb pid=$BPID killed alive)"
  wait_to $CPID 60
  if [ "$WRC" = "124" ]; then nok "E1 client HUNG"; elif [ "$WRC" -ne 0 ] && grep -q '\[NOK\]' $W/bk/e1.out; then
    ok "E1 client failed loudly (rc=$WRC) — no silent truncation"
  else nok "E1 rc=$WRC out=$(tail -1 $W/bk/e1.out)"; fi
else nok "E1 could not catch backupdb alive"; fi
left=1; for i in $(seq 1 100); do pgrep -f "backupdb.*$DB" >/dev/null || { left=0; break; }; sleep 0.1; done; [ $left -eq 0 ] && ok "E1 no leftover backupdb (<=10s)" || nok "E1 leftover backupdb"

R "=== E2: kill -9 cub_server while backupdb writing ==="
ensure_db; wconf 256MB $W/spool
if spawn_catch_producer $W/bk/e2; then
  SPID=$(pgrep -f "cub_server $DB" | head -1)
  [ -n "$SPID" ] && kill -9 $SPID && ok "E2 injection landed (cub_server killed, backupdb alive)"
  wait_to $CPID 60
  if [ "$WRC" = "124" ]; then nok "E2 client HUNG"; elif [ "$WRC" -ne 0 ]; then ok "E2 client failed loudly (rc=$WRC)"
  else
    # producer may have finished its snapshot before feeling the server death
    sz=$(stat -c%s $W/bk/e2 2>/dev/null || echo 0)
    [ "$sz" -gt 100000000 ] && ok "E2 full stream already produced before injection (size=$sz) — acceptable" \
      || nok "E2 client claimed success on truncated stream (size=$sz)"
  fi
else nok "E2 could not catch backupdb alive"; fi
left=1; for i in $(seq 1 100); do pgrep -f "backupdb.*$DB" >/dev/null || { left=0; break; }; sleep 0.1; done; [ $left -eq 0 ] && ok "E2 no leftover backupdb (<=10s)" || nok "E2 leftover backupdb"
cubrid server start $DB >/dev/null 2>&1

R "=== E3: kill -9 CLIENT while backupdb writing (orphan check) ==="
ensure_db; wconf 256MB $W/spool
if spawn_catch_producer $W/bk/e3; then
  kill -9 $CPID && ok "E3 injection landed (client killed, backupdb pid=$BPID alive)"
  gone=0; for i in $(seq 1 150); do pgrep -f "backupdb.*$DB" >/dev/null || { gone=1; break; }; sleep 0.1; done
  [ $gone -eq 1 ] && ok "E3 backupdb exited after client death (~$((i/10)).$((i%10))s, no orphan)" \
    || { nok "E3 ORPHAN backupdb"; pkill -9 -f "backupdb.*$DB"; }
else nok "E3 could not catch backupdb alive"; fi
hz=1; for i in $(seq 1 60); do csql -u dba -c "SELECT 1" $DB >/dev/null 2>&1 && { hz=0; break; }; sleep 0.5; done; [ $hz -eq 0 ] && ok "E3 server healthy (<=30s)" || nok "E3 server broken"

R "=== E4: undersized spool (4MB) + slow consumer — must complete + hash match ==="
ensure_db; wconf 4MB $W/spool
: > $LOG 2>/dev/null
$W/tc05 $DB 0 $W/bk/e4 3000 8192 > $W/bk/e4.out 2>&1 & wait_to $! 600
[ "$WRC" -eq 0 ] && grep -q '\[OK\]' $W/bk/e4.out && ok "E4 backup completed under backpressure" || nok "E4 failed (rc=$WRC)"
grep -oE 'wait_count=[0-9]+|disk_high_water=[0-9]+' $LOG | head -2 | sed 's/^/  [info] E4 /'
dhw=$(grep -oE 'disk_high_water=[0-9]+' $LOG | head -1 | cut -d= -f2)
[ "${dhw:-0}" -ge $((4*1024*1024)) ] && ok "E4 reserve actually filled (disk_high_water=$dhw == cap)" \
  || nok "E4 undersized path NOT exercised (disk_high_water=${dhw:-0} < 4MB)"
mkdir -p $W/d4; cp $W/bk/e4 $W/d4/${DB}_bk0v000
H4=$(restore_and_hash $W/d4)
[ "$H4" = "$H_SRC" ] && ok "E4 restore hash matches source" || nok "E4 hash mismatch"

R "=== E5a: INVALID spool path in conf — designed behavior = FAIL-FAST at init ==="
ensure_db; wconf 256MB /nonexistent_dir_xyz/spool
$W/tc05 $DB 0 $W/bk/e5a 0 65536 > $W/bk/e5a.out 2>&1 & wait_to $! 30
if [ "$WRC" = "124" ]; then nok "E5a HUNG"; elif [ "$WRC" -ne 0 ] && grep -q '\[NOK\]' $W/bk/e5a.out; then
  ok "E5a failed fast at init (rc=$WRC) — invalid conf rejected, no partial backup"
else nok "E5a rc=$WRC out=$(tail -1 $W/bk/e5a.out)"; fi
csql -u dba -c "SELECT 1" $DB >/dev/null 2>&1 && ok "E5a server untouched" || nok "E5a server broken"

R "=== E5b: RUNTIME spool reserve failure (100GB on 63G tmpfs) — degrade memory-only ==="
ensure_db; mkdir -p /tmp/errt2_spool; wconf 100GB /tmp/errt2_spool
: > $LOG 2>/dev/null
$W/tc05 $DB 0 $W/bk/e5b 1000 65536 > $W/bk/e5b.out 2>&1 & wait_to $! 300
grep -q 'spool setup failed' $LOG && ok "E5b degraded to memory-only (runtime warning logged)" || nok "E5b no degrade warning"
[ "$WRC" -eq 0 ] && grep -q '\[OK\]' $W/bk/e5b.out && ok "E5b backup completed memory-only" || nok "E5b failed (rc=$WRC)"
mkdir -p $W/d5; cp $W/bk/e5b $W/d5/${DB}_bk0v000
H5=$(restore_and_hash $W/d5)
[ "$H5" = "$H_SRC" ] && ok "E5b restore hash matches source" || nok "E5b hash mismatch"
rm -rf /tmp/errt2_spool

R "=== E6: early cubrid_backup_end mid-stream + handle reuse (healthy server) ==="
ensure_db; wconf 256MB $W/spool
$W/errcli $DB $W/bk/e6 > $W/bk/e6.out 2>&1 & wait_to $! 120
sed 's/^/  /' $W/bk/e6.out
if [ "$WRC" = "124" ]; then nok "E6 HUNG"; elif [ "$WRC" -eq 0 ]; then ok "E6 early-end + same-process reuse passed"
else nok "E6 rc=$WRC"; fi
sz6=$(stat -c%s $W/bk/e6 2>/dev/null || echo 0)
[ "$sz6" -gt 25000000 ] && ok "E6 second backup full-size ($sz6, LZ4-compressed full stream)" || nok "E6 second backup too small ($sz6)"

R "=== E7: server DOWN at begin — must fail bounded, not hang ==="
cubrid server stop $DB >/dev/null 2>&1
wconf 256MB $W/spool
t0=$(date +%s)
$W/tc05 $DB 0 $W/bk/e7 0 65536 > $W/bk/e7.out 2>&1 & wait_to $! 300
t1=$(date +%s)
if [ "$WRC" = "124" ]; then nok "E7 HUNG >300s with server down  <-- investigate"
elif [ "$WRC" -ne 0 ]; then ok "E7 failed bounded in $((t1-t0))s (rc=$WRC)"
else nok "E7 claimed success with server down"; fi
cubrid server start $DB >/dev/null 2>&1

R "=== FINAL: normal buffered backup + restore hash ==="
ensure_db; wconf 256MB $W/spool
: > $LOG 2>/dev/null
$W/tc05 $DB 0 $W/bk/fin 1000 65536 > $W/bk/fin.out 2>&1 & wait_to $! 300
mkdir -p $W/dfin; cp $W/bk/fin $W/dfin/${DB}_bk0v000
HF=$(restore_and_hash $W/dfin)
[ "$WRC" -eq 0 ] && [ "$HF" = "$H_SRC" ] && ok "FINAL sanity backup+restore hash match" || nok "FINAL failed (rc=$WRC)"
grep -c 'log-phase boundary detected' $LOG | sed 's/^/  [info] final armed: /'

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
