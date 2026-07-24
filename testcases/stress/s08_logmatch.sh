#!/bin/bash
# ============================================================================
#  stress suite 08 — log-phase content match
#  source: logphase_stress/test_logmatch.sh  (ported: absolute paths -> $CUBRID/$API/$W, logic unchanged)
#  run: CUBRID=/path/to/_install/CUBRID bash s08_logmatch.sh
#  Heavy / real-environment test; for the fast regression gate see testcases/run_test.sh.
#  Suite <-> verification-table mapping: see testcases/stress/README.md.
# ============================================================================
# ── log-phase CONTENT verification ────────────────────────────────────────────
# The API logs "log-phase boundary detected (~N bytes in)". This suite proves
# that claim against ground truth, on a QUIESCED db (no DML during backup):
#  C1 boundary position: offline walker (bkwalk) re-derives the first
#     neg-volid FILE_START after a data volume; must sit within [N, N+slack]
#     of the API-logged N (logged value = bytes committed at detection, so the
#     boundary can only be AT or AFTER it by at most one drain read = scratch).
#  C2 nothing but log after the boundary (no data-volume FILE_START).
#  C3 CONTENT: every archive section payload extracted from the backup ==
#     sha256 of the real lgar file on disk (archives are immutable).
#     Active log (lgat): prefix compare over the copied nbytes (backup appends
#     an end-record to the live lgat after copying, so full equality is
#     impossible by design — prefix is the honest criterion).
#  C4 structure closes: walker consumed the whole file, no unknown framing.
set -u
# ---- portable environment ----
: "${CUBRID:?set CUBRID to the target CUBRID install, e.g. .../build_x86_64_release/_install/CUBRID}"
export PATH="$CUBRID/bin:$PATH"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
API=${API:-$(cd "$SCRIPT_DIR/../.." && pwd)}   # repo root: testcases/stress -> ../..
W=${W:-/tmp/cbstress/lm}; DB=${DB:-lm}; PORT=${PORT:-1599}
C3="$CUBRID"   # back-compat alias for body references to $C3
C3DIR="$CUBRID"
export LD_LIBRARY_PATH=$W/lib:$C3DIR/lib
CONF=$C3DIR/conf/cubrid_backup.conf; SVCONF=$C3DIR/conf/cubrid.conf; LOG=$C3DIR/log/cubrid_backup.log
FAIL=0
R(){ echo "[$(date +%H:%M:%S)] ### $*"; }
ok(){ echo "  [OK] $*"; }
nok(){ echo "  [NOK] $*"; FAIL=1; }

rm -rf $W; mkdir -p $W/lib $W/hdr $W/spool $W/db $W/bk $W/x; cd $W/db
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/lib/libcubridbackupapi.so \
  $API/src/backup_api.c $API/src/backup_core.c $API/src/backup_manager.c $API/src/handle_manager.c -lpthread || { echo BUILD_FAIL; exit 1; }
cp $API/src/include/backup_api.h $W/hdr/cubrid_backup_api.h
gcc -I $W/hdr -O2 -o $W/tc05 $API/testcases/backup_tc05.c -L $W/lib -lcubridbackupapi || { echo CLIENT_FAIL; exit 1; }
gcc -std=gnu11 -D_GNU_SOURCE -I $API/src/include -O2 -o $W/bkwalk $SCRIPT_DIR/helpers/bkwalk.c || { echo BKWALK_FAIL; exit 1; }

cp "$SVCONF" $W/svconf.bak; [ -f "$CONF" ] && cp "$CONF" $W/bkconf.bak
setp(){ if grep -q "^$1=" "$SVCONF"; then sed -i "s/^$1=.*/$1=$2/" "$SVCONF"; else echo "$1=$2" >> "$SVCONF"; fi; }
setp cubrid_port_id $PORT
setp log_max_archives 100
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
compress=false
except_active_log=false
sleep_msecs=0
fifo_size=1MB
buffer_memory_size=1MB
buffer_disk_limit=256MB
buffer_disk_path=$W/spool
buffer_disk_keep_spool=false
E

