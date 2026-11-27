#!/bin/sh
# The "Fit box art" app shrank box art to keep it off the game names. MainUI
# itself now clips the names (tools/patch_mainui.py), so remove the app and
# put the original images back.
rm -rf /mnt/SDCARD/App/BoxartFit
for dir in /mnt/SDCARD/Roms/*/Imgs/.original; do
    [ -d "$dir" ] || continue
    for img in "$dir"/*.png; do
        [ -f "$img" ] && mv -f "$img" "$dir/../"
    done
    rmdir "$dir" 2> /dev/null
done
rm -f /mnt/SDCARD/.tmp_update/config/.boxartFit
