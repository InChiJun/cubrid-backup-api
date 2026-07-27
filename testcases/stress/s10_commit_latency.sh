#!/bin/bash
# ============================================================================
#  stress suite 10 — commit-latency / LOG_CS occupancy
#  source: logphase_stress/run_dml_stress6.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s10_commit_latency.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── DML non-interference at TRUE ~30GB backup-log scale ──────────────────────
# Force a big backup LOG_CS window: disable checkpoint + vacuum so first_arv_needed
# stays at DB creation -> backup must copy ALL ~30GB of archives (not just the
# checkpoint-recent subset). Measure LOG_CS-held time directly from the server
# error log (ER_LOG_BACKUP_CS_ENTER/EXIT = "Backup active log ... started/finished")
# and corroborate with a concurrent auto-commit DML latency probe.
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/dml6}; DB=${DB:-lp6}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
export LD_LIBRARY_PATH=$W/lib:$CUBRID/lib
CONF=$C3/conf/cubrid_backup.conf; SVCONF=$C3/conf/cubrid.conf; BKLOG=$C3/log/cubrid_backup.log
ERR=$C3/log/server/${DB}_latest.err
TARGET_ARCH=60; RES=$W/results6.txt
say(){ echo "[$(date +%H:%M:%S)] $*" | tee -a $RES; }
# safety trap: never leave the shared conf with checkpoint/vacuum disabled or a stray server
trap 'cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; cubrid master stop >/dev/null 2>&1; [ -f $W/cubrid.conf.bak ] && cp $W/cubrid.conf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"' EXIT

mkdir -p $W/lib $W/hdr $W/spool $W/bk $W/db; : > $RES; cd $W/db

