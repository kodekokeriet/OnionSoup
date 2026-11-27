#!/bin/sh
# Keeps box art in Roms/*/Imgs small enough that MainUI's game list does not
# draw it over the game names. MainUI shows the image at its own size on the
# right side of the list, so a narrower image leaves room for the text.
#
#   boxart_fit.sh        fit images added or changed since the last run
#   boxart_fit.sh --all  fit every image again (from the saved originals)
#
# Originals are kept in Imgs/.original/. Size: BOXART_MAX="w h" (default 160 240).
sysdir=/mnt/SDCARD/.tmp_update
stamp=$sysdir/config/.boxartFit
lock=/tmp/boxart_fit.lock
mode=$1
set -- ${BOXART_MAX:-160 240}
max_w=$1
max_h=$2

mkdir "$lock" 2> /dev/null || exit 0
trap 'rmdir "$lock"' EXIT

newer=""
[ "$mode" != "--all" ] && [ -f "$stamp" ] && newer="-newer $stamp"
touch "$stamp.new"

n=0
for dir in /mnt/SDCARD/Roms/*/Imgs; do
    [ -d "$dir" ] || continue
    # shellcheck disable=SC2086
    find "$dir" -maxdepth 1 -name '*.png' $newer | while read -r img; do
        orig="$dir/.original/$(basename "$img")"
        if [ "$mode" != "--all" ] || [ ! -f "$orig" ]; then
            mkdir -p "$dir/.original"
            cp "$img" "$orig"
        fi
        tmp="$img.fit.tmp"
        if "$sysdir/bin/pngScale" "$orig" "$tmp" "$max_w" "$max_h" > /dev/null 2>&1; then
            mv -f "$tmp" "$img"
            touch -r "$stamp.new" "$img" 2> /dev/null
        else
            rm -f "$tmp"
        fi
    done
done
mv -f "$stamp.new" "$stamp"
