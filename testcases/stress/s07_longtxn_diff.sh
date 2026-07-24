#!/bin/bash
# ============================================================================
#  stress suite 07 — long-txn before/after differential
#  source: logphase_stress/test_diff_longtxn.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s07_longtxn_diff.sh [abort|commit]
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── differential test: uncommitted LONG transaction spanning archived logs ────
# A long txn T changes rows, then heavy committed churn rolls T's log into ARCHIVE
# logs while T stays OPEN. A backup taken now must copy those archives and, on
# restore, recover to the backup point — where T is uncommitted, so T's rows are
# rolled back. CUBRID's exact recovery behavior here is subtle, so the GROUND
# TRUTH is equivalence: the PRE-improvement backup-api (baseline 121d9ab, direct
# FIFO) and the IMPROVED api (feature branch, tiered buffer + parser ON) must
# restore to IDENTICAL content. Run for T's fate = abort AND commit (after backup).
set -u
FATE=${1:-abort}                                   # abort | commit  (T's fate AFTER both backups)
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/dlt}; DB=${DB:-dlt}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
FIX="$API"                                         # improved api (feature branch); also the git repo for BASE_REF
BASE_REF=${BASE_REF:-121d9ab}                       # pre-improvement api, pulled from git history (no external worktree)
export LD_LIBRARY_PATH=$W/libfix:$C3/lib           # default; per-backup we swap
CONF=$C3/conf/cubrid_backup.conf; SVCONF=$C3/conf/cubrid.conf; LOG=$C3/log/cubrid_backup.log
FAIL=0
R(){ echo "[$(date +%H:%M:%S)] ### $*"; }
ok(){ echo "  [OK] $*"; }
nok(){ echo "  [NOK] $*"; FAIL=1; }

rm -rf $W; mkdir -p $W/libbase $W/libfix $W/hdr $W/spool $W/db $W/bkbase $W/bkfix $W/basesrc; cd $W/db
# pre-improvement API source straight from git history (self-contained; no cba-base worktree)
git -C $FIX archive $BASE_REF src | tar -x -C $W/basesrc || { echo BASE_EXTRACT_FAIL; exit 1; }
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $W/basesrc/src/include -O2 -o $W/libbase/libcubridbackupapi.so \
  $W/basesrc/src/backup_api.c $W/basesrc/src/backup_core.c $W/basesrc/src/backup_manager.c $W/basesrc/src/handle_manager.c -lpthread || { echo BASE_BUILD_FAIL; exit 1; }
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $FIX/src/include -O2 -o $W/libfix/libcubridbackupapi.so \
  $FIX/src/backup_api.c $FIX/src/backup_core.c $FIX/src/backup_manager.c $FIX/src/handle_manager.c -lpthread || { echo FIX_BUILD_FAIL; exit 1; }
cp $FIX/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $API/testcases/backup_tc05.c -L $W/libfix -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id $PORT
setp log_max_archives 500
cleanup(){ [ -n "${TXFD:-}" ] && exec 9>&- 2>/dev/null; pkill -f "dlt_churn" 2>/dev/null
  cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; cubrid master stop >/dev/null 2>&1
  cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"; }
trap cleanup EXIT

# baseline conf: NO buffer_* keys (pre-improvement api would reject them)
conf_base(){ cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=true
except_active_log=false
sleep_msecs=0
E
}
# fixed conf: buffering ON (tiered buffer + parser exercised) + slow consumer
conf_fix(){ cat > "$CONF" <<E
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
buffer_disk_limit=256MB
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E
}
SQL(){ csql -u dba -N -c "$1" $DB 2>/dev/null | grep -vE '^===|rows selected|Committed|^[[:space:]]*$'; }
RUN(){ csql -u dba -c "$1" $DB >/dev/null 2>&1; }
q1(){ SQL "$1" | tail -1 | tr -d " '"; }
dumpt(){ SQL "SELECT id,v FROM t ORDER BY id"; }         # ordered content for diff

R "=== FATE=$FATE : build DB, commit baseline rows, force archives with T OPEN ==="
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=200M --log-volume-size=20M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
RUN "CREATE TABLE t(id INT PRIMARY KEY, v VARCHAR(200), ck VARCHAR(32));"
RUN "CREATE TABLE churn(id INT PRIMARY KEY, b VARCHAR(200));"
# committed baseline rows B (must survive in every restore)
RUN "INSERT INTO t VALUES (1,'base-1',NULL);"
for i in $(seq 1 12); do RUN "INSERT INTO t SELECT id+(SELECT MAX(id) FROM t),'base-'||CAST(id AS VARCHAR),NULL FROM t;"; done
RUN "UPDATE t SET ck=MD5(CAST(id AS VARCHAR)||'#'||v);"
NB=$(q1 "SELECT COUNT(*) FROM t"); R "committed baseline rows = $NB"

R "open LONG txn T (uncommitted) via FIFO-controlled session; T changes rows"
rm -f $W/txpipe; mkfifo $W/txpipe
csql -u dba --no-auto-commit $DB < $W/txpipe > $W/tx.out 2>&1 &
exec 9>$W/txpipe; TXFD=1
echo "INSERT INTO t VALUES (900001,'T-uncommitted-a',MD5('900001#T-uncommitted-a'));" >&9
echo "INSERT INTO t VALUES (900002,'T-uncommitted-b',MD5('900002#T-uncommitted-b'));" >&9
echo "UPDATE t SET v='T-touched', ck=MD5(CAST(id AS VARCHAR)||'#T-touched') WHERE id IN (2,3,4);" >&9
sleep 2   # let T's statements execute and hold

R "heavy committed churn (INSERT..SELECT doubling) -> roll T's log into ARCHIVE logs (T stays open)"
RUN "INSERT INTO churn VALUES (1, RPAD('c',190,'z'));"
a0=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l); p=0
while [ $p -lt 26 ]; do
  RUN "INSERT INTO churn SELECT id+(SELECT MAX(id) FROM churn), RPAD('c',190,'z') FROM churn;"
  na=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
  [ $((na-a0)) -ge 6 ] && break
  p=$((p+1))
