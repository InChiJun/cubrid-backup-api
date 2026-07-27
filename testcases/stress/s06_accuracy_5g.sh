#!/bin/bash
# ============================================================================
#  stress suite 06 — restore accuracy 5GB + incremental (SNAP)
#  source: logphase_stress/test_accuracy.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s06_accuracy_5g.sh [false|true]   # false=NONE, true=LZ4
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── accuracy suite (QUERY-ONLY verification; no file diff, no file hash) ──────
# Every table carries ck = MD5(all its data columns concatenated). Correctness
# after restore is proven ENTIRELY by SQL against the restored DB:
#   (a) self-validation : COUNT WHERE ck <> MD5(content)  must be 0   (per-row content intact)
#   (b) row COUNT: captured on the LIVE db just before each backup (shell var),
#       recompared after restore -> catches missing/extra rows. External ground truth.
#       (5GB t_bulk self-validates a bounded sample + all mutated ranges, not a full scan.)
#   (c) point/leak queries: specific ids must/must-not exist with specific values
#       (proves each level restores its own point in time; no future data leaks back).
# Traps: rollback ghost, PK-dup rollback, partial-commit txn, uncommitted txn held
#        during backup, live INSERTs racing the backup. (Automatic checkpoints during
#        backup are already covered implicitly by the long 30GB/150GB runs.)
# Usage: test_accuracy.sh [true|false]   (compress; default true)
set -u
COMPRESS=${1:-true}
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/acc}; DB=${DB:-acc}; PORT=${PORT:-1599}
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
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id $PORT
setp log_max_archives 200
cleanup(){ cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1
  cubrid master stop >/dev/null 2>&1
  cp $W/svconf.bak "$SVCONF"; [ -f $W/bkconf.bak ] && cp $W/bkconf.bak "$CONF" || rm -f "$CONF"; }
trap cleanup EXIT

cat > "$CONF" <<E
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=$COMPRESS
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=1MB
buffer_disk_limit=256MB
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E

SQL(){ csql -u dba -N -c "$1" $DB 2>/dev/null | grep -vE '^===|rows selected|Committed|^[[:space:]]*$'; }
RUN(){ csql -u dba -c "$1" $DB >/dev/null 2>&1; }
q1(){ SQL "$1" | tail -1 | tr -d " '"; }

TBLS="t_person t_order t_wide t_small t_empty t_tx t_live t_bulk"
declare -A CONTENT
CONTENT[t_person]="CAST(id AS VARCHAR)||'#'||COALESCE(email,'~')||'#'||COALESCE(name,'~')||'#'||COALESCE(TO_CHAR(birth,'YYYY-MM-DD'),'~')||'#'||COALESCE(CAST(salary AS VARCHAR),'~')||'#'||COALESCE(note,'~')"
CONTENT[t_order]="CAST(id AS VARCHAR)||'#'||CAST(person_id AS VARCHAR)||'#'||CAST(amount AS VARCHAR)||'#'||TO_CHAR(created,'YYYY-MM-DD HH24:MI:SS')||'#'||COALESCE(status,'~')"
CONTENT[t_wide]="CAST(id AS VARCHAR)||'#'||COALESCE(big,'~')"
CONTENT[t_small]="CAST(id AS VARCHAR)||'#'||COALESCE(v,'~')"
CONTENT[t_empty]="CAST(id AS VARCHAR)||'#'||COALESCE(v,'~')"
CONTENT[t_tx]="CAST(id AS VARCHAR)||'#'||COALESCE(v,'~')"
CONTENT[t_live]="CAST(id AS VARCHAR)"
CONTENT[t_bulk]="CAST(id AS VARCHAR)||'#'||COALESCE(val,'~')"

setck(){ RUN "UPDATE $1 SET ck=MD5(${CONTENT[$1]})${2:+ WHERE $2}"; }        # refresh ck (whole table or predicate)
mut(){ RUN "UPDATE $1 SET $2 WHERE $3"; setck "$1" "$3"; }                    # content change + ck refresh, same predicate

