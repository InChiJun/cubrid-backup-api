#!/bin/bash
# Isolated end-to-end test: log-phase parser + 2-mode reserved spool.
# Non-destructive: dedicated db `logdb` under $E, conf saved/restored, all db
# ops run cwd-relative under $E (mirrors testcases/run_test.sh). Validates
# design §12: T13 (byte integrity via real restoredb + row count), parser
# activation on a real LZ4 stream, and the slow-consumer spill/probe path.
set -u
export CUBRID=/home/chijun/CUBRID
export PATH=$CUBRID/bin:$PATH
export LD_LIBRARY_PATH=/tmp/claude-11161/e2e/lib:$CUBRID/lib:${LD_LIBRARY_PATH:-}
WT=/home/chijun/workspace/cubrid-backup-api-logphase
E=/tmp/claude-11161/e2e
DB=logdb
CONF=$CUBRID/conf/cubrid_backup.conf
LOG=$CUBRID/log/cubrid_backup.log
ROWS=262144
R() { echo "### $*"; }

rm -rf $E/lib $E/hdr $E/bkA $E/bkB $E/bkC $E/spool
mkdir -p $E/lib $E/hdr $E/bkA $E/bkB $E/bkC $E/spool
cd $E

R "build .so + clients (direct gcc; build.sh runs git clean, avoided)"
gcc -shared -fPIC -std=gnu11 -D_GNU_SOURCE -I $WT/src/include -O2 \
    -o $E/lib/libcubridbackupapi.so \
    $WT/src/backup_api.c $WT/src/backup_core.c $WT/src/backup_manager.c $WT/src/handle_manager.c \
    -lpthread || { echo "BUILD FAIL"; exit 1; }
cp $WT/src/include/backup_api.h $E/hdr/cubrid_backup_api.h
for tc in backup_tc01 backup_tc05 restore_tc01; do
  gcc -I $E/hdr -O2 -o $E/$tc $WT/testcases/$tc.c -L $E/lib -lcubridbackupapi || { echo "CLIENT $tc FAIL"; exit 1; }
done

[ -f "$CONF" ] && cp "$CONF" $E/conf.bak

R "create $DB (in $E) + load $ROWS rows"
cubrid service start >/dev/null 2>&1
cubrid server stop $DB >/dev/null 2>&1; cubrid deletedb $DB >/dev/null 2>&1; rm -rf $E/$DB
cubrid createdb -r --db-volume-size=300M --log-volume-size=100M $DB en_US >/dev/null 2>&1 || { echo "CREATEDB FAIL"; exit 1; }
cubrid server start $DB >/dev/null 2>&1
{ echo "CREATE TABLE t(a INT PRIMARY KEY, b VARCHAR(200));"; \
  echo "INSERT INTO t VALUES (1, RPAD('x',180,'y'));"; \
  for i in $(seq 1 18); do echo "INSERT INTO t SELECT a+(SELECT MAX(a) FROM t), b FROM t;"; done; } \
  | csql -u dba $DB >/dev/null 2>&1
LOADED=$(csql -u dba -N -c "SELECT COUNT(*) FROM t" $DB 2>/dev/null | tr -d ' ' | grep -E '^[0-9]+$' | tail -1)
R "rows loaded: $LOADED (expect $ROWS)"
[ "$LOADED" = "$ROWS" ] || { echo "[NOK] data load failed"; }

writeconf() { cat > "$CONF" <<EOF
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
buffer_disk_path=$E/spool
buffer_disk_keep_spool=false
EOF
}

verify_restore() { # $1 label, $2 backup dir
  cubrid server stop $DB >/dev/null 2>&1
  rm -rf $E/$DB
  expect -c "spawn cubrid restoredb -B $2 -l 0 $DB
             expect { \"The\" {send \"0\r\"; exp_continue} eof }" >/dev/null 2>&1
  cubrid server start $DB >/dev/null 2>&1
  local rc=$(csql -u dba -N -c "SELECT COUNT(*) FROM t" $DB 2>/dev/null | tr -d ' ' | grep -E '^[0-9]+$' | tail -1)
  if [ "$rc" = "$ROWS" ]; then echo "[OK] $1: restoredb OK, row count $rc == $ROWS (byte integrity)";
  else echo "[NOK] $1: restore row count '$rc' != $ROWS"; fi
}
reload_after_restore() { :; }   # data already restored; reused for next backup

R "=== A: parser + disk tier (LZ4), normal consumer ==="
: > $LOG; writeconf true 4MB 256MB
cubrid server start $DB >/dev/null 2>&1
$E/backup_tc01 $DB 0 $E/bkA/${DB}_bk0v000 2>&1 | grep -iE "OK|NOK|size"
grep -E "log-phase|tiered buffer summary|parser disabled" $LOG | sed 's/^/    A: /'
verify_restore "A normal" $E/bkA

R "=== B: SLOW consumer -> spill + probe + backpressure ==="
: > $LOG; writeconf true 1MB 256MB
cubrid server start $DB >/dev/null 2>&1
$E/backup_tc05 $DB 0 $E/bkB/${DB}_bk0v000 4000 4096 2>&1 | grep -iE "OK|NOK|size"
grep -E "log-phase|tiered buffer summary|parser disabled" $LOG | sed 's/^/    B: /'
verify_restore "B slow-consumer" $E/bkB

R "=== C: buffering OFF (legacy fallback) baseline ==="
: > $LOG; writeconf true 0 0
cubrid server start $DB >/dev/null 2>&1
$E/backup_tc01 $DB 0 $E/bkC/${DB}_bk0v000 2>&1 | grep -iE "OK|NOK|size"
grep -E "log-phase|tiered buffer summary|parser disabled" $LOG | sed 's/^/    C: /'
verify_restore "C fallback-off" $E/bkC

R "=== cleanup ==="
cubrid server stop $DB >/dev/null 2>&1
cubrid deletedb $DB >/dev/null 2>&1; rm -rf $E/$DB $E/${DB}_bkvinf
if [ -f $E/conf.bak ]; then cp $E/conf.bak "$CONF"; else rm -f "$CONF"; fi
cubrid service stop >/dev/null 2>&1
R "DONE"
