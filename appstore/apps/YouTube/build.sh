#!/bin/sh
# Built by the App Store workflow inside the Miyoo toolchain container.
# jfplayer (from the Jellyfin app) is the video player.
set -e
here=$(cd "$(dirname "$0")" && pwd)
for app in youtube jfplayer; do
	cd "$here/../../../src/$app"
	make clean > /dev/null
	BUILD_DIR="$here" make
	test -x "$here/$app"
done