# Lightweight query-based verification (per user): row COUNT + per-row self-validation.
# COUNT (a plain integer, no csql string-quoting) catches missing/extra rows; self-
# validation (ck <> MD5(content)) catches any content corruption. For the 5GB t_bulk
# we self-validate a bounded sample + all mutated ranges (proves "last operation
# reflected" + a corruption sample) instead of a full 16.7M-row scan every level.
declare -A CNT
snapshot(){ local tag=$1 t; for t in $TBLS; do CNT[${tag}_$t]=$(q1 "SELECT COUNT(*) FROM $t"); done; }
selfval(){ local t=$1
  if [ "$t" = "t_bulk" ]; then
    q1 "SELECT COUNT(*) FROM t_bulk WHERE (id<=500000 OR (id BETWEEN 1000000 AND 1001499)) AND COALESCE(ck,'X')<>MD5(${CONTENT[t_bulk]})"
  else
    q1 "SELECT COUNT(*) FROM $t WHERE COALESCE(ck,'X')<>MD5(${CONTENT[$t]})"
  fi; }
verify(){ local tag=$1 t
  for t in $TBLS; do
    local bad=$(selfval $t) n=$(q1 "SELECT COUNT(*) FROM $t") w=${CNT[${tag}_$t]}
    [ "$bad" = "0" ] && ok "[$tag] $t self-validation 0 corrupted (${n} rows)" || nok "[$tag] $t $bad CORRUPTED rows"
    [ "$n" = "$w" ] && ok "[$tag] $t row count $n matches expected" || nok "[$tag] $t COUNT got=$n want=$w"
  done
  local c1=$(q1 "SELECT COUNT(*) FROM t_person"); local c2=$(q1 "SELECT COUNT(DISTINCT email) FROM t_person")
  [ "$c1" = "$c2" ] && ok "[$tag] unique(email) intact" || nok "[$tag] unique broken"; }

# bk_to DIR LEVEL [delay] [rbuf] : backup into its OWN dir (never clobbers the L0/L1/L2 chain)
bk_to(){ local dir=$1 L=$2 delay=${3:-300} rbuf=${4:-65536}; mkdir -p $dir; : > $LOG 2>/dev/null
  $W/tc05 $DB $L $dir/${DB}_bk${L}v000 $delay $rbuf > $dir/l$L.out 2>&1
  grep -q '\[OK\]' $dir/l$L.out && ok "backup L$L -> $(basename $dir) ($(stat -c%s $dir/${DB}_bk${L}v000)B)" || nok "backup L$L ($(basename $dir)) failed"
  grep -q 'log-phase boundary detected' $LOG && echo "  [info] L$L parser armed" || echo "  [info] L$L parser NOT armed (single-mode?) — correctness judged by restore only"; }
# canonical L0/L1/L2 chain: snapshot THIS level's archive chain right after backup,
# so an incremental restore can be given exactly the archives up to its point
# (proven in rtest3: providing the chain restores each level to its correct state,
# identically for LZ4 and NONE; wiping the chain is what FATAL'd at 5GB scale).
backup_level(){ bk_to $W/bk $1 300 65536
  mkdir -p $W/snap$1; rm -f $W/snap$1/${DB}_lgar* 2>/dev/null
  cp $W/db/${DB}_lgar[0-9]* $W/snap$1/ 2>/dev/null
  echo "  [info] L$1 archive-chain snapshot: $(ls $W/snap$1/${DB}_lgar* 2>/dev/null | wc -l) archive(s)"; }
