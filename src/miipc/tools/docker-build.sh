#!/bin/sh
# Cross-compile for Miyoo Mini with the Union toolchain (Docker).
# Usage: tools/docker-build.sh   -> output in dist/MiiPC/
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tc="$here/.toolchain"
[ -d "$tc" ] || git clone --depth 1 https://github.com/shauninman/union-miyoomini-toolchain "$tc"
cd "$tc"
make .build
docker run --rm -v "$here":/root/workspace miyoomini-toolchain /bin/bash -c \
  'source /root/setup-env.sh 2>/dev/null; cd /root/workspace && make package PLATFORM=miyoo'