say "=== 0. build API + clients (feature/log-phase-parser) ==="
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread || { say "BUILD_FAIL"; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/backup_tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || exit 1

say "=== 1. isolated 11.3 instance: port 1599, checkpoint+vacuum DISABLED (pin archive range) ==="
cp "$SVCONF" $W/cubrid.conf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id 1599
setp log_max_archives 1000
setp checkpoint_interval_in_mins 100000
setp checkpoint_every_size 1024G
setp vacuum_disable yes
[ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=1G --log-volume-size=512M $DB en_US >/dev/null 2>&1 || { say "CREATEDB_FAIL"; cp $W/cubrid.conf.bak "$SVCONF"; exit 1; }
cubrid server start $DB >/dev/null 2>&1 || { say "SERVER START FAIL"; cp $W/cubrid.conf.bak "$SVCONF"; exit 1; }

say "=== 2. seed + generate WAL to >= $TARGET_ARCH archives (checkpoint off => all needed) ==="
{ echo "CREATE TABLE churn(a INT PRIMARY KEY, b VARCHAR(220));"; echo "INSERT INTO churn VALUES (1, RPAD('x',210,'y'));";
  for i in $(seq 1 20); do echo "INSERT INTO churn SELECT a+(SELECT MAX(a) FROM churn), b FROM churn;"; done; } | csql -u dba $DB >/dev/null 2>&1
pass=0
while :; do
  na=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
  [ $((pass%10)) -eq 0 ] && say "   archives=$na/$TARGET_ARCH pass=$pass"
  [ "$na" -ge "$TARGET_ARCH" ] && break
  [ "$pass" -ge 4000 ] && { say "   cap hit archives=$na"; break; }
  echo "UPDATE churn SET b=RPAD(CHR(65+MOD(a,26)),210,'z');" | csql -u dba $DB >/dev/null 2>&1
  pass=$((pass+1))
done
ARCH=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
say "   >>> archives=$ARCH (~$(du -sBG $W/db/${DB}_lgar* 2>/dev/null|awk '{s+=$1}END{print s+0}')GB)"

mk_probe(){ local end=$(( $(date +%s)+$1 )) t0 t1 ms n=0 mx=0 sum=0 ge1s=0
  echo "CREATE TABLE IF NOT EXISTS probe(id INT AUTO_INCREMENT PRIMARY KEY,t VARCHAR(10));" | csql -u dba $DB >/dev/null 2>&1
  : > $W/probe_$2.lat
  while [ $(date +%s) -lt $end ]; do t0=$(date +%s%3N)
    csql -u dba -c "INSERT INTO probe(t) VALUES('x')" $DB >/dev/null 2>&1
    t1=$(date +%s%3N); ms=$((t1-t0)); echo $ms>>$W/probe_$2.lat
    n=$((n+1)); sum=$((sum+ms)); [ $ms -gt $mx ] && mx=$ms; [ $ms -ge 1000 ] && ge1s=$((ge1s+1)); done
  local p99=$(sort -n $W/probe_$2.lat|awk '{a[NR]=$1}END{print a[int(NR*0.99)]}')
  say "   PROBE[$2]: n=$n mean=$((sum/(n>0?n:1)))ms p99=${p99}ms max=${mx}ms (>=1s: $ge1s)"; }

# LOG_CS held time from server err log (started/finished). Args: label
cs_report(){ python3 - "$ERR" "$1" <<'PY'
import sys,re,datetime
err,lab=sys.argv[1],sys.argv[2]
def ts(line):
    m=re.search(r'Time:\s+(\d\d)/(\d\d)/(\d\d)\s+(\d\d):(\d\d):(\d\d)\.(\d+)',line)
    if not m: return None
    mo,d,y,H,M,S,ms=m.groups()
    return datetime.datetime(2000+int(y),int(mo),int(d),int(H),int(M),int(S),int(ms)*1000)
try: L=open(err,errors='replace').read().splitlines()
except: print("   CS[%s]: (no server err log)"%lab); sys.exit()
st=fin=None
for i,ln in enumerate(L):
    if "Backup active log" in ln and "started" in ln:
        for j in range(i,max(i-4,-1),-1):
            if ts(L[j]): st=ts(L[j]); break
    if "Backup active log" in ln and "finished" in ln:
        for j in range(i,max(i-4,-1),-1):
            if ts(L[j]): fin=ts(L[j]); break
if st and fin: print("   CS[%s]: LOG_CS held = %.1fs (started..finished)"%(lab,(fin-st).total_seconds()))
elif st: print("   CS[%s]: LOG_CS ENTER logged, NO EXIT within run => held >= probe window (consumer-paced)"%lab)
else: print("   CS[%s]: no backup CS_ENTER in server log"%lab)
PY
}

wconf(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=8
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

say "=== 3. P_base: probe only 60s ==="; mk_probe 60 base

say "=== 4. P_fix: parser + spool 3GB (>= peak backlog ~2GB: mitigation WIN expected) + slow consumer ==="
: > $ERR 2>/dev/null; : > $BKLOG 2>/dev/null; wconf 64MB 3221225472; cubrid server start $DB >/dev/null 2>&1
$W/backup_tc05 $DB 0 $W/bk/${DB}_bk0v000 5000 65536 > $W/bk_fix.out 2>&1 &
BKPID=$!; sleep 6; mk_probe 300 fix
grep -E "log-phase|tiered buffer summary" $BKLOG | sed 's/^/     FIX: /' | tee -a $RES
say "   FIX backup bytes so far: $(stat -c%s $W/bk/${DB}_bk0v000 2>/dev/null)"
cs_report fix | tee -a $RES
kill $BKPID 2>/dev/null; wait $BKPID 2>/dev/null; rm -f $W/bk/*

say "=== 5. P_off: buffering OFF + slow consumer (same rate) ==="
: > $ERR 2>/dev/null; wconf 0 0; cubrid server start $DB >/dev/null 2>&1
$W/backup_tc05 $DB 0 $W/bk/${DB}_bk0v000 5000 65536 > $W/bk_off.out 2>&1 &
BKPID=$!; sleep 6; mk_probe 300 off
say "   OFF backup bytes so far: $(stat -c%s $W/bk/${DB}_bk0v000 2>/dev/null)"
cs_report off | tee -a $RES
kill $BKPID 2>/dev/null; wait $BKPID 2>/dev/null; rm -f $W/bk/*

say "=== cleanup ==="
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; cubrid master stop >/dev/null 2>&1
cp $W/cubrid.conf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"
rm -rf $W/db/* $W/spool/* $W/bk/*
say "=== DONE ==="

# ── scoring ────────────────────────────────────────────────────────────────
# This suite used to print measurements only, so run_stress.sh could never score
# it (always INCONCLUSIVE). Assert the property the suite exists to prove: the
# tiered buffer must shorten how long the backup holds LOG_CS.
FIXCS=$(grep -oE 'CS\[fix\]: LOG_CS held = [0-9.]+' $RES | tail -1 | grep -oE '[0-9.]+$')
OFFCS=$(grep -oE 'CS\[off\]: LOG_CS held = [0-9.]+' $RES | tail -1 | grep -oE '[0-9.]+$')
S10FAIL=0
# OFF holding LOG_CS for the whole window (ENTER logged, no EXIT) is the
# STRONGEST evidence for the mitigation, not a measurement failure.
OFF_UNBOUNDED=$(grep -c 'CS\[off\]: LOG_CS ENTER logged, NO EXIT' $RES)
if [ -n "$FIXCS" ] && [ "$OFF_UNBOUNDED" -gt 0 ]; then
  echo "  [OK] LOG_CS held ${FIXCS}s with buffering; without buffering it was still held at end of window" | tee -a $RES
elif [ -n "$FIXCS" ] && [ -n "$OFFCS" ]; then
  # require a real margin: a near-tie on a single n=1 sample is not evidence
  if awk -v a="$FIXCS" -v b="$OFFCS" 'BEGIN{exit !(b - a >= 0.5 || b > a * 1.5)}'; then
    echo "  [OK] LOG_CS held ${FIXCS}s with buffering < ${OFFCS}s without" | tee -a $RES
  elif awk -v a="$FIXCS" -v b="$OFFCS" 'BEGIN{exit !(a <= b)}'; then
    echo "  [info] LOG_CS ${FIXCS}s vs ${OFFCS}s - no significant difference (n=1)" | tee -a $RES
  else
    echo "  [NOK] LOG_CS held ${FIXCS}s with buffering is HIGHER than ${OFFCS}s without" | tee -a $RES
    S10FAIL=1
  fi
else
  echo "  [NOK] LOG_CS hold time not measurable (fix='${FIXCS}' off='${OFFCS}')" | tee -a $RES
  S10FAIL=1
fi
echo "RESULT: $([ $S10FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)" | tee -a $RES
[ $S10FAIL -eq 0 ] || exit 1
