#!/bin/sh
# Shrink box art in Roms/*/Imgs so it no longer covers the game names in the
# game list. Originals are kept in Imgs/.original/ (run again with them in
# place to reapply, or copy them back to undo). Size: BOXART_MAX="w h".
sysdir=/mnt/SDCARD/.tmp_update
touch /tmp/stay_awake
st -q -e sh -c '
set -- ${BOXART_MAX:-200 300}
n=0
for img in /mnt/SDCARD/Roms/*/Imgs/*.png; do
	[ -f "$img" ] || continue
	dir=$(dirname "$img")
	orig="$dir/.original/$(basename "$img")"
	if [ ! -f "$orig" ]; then
		mkdir -p "$dir/.original"
		cp "$img" "$orig"
	fi
	'"$sysdir"'/bin/pngScale "$orig" "$img" "$1" "$2" > /dev/null 2>&1 && n=$((n + 1))
	[ $((n % 25)) -eq 0 ] && echo "$n bilder ..."
done
echo
echo "Ferdig: $n coverbilder tilpasset (maks $1x$2)."
echo "Originalene ligger i Imgs/.original/."
sleep 3
'
rm -f /tmp/stay_awake
