#!/bin/sh
# Built by the App Store workflow inside the Miyoo toolchain container.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../../../src/miipc"
rm -rf build/miyoo
make PLATFORM=miyoo CROSS="$CROSS_COMPILE" > /dev/null
"${CROSS_COMPILE}strip" -o "$here/miipc" build/miyoo/miipc
rm -rf "$here/samples"
cp -r samples "$here/samples"
test -x "$here/miipc"