# rest_from DIR LEVEL [SNAPDIR] : wipe volumes, restore, ASSERT restoredb + start ok.
# With SNAPDIR (canonical incremental chain): place exactly that level's archive
# snapshot, then restoredb WITHOUT -p (proven method — roll-forward stops at the
# level's point because no later archive is present; no over-roll, no FATAL).
# Without SNAPDIR (self-contained L0 trap backups): keep partial-recovery '-p' + '1'.
rest_from(){ local dir=$1 L=$2 snapdir=${3:-}; cubrid server stop $DB >/dev/null 2>&1
  rm -f $W/db/${DB} $W/db/${DB}_* $W/db/${DB}.* 2>/dev/null
  local pflag='-p'
  # FORCE_WIPE=1 : deliberately DO NOT restore the archive chain (reproduce the
  # old wipe-based restore that FATAL'd), to test whether the chain is required.
  if [ -z "${FORCE_WIPE:-}" ] && [ -n "$snapdir" ] && ls $snapdir/${DB}_lgar[0-9]* >/dev/null 2>&1; then
    cp $snapdir/${DB}_lgar[0-9]* $W/db/ 2>/dev/null; pflag=''
  fi
  printf '1\n1\n1\n1\n1\n1\n' | cubrid restoredb $pflag -B $dir -l $L $DB > $W/restore_l$L.out 2>&1
  local rrc=$?
  cubrid server start $DB > $W/start_l$L.out 2>&1
  local src=$?
  if [ $rrc -ne 0 ] || [ $src -ne 0 ] || ! csql -u dba -c "SELECT 1" $DB >/dev/null 2>&1; then
    nok "restore L$L from $(basename $dir) failed (restoredb=$rrc start=$src): $(tail -1 $W/restore_l$L.out)"; return 1; fi; return 0; }
restore_level(){ rest_from $W/bk $1 $W/snap$1; }

R "0. createdb + schema (every table has ck) — compress=$COMPRESS"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=1G --log-volume-size=300M $DB en_US.utf8 >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
csql -u dba $DB >/dev/null 2>&1 <<'EOSQL'
CREATE TABLE t_person (id INT PRIMARY KEY, email VARCHAR(60) UNIQUE, name VARCHAR(80), birth DATE, salary DECIMAL(12,2), note VARCHAR(200), ck VARCHAR(32));
CREATE INDEX ix_sal ON t_person(salary);
CREATE TABLE t_order  (id BIGINT PRIMARY KEY, person_id INT, amount NUMERIC(14,3), created TIMESTAMP, status CHAR(1), ck VARCHAR(32));
CREATE INDEX ix_po ON t_order(person_id, status);
CREATE TABLE t_wide  (id INT PRIMARY KEY, big VARCHAR(2000), ck VARCHAR(32));
CREATE TABLE t_small (id INT PRIMARY KEY, v VARCHAR(40), ck VARCHAR(32));
CREATE TABLE t_empty (id INT PRIMARY KEY, v VARCHAR(10), ck VARCHAR(32));
CREATE TABLE t_tx    (id INT PRIMARY KEY, v VARCHAR(40), ck VARCHAR(32));
CREATE TABLE t_live  (id INT PRIMARY KEY, ck VARCHAR(32));
CREATE TABLE t_bulk  (id BIGINT PRIMARY KEY, val VARCHAR(200), ck VARCHAR(32));
INSERT INTO t_person VALUES (1,'u1@x.kr','사용자_1','1990-01-02',1000.50,'첫 행',NULL);
INSERT INTO t_order  VALUES (1,1,10.001,'2026-01-01 09:00:00','N',NULL);
INSERT INTO t_wide   VALUES (1, REPEAT('x',1990),NULL);
INSERT INTO t_bulk   VALUES (1,'bulk-seed',NULL);
EOSQL
for i in $(seq 1 14); do RUN "INSERT INTO t_person SELECT id+(SELECT MAX(id) FROM t_person),
   'u'||CAST(id+(SELECT MAX(id) FROM t_person) AS VARCHAR)||'@x.kr',
   CASE MOD(id,3) WHEN 0 THEN '김철수_'||CAST(id AS VARCHAR) WHEN 1 THEN 'O''Brien\"&|;'||CAST(id AS VARCHAR) ELSE '사용자_'||CAST(id AS VARCHAR) END,
   DATE_ADD('1970-01-01', INTERVAL MOD(id,15000) DAY), CAST(MOD(id*37,900000) AS DECIMAL(12,2))/100,
   CASE MOD(id,5) WHEN 0 THEN NULL WHEN 1 THEN '' ELSE 'note-'||CAST(id AS VARCHAR) END, NULL FROM t_person;"; done
