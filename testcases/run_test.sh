#!/bin/bash

#set -x
cur_path=`pwd`
db_name='testdb'

: "${CUBRID:?CUBRID must be set}"

cd ..
sh build.sh || { echo "[NOK] build.sh failed"; exit 1; }
cd testcases
# drop stale artifacts so a failed build cannot be tested by mistake
rm -rf cubrid-backup-api cubrid-backup-api-*.tar.gz
cp ../build_*/cubrid-backup-api-*.tar.gz . || { echo "[NOK] package missing"; exit 1; }
tar xfz cubrid-backup-api-*.tar.gz || { echo "[NOK] untar failed"; exit 1; }
cmake . || { echo "[NOK] cmake failed"; exit 1; }
make  || { echo "[NOK] make failed"; exit 1; }

expect_val=""
function restoredb_exe()
{
	expect_val=`expect -c "
        	spawn cubrid restoredb $1 $db_name
                expect {
			The { send \"0\r\"; exp_continue }
                        eof
                }
		"`
}

rm -rf ./backup_dir/* ./restore_dir/* *_result $CUBRID/log/* $CUBRID/conf/cubrid_backup.conf

cubrid service stop
cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
cubrid server start $db_name

echo ""
echo "============================"
echo "==cubrid backup api test start"
echo ""

echo "==run backup_tc01"
./backup_tc01 $db_name 0 ./backup_dir/${db_name}_bk0v000 > backup_tc01_result 2>&1 
sleep 1
./backup_tc01 $db_name 1 ./backup_dir/${db_name}_bk1v000 >> backup_tc01_result 2>&1 
sleep 1
./backup_tc01 $db_name 2 ./backup_dir/${db_name}_bk2v000 >> backup_tc01_result 2>&1 

sleep 1
if [ `grep "cubrid backupdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 3 ]; then
	echo "[OK] cubrid_utility.log" >> backup_tc01_result
else
	echo "[NOK] cubrid_utility.log" >> backup_tc01_result
fi
echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 2"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc01_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc01_result
fi
echo ""

echo "==run restore_tc01"
./restore_tc01 $db_name 0 ./backup_dir/${db_name}_bk0v000 0 ./restore_dir/ > restore_tc01_result 2>&1
if cmp -s ./backup_dir/${db_name}_bk0v000 ./restore_dir/${db_name}_bk0v000; then
	echo "[OK] compare restore file of level 0" >> restore_tc01_result
else
	echo "[NOK] compare restore file of level 0" >> restore_tc01_result
fi
./restore_tc01 $db_name 1 ./backup_dir/${db_name}_bk1v000 0 ./restore_dir/ >> restore_tc01_result 2>&1
if cmp -s ./backup_dir/${db_name}_bk1v000 ./restore_dir/${db_name}_bk1v000; then
	echo "[OK] compare restore file of level 1" >> restore_tc01_result
else
	echo "[NOK] compare restore file of level 1" >> restore_tc01_result
fi
./restore_tc01 $db_name 2 ./backup_dir/${db_name}_bk2v000 0 ./restore_dir/ >> restore_tc01_result 2>&1
if cmp -s ./backup_dir/${db_name}_bk2v000 ./restore_dir/${db_name}_bk2v000; then
	echo "[OK] compare restore file of level 2" >> restore_tc01_result
else
	echo "[NOK] compare restore file of level 2" >> restore_tc01_result
fi
echo ""
echo "==run backup_tc02"
./backup_tc02 $db_name 0 > backup_tc02_result 2>&1 
sleep 5
./backup_tc02 $db_name 1 >> backup_tc02_result 2>&1 
sleep 1
./backup_tc02 $db_name 2 >> backup_tc02_result 2>&1 
echo ""

echo "==run restore_tc02"
./restore_tc02 $db_name 0 0 ./restore_dir > restore_tc02_result 2>&1 
./restore_tc02 $db_name 1 0 ./restore_dir >> restore_tc02_result 2>&1 
./restore_tc02 $db_name 2 0 ./restore_dir >> restore_tc02_result 2>&1 
echo ""

echo "==run backup_tc03"
./backup_tc03 ${db_name} > backup_tc03_result 2>&1 
echo ""

echo "==run restore_tc03"
./restore_tc03 ${db_name} > restore_tc03_result 2>&1 
echo ""

echo "==run backup_tc04"
rm -rf $CUBRID/log/cubrid_utility.log
./backup_tc04 $db_name 0 -1 -1 -1 -1 ./backup_dir/${db_name}_bk0v000 > backup_tc04_result 2>&1 
if [ `grep "0 \-l 0 \-\-no\-compress testdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 1 ]; then
        echo "[OK] set options" >> backup_tc04_result
else
        echo "[NOK] set options" >> backup_tc04_result
        cat $CUBRID/log/cubrid_utility.log >> backup_tc04_result
fi
rm -rf $CUBRID/log/cubrid_utility.log

echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 0"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc04_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc04_result
fi
sleep 1

./backup_tc04 $db_name 0 1 0 1 1 ./backup_dir/${db_name}_bk0v000 >> backup_tc04_result 2>&1 
if [ `grep "0 \-r \-l 0 \-\-no\-check \-z testdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 1 ]; then
        echo "[OK] set options" >> backup_tc04_result
else
        echo "[NOK] set options" >> backup_tc04_result
        cat $CUBRID/log/cubrid_utility.log >> backup_tc04_result
fi

echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 0"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc04_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc04_result
fi
rm -rf $CUBRID/log/cubrid_utility.log
sleep 1

./backup_tc04 $db_name 0 1 0 0 0 ./backup_dir/${db_name}_bk0v000 >> backup_tc04_result 2>&1 
if [ `grep "0 \-r \-l 0 \-\-no\-compress testdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 1 ]; then
        echo "[OK] set options" >> backup_tc04_result
else
        echo "[NOK] set options" >> backup_tc04_result
        cat $CUBRID/log/cubrid_utility.log >> backup_tc04_result
fi

echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 0"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc04_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc04_result
fi
rm -rf $CUBRID/log/cubrid_utility.log
sleep 1

./backup_tc04 $db_name 0 0 0 1 0 ./backup_dir/${db_name}_bk0v000 >> backup_tc04_result 2>&1 
if [ `grep "0 \-l 0 \-\-no\-check \-\-no\-compress testdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 1 ]; then
        echo "[OK] set options" >> backup_tc04_result
else
        echo "[NOK] set options" >> backup_tc04_result
        cat $CUBRID/log/cubrid_utility.log >> backup_tc04_result
fi

echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 0"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc04_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc04_result
fi
rm -rf $CUBRID/log/cubrid_utility.log
sleep 1

./backup_tc04 $db_name 0 0 0 0 1 ./backup_dir/${db_name}_bk0v000 >> backup_tc04_result 2>&1 
if [ `grep "0 \-l 0 \-z testdb" $CUBRID/log/cubrid_utility.log | wc -l` -eq 1 ]; then
        echo "[OK] set options" >> backup_tc04_result
else
        echo "[NOK] set options" >> backup_tc04_result
        cat $CUBRID/log/cubrid_utility.log >> backup_tc04_result
fi

echo ""
cubrid server stop $db_name
rm -rf $db_name
restoredb_exe "-B ./backup_dir -l 0"
cubrid server start $db_name
if [ `cubrid server status $db_name |grep "Server $db_name" |wc -l` -eq 0 ]; then
	echo "[NOK] run restoredb" >> backup_tc04_result
	cubrid deletedb $db_name
	cubrid createdb -r --db-volume-size=100M --log-volume-size=100M $db_name en_US
	cubrid server start $db_name
else
	echo "[OK] run restoredb" >> backup_tc04_result
fi
rm -rf $CUBRID/log/cubrid_utility.log
sleep 1

echo ""

# ============================================================================
# == [LIGHT] tiered-buffer / log-phase parser tests ==========================
# Fast, deterministic additions for the feature/log-phase-parser work. At this
# 100M `testdb` scale parser_ut is ~instant and backup_tc05 adds ~1 min (it
# deliberately reads slowly). Both use the same [OK]/[NOK] *_result convention as
# the cases above, so the final summary picks them up. Each is wrapped in a
# `timeout` so a hang in the slow-consumer path fails loudly instead of stalling
# the whole gate.
#   - parser_ut   : deterministic unit tests for the observational log-phase
#                   parser + memory ring (synthetic streams, NO server needed).
#   - backup_tc05 : slow consumer drives the tiered buffer (mem -> disk spool ->
#                   WAIT); the produced backup must still restore byte-for-byte.
# Heavy / real-environment suites (5GB/150GB accuracy, LOG_CS latency, fault
# injection, ...) are OPT-IN and live under  testcases/stress/  (see
# testcases/stress/run_stress.sh) — deliberately kept OUT of this quick gate.
# ============================================================================
echo ""
echo "==run parser_ut (log-phase parser + ring unit tests, no server)"
timeout 60 ./parser_ut > parser_ut_result 2>&1
put_rc=$?
if [ $put_rc -eq 124 ]; then
	echo "[NOK] parser_ut timed out" >> parser_ut_result
elif [ $put_rc -eq 0 ]; then
	echo "[OK] parser_ut (all checks passed)" >> parser_ut_result
else
	echo "[NOK] parser_ut (a check failed)" >> parser_ut_result
fi

echo ""
echo "==run backup_tc05 (slow-consumer tiered buffer -> byte-identical restore)"
mkdir -p ./spool
cat > $CUBRID/conf/cubrid_backup.conf <<EOF
[backup]
remove_archive=false
sa_mode=false
no_check=true
thread_count=1
compress=true
fifo_size=64KB
buffer_memory_size=1MB
buffer_disk_limit=64MB
buffer_disk_path=./spool
buffer_disk_keep_spool=false
EOF
rm -rf ./backup_dir/* ./restore_dir/* ./spool/*
timeout 180 ./backup_tc05 $db_name 0 ./backup_dir/${db_name}_bk0v000 3000 4096 > backup_tc05_result 2>&1
tc05_rc=$?
if [ $tc05_rc -eq 124 ]; then
	echo "[NOK] backup_tc05 timed out (possible slow-consumer hang)" >> backup_tc05_result
elif grep -q "\[OK\]" backup_tc05_result; then
	./restore_tc01 $db_name 0 ./backup_dir/${db_name}_bk0v000 0 ./restore_dir/ >> backup_tc05_result 2>&1
	if cmp -s ./backup_dir/${db_name}_bk0v000 ./restore_dir/${db_name}_bk0v000; then
		echo "[OK] backup_tc05 slow-consumer backup restores byte-identical" >> backup_tc05_result
	else
		echo "[NOK] backup_tc05 restore byte mismatch" >> backup_tc05_result
	fi
else
	echo "[NOK] backup_tc05 slow-consumer backup failed" >> backup_tc05_result
fi
rm -f $CUBRID/conf/cubrid_backup.conf
rm -rf ./spool
# == [/LIGHT] ================================================================

echo ""
echo "==run conf_test"
echo ""
sh conf_test.sh $db_name
cubrid server stop $db_name

for i in $(seq 1 10); do
	restoredb_exe "-B ./backup_dir/$i"
	if [ `echo $expect_val | grep "The following" | wc -l` -eq 1 ]; then
		echo "[NOK] set cubrid_backup.conf : restoredb_exe ($i)" >> conf_test_result
	else
		echo "[OK] set cubrid_backup.conf : restoredb_exe ($i)" >> conf_test_result
	fi
done

echo ""
echo "==cubrid backup api test end"
echo "=========================="

echo ""
cubrid service stop
cubrid deletedb $db_name
rm -rf ${db_name}_bkvinf ./backup_dir/* ./restore_dir/* $CUBRID/log/*

# verdict guards: a missing result file or a truncated run must not read as PASS
EXPECTED_RESULTS="backup_tc01 backup_tc02 backup_tc03 backup_tc04 backup_tc05 restore_tc01 restore_tc02 restore_tc03 conf_test parser_ut"
for e in $EXPECTED_RESULTS; do
	if [ ! -s "${e}_result" ]; then
		echo "[NOK] missing or empty ${e}_result" >> harness_result
	fi
done
ok_total=`cat *_result 2>/dev/null | grep -c "\[OK\]"`
if [ "$ok_total" -lt 165 ]; then
	echo "[NOK] under-run: only $ok_total [OK] markers (expected >= 100)" >> harness_result
fi

fail_count=`grep "\[NOK\]" *_result 2>/dev/null |wc -l`
echo ""
echo "==================="
if [ $fail_count -ne 0 ]; then
echo "FAILED TEST SUMMARY"
echo "-------------------"
for result in `ls *_result`
do
	if [ `cat $result |grep "\[NOK\]" |wc -l` -ne 0 ]; then
		echo ${result%_*}
	fi
done
else
	echo "ALL PASSED"
fi
echo "==================="
echo ""

[ "$fail_count" -eq 0 ] || exit 1
