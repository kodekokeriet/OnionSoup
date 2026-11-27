#!/bin/sh
# Fit all box art in Roms/*/Imgs again, so it does not cover the game names in
# the game list. New images are fitted automatically when the menu starts
# (script/boxart_fit.sh); this redoes every image from the saved originals.
sysdir=/mnt/SDCARD/.tmp_update
touch /tmp/stay_awake
st -q -e sh -c '
echo
echo "  Fitting box art, please wait ..."
while [ -d /tmp/boxart_fit.lock ]; do sleep 1; done
sh '"$sysdir"'/script/boxart_fit.sh --all
echo
echo "  Done. Originals are kept in Imgs/.original/."
sleep 3
'
rm -f /tmp/stay_awake