for i in $(seq 1 15); do RUN "INSERT INTO t_order SELECT id+(SELECT MAX(id) FROM t_order),
   MOD(id*13,16000)+1, CAST(MOD(id*7,1000000) AS NUMERIC(14,3))/1000, TIMESTAMP('2026-01-01 09:00:00')+MOD(id,86400),
   CASE MOD(id,4) WHEN 0 THEN 'N' WHEN 1 THEN 'P' WHEN 2 THEN 'S' ELSE 'C' END, NULL FROM t_order;"; done
for i in $(seq 1 13); do RUN "INSERT INTO t_wide SELECT id+(SELECT MAX(id) FROM t_wide), REPEAT(CHR(97+MOD(id,26)),1990),NULL FROM t_wide;"; done
RUN "DELETE FROM t_wide WHERE id>8192;"
RUN "INSERT INTO t_small SELECT ROWNUM,'small-'||CAST(ROWNUM AS VARCHAR),NULL FROM t_person WHERE ROWNUM<=100;"
R "   growing t_bulk to ~5GB..."
for i in $(seq 1 24); do RUN "INSERT INTO t_bulk SELECT id+(SELECT MAX(id) FROM t_bulk),
   'v'||CAST(MOD(id*31,99991) AS VARCHAR)||'-'||REPEAT(CHR(65+MOD(id,26)),150),NULL FROM t_bulk;"; done
R "   setting ck on all tables..."
for t in $TBLS; do setck "$t" ""; done
echo "  rows: person=$(q1 'SELECT COUNT(*) FROM t_person') order=$(q1 'SELECT COUNT(*) FROM t_order') wide=$(q1 'SELECT COUNT(*) FROM t_wide') small=$(q1 'SELECT COUNT(*) FROM t_small') bulk=$(q1 'SELECT COUNT(*) FROM t_bulk') db=$(du -sBG $W/db|cut -f1)"

R "1. TRAPS before L0: rollback ghost / PK-dup rollback / partial-commit txn"
csql -u dba --no-auto-commit $DB >/dev/null 2>&1 <<'EOSQL'
INSERT INTO t_person VALUES (999999,'ghost@x.kr','유령','2000-01-01',0,'ghost',MD5('x'));
ROLLBACK;
EOSQL
csql -u dba --no-auto-commit $DB >/dev/null 2>&1 <<'EOSQL'
INSERT INTO t_person VALUES (1,'dup@x.kr','dup','2000-01-01',0,'dup',MD5('x'));
ROLLBACK;
EOSQL
csql -u dba --no-auto-commit $DB >/dev/null 2>&1 <<'EOSQL'
INSERT INTO t_tx VALUES (10,'kept-A',NULL),(11,'kept-B',NULL),(12,'kept-C',NULL);
COMMIT;
INSERT INTO t_tx VALUES (20,'gone-A',NULL),(21,'gone-B',NULL);
ROLLBACK;
EOSQL
setck t_tx ""
ok "traps prepared (ghost/dup rolled back; t_tx: 10-12 committed, 20-21 rolled back)"

R "2. snapshot L0-state -> LEVEL 0 backup"
snapshot E0; backup_level 0

R "3. level-1 mutations (update/delete/insert, all with ck refresh) -> snapshot -> LEVEL 1"
mut t_person "salary=salary*2, note='L1-updated'" "id BETWEEN 100 AND 199"
RUN "DELETE FROM t_person WHERE id BETWEEN 300 AND 349;"
RUN "INSERT INTO t_person SELECT id+40000,'L1_'||CAST(id AS VARCHAR)||'@x.kr','L1신규',birth,salary,'L1-ins',NULL FROM t_person WHERE id<=1000;"
setck t_person "id>40000 AND id<=41000"
mut t_order "status='X'" "person_id=7"
RUN "DELETE FROM t_order WHERE id BETWEEN 400 AND 449;"
mut t_wide "big=REPEAT('M',1990)" "id<=50"
mut t_bulk "val='L1bulk-'||CAST(id AS VARCHAR)" "id BETWEEN 1000000 AND 1000999"
RUN "DELETE FROM t_bulk WHERE id BETWEEN 2000000 AND 2000099;"
snapshot E1; backup_level 1

