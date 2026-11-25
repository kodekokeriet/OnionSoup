#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update

touch /tmp/stay_awake
cd $sysdir
if [ -x ./bin/appStore ]; then
	./bin/appStore
else
	st -q -e sh $sysdir/script/appstore.sh
fi
rm -f /tmp/stay_awake
