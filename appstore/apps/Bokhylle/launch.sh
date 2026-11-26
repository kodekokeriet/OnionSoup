#!/bin/sh
# Bokhylle for OnionSoup: books are read from /mnt/SDCARD/Media/Books
appdir=$(cd "$(dirname "$0")" && pwd)
sysdir=/mnt/SDCARD/.tmp_update
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$sysdir/lib"

cd "$appdir"
"$appdir/bokhylle" 2> "$appdir/log.txt"