R "4. level-2 mutations (re-insert deleted keys with new value, double-update, delete L1 rows) -> snapshot -> LEVEL 2"
RUN "INSERT INTO t_person VALUES (320,'re320@x.kr','재삽입',DATE'1999-12-31',777.77,'L2-reborn',NULL),(321,'re321@x.kr','재삽입',DATE'1999-12-31',777.77,'L2-reborn',NULL);"
setck t_person "id IN (320,321)"
mut t_person "salary=salary+1, note='L2-again'" "id BETWEEN 150 AND 159"
RUN "DELETE FROM t_person WHERE id BETWEEN 40500 AND 40599;"
RUN "DELETE FROM t_order WHERE person_id=13;"
mut t_small "v='L2-'||v" "1=1"
mut t_bulk "val='L2bulk-'||CAST(id AS VARCHAR)" "id BETWEEN 1000500 AND 1001499"
snapshot E2; backup_level 2

R "5. restore -l 0 : must be the L0-state (no later change, no ghost)"
restore_level 0; verify E0
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE id=999999")" = "0" ] && ok "[E0] rolled-back ghost absent" || nok "[E0] GHOST"
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE note='L1-updated'")" = "0" ] && ok "[E0] no level-1 update leaked back" || nok "[E0] L1 leaked into L0"
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE id BETWEEN 300 AND 349")" = "50" ] && ok "[E0] later-deleted rows still present (50)" || nok "[E0] later delete leaked"
[ "$(q1 "SELECT COUNT(*) FROM t_tx")" = "3" ] && ok "[E0] partial-commit txn: exactly 3 kept rows" || nok "[E0] t_tx count wrong"
[ "$(q1 "SELECT COUNT(*) FROM t_tx WHERE id>=20")" = "0" ] && ok "[E0] rolled-back txn rows absent" || nok "[E0] rolled-back rows present"

R "6. restore -l 1 : level-1 change visible, level-2 change absent"
restore_level 1; verify E1
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE note='L1-updated'")" = "100" ] && ok "[E1] level-1 update present (100)" || nok "[E1] L1 update wrong"
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE id BETWEEN 300 AND 349")" = "0" ] && ok "[E1] level-1 delete applied" || nok "[E1] L1 delete missing"
[ "$(q1 "SELECT COUNT(*) FROM t_person WHERE id=320 AND note='L2-reborn'")" = "0" ] && ok "[E1] level-2 re-insert absent" || nok "[E1] L2 leaked into L1"
[ "$(q1 "SELECT note FROM t_person WHERE id=155")" = "L1-updated" ] && ok "[E1] id=155 has level-1 value" || nok "[E1] id=155 wrong"
[ "$(q1 "SELECT COUNT(*) FROM t_bulk WHERE val LIKE 'L2bulk%'")" = "0" ] && ok "[E1] no level-2 bulk rows" || nok "[E1] L2 bulk leaked"

R "7. restore -l 2 : level-1 AND level-2 changes present"
restore_level 2; verify E2
[ "$(q1 "SELECT note FROM t_person WHERE id=320")" = "L2-reborn" ] && ok "[E2] deleted->re-inserted key has level-2 value" || nok "[E2] re-insert wrong"
[ "$(q1 "SELECT note FROM t_person WHERE id=155")" = "L2-again" ] && ok "[E2] double-updated key final value" || nok "[E2] id=155 wrong"
[ "$(q1 "SELECT COUNT(*) FROM t_order WHERE person_id=13")" = "0" ] && ok "[E2] level-2 order delete applied" || nok "[E2] order delete missing"
[ "$(q1 "SELECT COUNT(*) FROM t_small WHERE v NOT LIKE 'L2-%'")" = "0" ] && ok "[E2] t_small fully level-2" || nok "[E2] t_small stale"
[ "$(q1 "SELECT COUNT(*) FROM t_bulk WHERE val LIKE 'L2bulk%'")" = "1000" ] && ok "[E2] level-2 bulk rows present (1000)" || nok "[E2] L2 bulk wrong"

