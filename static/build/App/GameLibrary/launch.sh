#!/bin/sh
# OnionSoup game library. Starting a game writes cmd_to_run.sh and sets
# /tmp/quick_switch, so the runtime launches it as soon as we exit.
sysdir=/mnt/SDCARD/.tmp_update

cd $sysdir
./bin/gameLibrary
