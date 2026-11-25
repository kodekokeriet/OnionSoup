#!/bin/sh
# Built by the App Store workflow inside the Miyoo toolchain container.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../../../src/jellyfin"
make clean > /dev/null
BUILD_DIR="$here" make
test -x "$here/jellyfin"