done
NARCH=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
R "archives now = $NARCH (T's early changes are in archived log); T still OPEN"
[ "$NARCH" -ge 3 ] && ok "enough archives to force archive-log recovery" || nok "too few archives ($NARCH) — T's log may not be archived"

R "backup #1 with PRE-IMPROVEMENT api (direct FIFO), T still open"
conf_base; : > $LOG 2>/dev/null
LD_LIBRARY_PATH=$W/libbase:$C3/lib $W/tc05 $DB 0 $W/bkbase/${DB}_bk0v000 0 65536 > $W/bkbase/out.txt 2>&1
grep -q '\[OK\]' $W/bkbase/out.txt && ok "baseline backup done ($(stat -c%s $W/bkbase/${DB}_bk0v000)B)" || nok "baseline backup failed"

R "backup #2 with IMPROVED api (tiered buffer + parser ON, slow consumer), T still open"
conf_fix; : > $LOG 2>/dev/null
LD_LIBRARY_PATH=$W/libfix:$C3/lib $W/tc05 $DB 0 $W/bkfix/${DB}_bk0v000 1500 8192 > $W/bkfix/out.txt 2>&1
grep -q '\[OK\]' $W/bkfix/out.txt && ok "improved backup done ($(stat -c%s $W/bkfix/${DB}_bk0v000)B)" || nok "improved backup failed"
grep -q 'log-phase boundary detected' $LOG && ok "improved: parser armed on this stream" || echo "  [info] improved: parser not armed"

R "resolve T on LIVE db: $FATE (AFTER both backups)"
if [ "$FATE" = "commit" ]; then echo "COMMIT;" >&9; else echo "ROLLBACK;" >&9; fi
exec 9>&-; TXFD=""; sleep 2
R "live t now has $(q1 'SELECT COUNT(*) FROM t') rows (T $FATE applied to live)"

restore_dump(){ # $1 backup dir -> writes $2 dump file; echoes "count|selfvalbad"
  cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  # -p: restore to this backup's consistent point only (T's fate on the live db
  # afterward, and any live archives it created, must not affect the restore).
  printf '0\n0\n0\n0\n' | cubrid restoredb -p -B $1 -l 0 $DB > $W/restore_$(basename $1).out 2>&1
  cubrid server start $DB >/dev/null 2>&1
  dumpt > $2
  echo "$(q1 'SELECT COUNT(*) FROM t')|$(q1 "SELECT COUNT(*) FROM t WHERE COALESCE(ck,'X')<>MD5(CAST(id AS VARCHAR)||'#'||v)")"; }

R "restore PRE-IMPROVEMENT backup -> dump"
IB=$(restore_dump $W/bkbase $W/resbase.txt); R "baseline restore: count|selfval_bad = $IB"
R "restore IMPROVED backup -> dump"
IF=$(restore_dump $W/bkfix $W/resfix.txt); R "improved restore: count|selfval_bad = $IF"

R "=== VERDICT ==="
if diff -q $W/resbase.txt $W/resfix.txt >/dev/null 2>&1; then
  ok "EQUIVALENCE: improved restore == baseline restore (row-by-row identical) — ground truth met"
else
  nok "DIVERGENCE: improved restore != baseline restore"; diff $W/resbase.txt $W/resfix.txt | head -10 | sed 's/^/      /'
fi
[ "${IB%|*}" = "${IF%|*}" ] && ok "same row count ($IB vs $IF)" || nok "row count differs base=$IB fix=$IF"
[ "${IB#*|}" = "0" ] && [ "${IF#*|}" = "0" ] && ok "both restores self-validate (0 corrupted)" || nok "self-validation nonzero base=$IB fix=$IF"
# informational: did T's uncommitted rows survive recovery? (the 'unknown' behavior)
TB=$(grep -c 'T-uncommitted\|T-touched' $W/resbase.txt); TF=$(grep -c 'T-uncommitted\|T-touched' $W/resfix.txt)
R "T's uncommitted-at-backup rows present after restore: baseline=$TB improved=$TF (must be equal; value itself is CUBRID's recovery choice)"
[ "$TB" = "$TF" ] && ok "T-row visibility identical across both apis" || nok "T-row visibility differs"

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL) (FATE=$FATE)"
