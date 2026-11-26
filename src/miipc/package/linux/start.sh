#!/bin/sh
# Starts MiiPC on a Linux PC. Optional: MIIPC_RES=752x560 ./start.sh  (Mini v4 screen)
cd "$(dirname "$0")"
exec ./miipc samples/kit01 "$@"
