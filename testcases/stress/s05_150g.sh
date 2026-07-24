#!/bin/bash
# ============================================================================
#  stress suite 05 — 150GB large DB
#  source: logphase_stress/test_150g.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s05_150g.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── 150GB endurance: levels 0/1/2, NONE (uncompressed => the buffer really
#    carries ~150GB), tiered buffer on, per-level integrity via aggregate
#    checksums + sampled ordered dump hash (full-dump hashing is impractical
#    at this scale; restoredb page checksums add an independent layer). ──────
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/g150}; DB=${DB:-g150}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
export LD_LIBRARY_PATH=$W/lib:$C3/lib
CONF=$C3/conf/cubrid_backup.conf; SVCONF=$C3/conf/cubrid.conf; LOG=$C3/log/cubrid_backup.log
FAIL=0
BASE=16777216            # 2^24 rows built by doubling, then 8 offset copies -> 9*BASE rows (~150M*... see below)
COPIES=37                # total rows = (1+COPIES)*BASE ~ 637M rows * ~250B/row on-page ≈ 150GB
R(){ echo "[$(date +%H:%M:%S)] ### $*"; }
ok(){ echo "  [OK] $*"; }
nok(){ echo "  [NOK] $*"; FAIL=1; }

avail=$(df --output=avail -B1G /home | tail -1 | tr -d ' ')
[ "$avail" -lt 450 ] && { echo "ABORT: need >=450G free on /home, have ${avail}G"; exit 1; }

rm -rf $W; mkdir -p $W/lib $W/hdr $W/spool $W/db $W/bk; cd $W/db
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread || { echo BUILD_FAIL; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id $PORT
setp log_max_archives 20
setp data_buffer_size 4G
cleanup(){ cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1
  cubrid master stop >/dev/null 2>&1
  cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"
  rm -rf $W/spool/*; }
trap cleanup EXIT

# mem disk delay_us readbuf
wconf(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=4
compress=false
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=$1
buffer_disk_limit=$2
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E
}

R "1. createdb (autoextend) + build 150GB table: double to $BASE rows, then $COPIES offset copies"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=10G --log-volume-size=2G $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a BIGINT PRIMARY KEY, b VARCHAR(200));"; echo "INSERT INTO t VALUES (1, 's');";
  for i in $(seq 1 24); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done;
  echo "UPDATE t SET b=RPAD(TO_CHAR(a),190,CHR(65+MOD(a,26))) WHERE a<=$BASE;"; } | csql -u dba $DB >/dev/null 2>&1
R "   base rows built; copying..."
for k in $(seq 1 $COPIES); do
  echo "INSERT INTO t SELECT a+$k*$BASE, b FROM t WHERE a<=$BASE;" | csql -u dba $DB >/dev/null 2>&1
  [ $((k%5)) -eq 0 ] && R "   copy $k/$COPIES done, db=$(du -sBG $W/db | cut -f1)"
done
R "   final db size=$(du -sBG $W/db | cut -f1)"

# integrity fingerprint: count|min|max (overflow-safe: SUM(a) hit -458 at 638M rows)
# + sha256 of every-1000th row ordered. stderr kept in $W/v.err so a failing
# query can never silently collapse the fingerprint again.
V(){ csql -u dba -N -c "SELECT COUNT(*), MIN(a), MAX(a) FROM t" $DB 2>>$W/v.err | grep -E '[0-9]' | tr -s ' ' '|'
     csql -u dba -N -c "SELECT a,b FROM t WHERE MOD(a,1000)=0 ORDER BY a" $DB 2>>$W/v.err | grep -vE 'rows selected|Committed|^[[:space:]]*$' | sha256sum | awk '{print $1}'; }

declare -A FP
do_level(){ local L=$1 mem=$2 disk=$3 delay=$4 rbuf=$5
  FP[$L]=$(V | tr '\n' '/')
  : > $LOG 2>/dev/null; wconf $mem $disk
  local t0=$(date +%s)
  $W/tc05 $DB $L $W/bk/${DB}_bk${L}v000 $delay $rbuf > $W/bk/l$L.out 2>&1
  local rc=$? t1=$(date +%s)
  local sz=$(stat -c%s $W/bk/${DB}_bk${L}v000 2>/dev/null || echo 0)
  [ $rc -eq 0 ] && grep -q '\[OK\]' $W/bk/l$L.out \
    && ok "L$L backup: $sz bytes in $((t1-t0))s" || nok "L$L backup failed rc=$rc"
  grep -q 'log-phase boundary detected' $LOG && ok "L$L parser armed (NONE @150G)" || nok "L$L parser NOT armed"
  local bt=$(grep -oE 'bytes_total=[0-9]+' $LOG | head -1 | cut -d= -f2)
  [ "$bt" = "$sz" ] && ok "L$L J1 bytes_total==file" || nok "L$L J1 bt=$bt file=$sz"
  grep 'tiered buffer summary' $LOG | head -1 | sed "s/.*tiered/  [info] L$L tiered/"
}

R "2. LEVEL 0 (NONE, fast consumer: the buffer carries the full ~150GB)"
do_level 0 64MB 8GB 0 1048576

R "3. bounded mod A (1M rows) -> LEVEL 1 (mem 1MB + slow-ish consumer => wrap+spill at scale)"
echo "UPDATE t SET b=RPAD('L1',190,'q') WHERE a<=1000000;" | csql -u dba $DB >/dev/null 2>&1
do_level 1 1MB 8GB 2000 1048576

R "4. bounded mod B -> LEVEL 2 (fast)"
echo "UPDATE t SET b=RPAD('L2',190,'w') WHERE a>1000000 AND a<=2000000;" | csql -u dba $DB >/dev/null 2>&1
do_level 2 64MB 8GB 0 1048576

R "5. restore chain -l 2 / 1 / 0 and compare fingerprints"
for L in 2 1 0; do
  cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  t0=$(date +%s)
  printf '0\n0\n0\n0\n0\n0\n' | cubrid restoredb -B $W/bk -l $L $DB > $W/restore_l$L.out 2>&1
  cubrid server start $DB >/dev/null 2>&1
  t1=$(date +%s)
  FPR=$(V | tr '\n' '/')
  [ "$FPR" = "${FP[$L]}" ] && ok "restore -l $L fingerprint match ($((t1-t0))s)" \
    || nok "restore -l $L MISMATCH: got=$FPR want=${FP[$L]}"
done

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