R "7b. FULL-TABLE content scan of the 5GB t_bulk (ALL 16.7M rows, not the 3% sample) at L2"
FULLN=$(q1 "SELECT COUNT(*) FROM t_bulk")
FULLBAD=$(q1 "SELECT COUNT(*) FROM t_bulk WHERE COALESCE(ck,'X')<>MD5(${CONTENT[t_bulk]})")
[ "$FULLBAD" = "0" ] && ok "[E2] t_bulk FULL-SCAN 0 corrupted across ALL $FULLN rows" || nok "[E2] t_bulk FULL-SCAN $FULLBAD corrupted of $FULLN"

R "8. TRAP: uncommitted txn held OPEN across the whole backup -> absent; a row committed just before backup -> present"
restore_level 2   # known-good starting state (reads the intact acc_bk0/1/2 chain)
RUN "INSERT INTO t_tx VALUES (888,'committed-just-before-backup',NULL);"; setck t_tx "id=888"
( csql -u dba --no-auto-commit $DB >/dev/null 2>&1 <<'EOSQL'
INSERT INTO t_tx VALUES (777,'uncommitted-during-backup',MD5('x'));
SELECT SLEEP(120) FROM db_root;
ROLLBACK;
EOSQL
) & TXP=$!
sleep 2; bk_to $W/bk8 0 300 65536      # own dir; leaves the L0/L1/L2 chain intact
wait $TXP 2>/dev/null
rest_from $W/bk8 0
[ "$(q1 "SELECT COUNT(*) FROM t_tx WHERE id=777")" = "0" ] && ok "[TX] uncommitted-during-backup row absent" || nok "[TX] UNCOMMITTED LEAKED"
[ "$(q1 "SELECT COUNT(*) FROM t_tx WHERE id=888")" = "1" ] && ok "[TX] committed-just-before row present (positive control)" || nok "[TX] committed-before row LOST"

R "9. TRAP: live INSERTs racing the backup -> no torn rows, prefix intact"
restore_level 2
( i=1; while [ $i -le 200000 ]; do csql -u dba -c "INSERT INTO t_live VALUES ($i, MD5(CAST($i AS VARCHAR)))" $DB >/dev/null 2>&1 || break; i=$((i+1)); done ) &
LWP=$!
sleep 2; bk_to $W/blive 0 300 65536; POST=$(q1 "SELECT COUNT(*) FROM t_live"); kill $LWP 2>/dev/null; pkill -P $LWP 2>/dev/null; wait $LWP 2>/dev/null
rest_from $W/blive 0
GOT=$(q1 "SELECT COUNT(*) FROM t_live")
BADL=$(q1 "SELECT COUNT(*) FROM t_live WHERE ck<>MD5(CAST(id AS VARCHAR))")
GAPS=$(q1 "SELECT COUNT(*) FROM t_live a WHERE id>1 AND NOT EXISTS(SELECT 1 FROM t_live b WHERE b.id=a.id-1)")
[ "$BADL" = "0" ] && ok "[LIVE] 0 torn rows among $GOT restored" || nok "[LIVE] $BADL TORN"
[ "$GAPS" = "0" ] && ok "[LIVE] committed prefix intact (no holes)" || nok "[LIVE] $GAPS holes"
[ "$GOT" -ge 1 ] && [ "$GOT" -le $((POST+5)) ] && ok "[LIVE] restored count $GOT within backup window (<=$POST)" || nok "[LIVE] count $GOT implausible"

R "10. SELF-TEST (negative control): inject one corrupted row -> detector MUST fire"
RUN "UPDATE t_small SET v='CORRUPTED-BYTE' WHERE id=1;"    # change content WITHOUT refreshing ck
nbad=$(q1 "SELECT COUNT(*) FROM t_small WHERE COALESCE(ck,'X')<>MD5(${CONTENT[t_small]})")
[ "${nbad:-0}" -ge 1 ] && ok "[SELFTEST] corruption detector works (caught $nbad injected)" || nok "[SELFTEST] detector FAILED to catch injected corruption — verify() is unsound"

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL) (compress=$COMPRESS)"
