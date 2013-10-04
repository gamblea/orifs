cd $TEMP_DIR

$ORI_HTTPD $SOURCE_FS &

sleep 1

$ORI_EXE replicate http://127.0.0.1:8080/ $TEST_FS

$ORIFS_EXE $SOURCE_FS
$ORIFS_EXE $TEST_FS
sleep 1

$PYTHON $SCRIPTS/compare.py "$SOURCE_FS" "$TEST_FS"

$UMOUNT $TEST_FS
$UMOUNT $SOURCE_FS

cd ~/.ori/$TEST_FS.ori
$ORIDBG_EXE verify

kill %1

cd $TEMP_DIR
$ORI_EXE removefs $TEST_FS

