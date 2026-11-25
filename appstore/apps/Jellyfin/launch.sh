#!/bin/sh
# Jellyfin for OnionSoup. The app exits with code 10 when a video should play;
# we then run ffplay and start the app again, which returns to the same screen.
appdir=$(cd "$(dirname "$0")" && pwd)
sysdir=/mnt/SDCARD/.tmp_update

cd $sysdir
touch /tmp/stay_awake
while :; do
	"$appdir/jellyfin"
	[ $? -eq 10 ] || break
	url=$(cat /tmp/jellyfin_play 2> /dev/null)
	rm -f /tmp/jellyfin_play
	[ -n "$url" ] || break
	echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
	./bin/ffplay -autoexit -vf "hflip,vflip" -i "$url"
done
rm -f /tmp/stay_awake
