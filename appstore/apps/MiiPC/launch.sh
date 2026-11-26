#!/bin/sh
# MiiPC for OnionSoup. Lives in /mnt/SDCARD/App/MiiPC/
appdir=$(cd "$(dirname "$0")" && pwd)
sysdir=/mnt/SDCARD/.tmp_update
export LD_LIBRARY_PATH="$appdir/lib:/mnt/SDCARD/miyoo/lib:$sysdir/lib:$LD_LIBRARY_PATH"

cd "$appdir"
# A bit more CPU for low audio latency
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
"$appdir/miipc" samples/kit01 > "$appdir/log.txt" 2>&1
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
