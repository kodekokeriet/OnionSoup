#!/bin/sh
# YouTube for OnionSoup. The app exits with code 10 when a video should play.
# A stream is downloaded with curl to the memory card while jfplayer plays it
# (FFmpeg on the Miyoo has no HTTPS); afterwards the app starts again on the
# same screen.
appdir=$(cd "$(dirname "$0")" && pwd)
sysdir=/mnt/SDCARD/.tmp_update
cache=/mnt/SDCARD/YouTube/.cache
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$sysdir/lib:$sysdir/lib/parasyte"
export PATH="$sysdir/bin:$PATH"
export YT_DIR="$appdir"

cd $sysdir
touch /tmp/stay_awake
while :; do
	"$appdir/youtube"
	[ $? -eq 10 ] || break
	[ -f /tmp/youtube_play ] || break
	echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
	url=$(sed -n 's/^stream=//p' /tmp/youtube_play)
	if [ -n "$url" ]; then
		rm -rf "$cache"
		mkdir -p "$cache"
		: > "$cache/video.mp4"
		touch "$cache/loading"
		(
			curl -s -k -L -f --retry 2 -o "$cache/video.mp4" "$url" &
			echo $! > "$cache/pid"
			wait $!
			rm -f "$cache/loading"
		) &
		{ grep -v '^stream=' /tmp/youtube_play; echo "url=$cache/video.mp4"; echo "local=1"; echo "grow=$cache/loading"; } > /tmp/youtube_jfp
		"$appdir/jfplayer" /tmp/youtube_jfp
		[ -f "$cache/pid" ] && kill "$(cat "$cache/pid")" 2> /dev/null
		sleep 1
		rm -rf "$cache" /tmp/youtube_jfp
	else
		"$appdir/jfplayer" /tmp/youtube_play
	fi
	rm -f /tmp/youtube_play /tmp/jellyfin_result
done
rm -f /tmp/stay_awake