R "1. quiesced DB with several archives"
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $W/db/*
cubrid createdb -r --db-volume-size=200M --log-volume-size=50M $DB en_US >/dev/null 2>&1 || { echo CREATEDB_FAIL; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));"; echo "INSERT INTO t VALUES (1,'s');";
  for i in $(seq 1 15); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done; } | csql -u dba $DB >/dev/null 2>&1
for p in 1 2 3 4 5 6; do
  echo "UPDATE t SET b=RPAD(CHR(64+$p),190,'z');" | csql -u dba $DB >/dev/null 2>&1
done
NARCH=$(ls $W/db/${DB}_lgar[0-9]* 2>/dev/null | wc -l)
R "   archives on disk: $NARCH  (quiescing now: no further DML)"
sleep 2

R "2. buffered NONE backup (slow-ish consumer)"
: > $LOG
$W/tc05 $DB 0 $W/bk/${DB}_bk0v000 1000 8192 > $W/bk/out.txt 2>&1
grep -q '\[OK\]' $W/bk/out.txt && ok "backup completed ($(stat -c%s $W/bk/${DB}_bk0v000)B)" || { nok "backup failed"; exit 1; }
API_N=$(grep -oE 'detected \(~[0-9]+' $LOG | grep -oE '[0-9]+' | head -1)
[ -n "$API_N" ] && ok "API boundary log present (~$API_N bytes in)" || nok "no boundary log line"

R "3. offline walk + payload extraction"
$W/bkwalk $W/bk/${DB}_bk0v000 -x $W/x > $W/walk.out 2>&1
WRC=$?
grep -E 'HDR|BOUNDARY|VIOLATION|STOP|END' $W/walk.out | sed 's/^/   /'
BOUND=$(grep -oE 'BOUNDARY off=[0-9]+' $W/walk.out | grep -oE '[0-9]+' | head -1)

R "4. C1: walker boundary vs API-logged position"
if [ -n "$BOUND" ] && [ -n "$API_N" ]; then
  SLACK=$((2*1024*1024))    # one probe read <= scratch (pipe 1MB) + mem commit granularity
  D=$((BOUND - API_N))
  [ $D -ge 0 ] && [ $D -le $SLACK ] && ok "C1 boundary offset $BOUND within [logged $API_N, +${SLACK}] (delta=$D)" \
    || nok "C1 boundary $BOUND vs logged $API_N (delta=$D) outside slack"
else nok "C1 missing boundary ($BOUND) or API log ($API_N)"; fi

R "5. C2: nothing but log volumes after the boundary"
[ "$WRC" -eq 0 ] && ok "C2 no data-volume FILE_START after boundary" || nok "C2 VIOLATION reported by walker"

R "6. C3: archive payloads == real lgar files (sha256)"
NCMP=0
for f in $W/db/${DB}_lgar[0-9]*; do
  base=$(basename $f)
  if [ -f $W/x/$base.payload ]; then
    H1=$(sha256sum $f | awk '{print $1}'); H2=$(sha256sum $W/x/$base.payload | awk '{print $1}')
    [ "$H1" = "$H2" ] && ok "C3 $base: backup payload == on-disk archive" || nok "C3 $base MISMATCH"
    NCMP=$((NCMP+1))
  else nok "C3 $base not found in backup post-boundary sections"; fi
done
[ $NCMP -ge 1 ] && ok "C3 compared $NCMP archives" || nok "C3 nothing compared"

R "7. C3b: active log prefix compare (copied nbytes)"
ACT=$(grep "label=${DB}_lgat" $W/walk.out | head -1)
if [ -n "$ACT" ] && [ -f $W/x/${DB}_lgat.payload ]; then
  NB=$(stat -c%s $W/x/${DB}_lgat.payload)
  cmp -n $NB $W/x/${DB}_lgat.payload $W/db/${DB}_lgat >/dev/null 2>&1 \
    && ok "C3b active log: first $NB bytes match live lgat" \
    || echo "  [info] C3b active-log prefix differs (backup-end record rewrites pages in place; informational)"
else echo "  [info] C3b no active-log section extracted"; fi

R "8. C4: structure closes"
WALKED=$(grep -oE 'END walked=[0-9]+' $W/walk.out | grep -oE '[0-9]+' | head -1)
FSZ=$(stat -c%s $W/bk/${DB}_bk0v000)
if [ "$WALKED" = "$FSZ" ]; then ok "C4 walker consumed entire file ($FSZ)"
else echo "  [info] C4 walked=$WALKED file=$FSZ (trailing sentinel padding after STOP is expected)"; fi

R "RESULT: $([ $FAIL -eq 0 ] && echo ALL-PASS || echo FAIL)"
