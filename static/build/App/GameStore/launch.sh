#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update

touch /tmp/stay_awake
cd $sysdir
./bin/appStore --games
rm -f /tmp/stay_awake
