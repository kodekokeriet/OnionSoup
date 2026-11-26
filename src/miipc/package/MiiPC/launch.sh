#!/bin/sh
# Onion OS launcher for MiiPC. Lives in /mnt/SDCARD/App/MiiPC/
mydir=$(dirname "$0")
cd "$mydir"
export LD_LIBRARY_PATH="$mydir/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"
# A bit more CPU for low audio latency
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
./miipc samples/kit01 > miipc.log 2>&1
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
