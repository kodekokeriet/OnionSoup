#!/bin/sh
# Jellyfin for OnionSoup. The app exits with code 10 when a video should play;
# we then run the player (jfplayer) and start the app again, which returns to
# the same screen.
appdir=$(cd "$(dirname "$0")" && pwd)
sysdir=/mnt/SDCARD/.tmp_update
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$sysdir/lib:$sysdir/lib/parasyte"

cd $sysdir
touch /tmp/stay_awake
while :; do
	"$appdir/jellyfin"
	[ $? -eq 10 ] || break
	[ -f /tmp/jellyfin_play ] || break
	echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
	if [ -x "$appdir/jfplayer" ]; then
		"$appdir/jfplayer" /tmp/jellyfin_play
	else
		# Fallback without on-screen controls. FFmpeg 2.4 needs the header
		# to end with CRLF.
		url=$(sed -n 's/^url=//p' /tmp/jellyfin_play)
		cr=$(printf '\r')
		nl='
'
		headers="Authorization: $(sed -n 's/^auth=//p' /tmp/jellyfin_play)$cr$nl"
		./bin/ffplay -autoexit -vf "hflip,vflip" -headers "$headers" -i "$url"
	fi
	rm -f /tmp/jellyfin_play
done
rm -f /tmp/stay_awake
