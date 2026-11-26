#!/bin/sh
# Put the newest OnionSoup build on a Miyoo over SSH, in one go.
# Usage: tools/miyoo_update.sh <miyoo-address> [branch]   (default: main; a PR branch works too)
# Needs: gh (logged in), unzip, ssh. Takes the release zip from the newest successful
# "build" run of the branch and unpacks it over /mnt/SDCARD. ROMs, saves and your
# settings in .tmp_update/config are kept; the Miyoo reboots at the end.
set -eu
host=${1:?usage: $0 <miyoo-address> [branch]}
branch=${2:-main}
repo=kodekokeriet/OnionSoup
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run=$(gh run list -R "$repo" -b "$branch" -w build.yml -s success -L 1 --json databaseId -q '.[0].databaseId')
[ -n "$run" ] || { echo "No successful build on $branch"; exit 1; }
echo "Downloading build $run ($branch)..."
gh run download "$run" -R "$repo" -D "$work/dl"
zip=$(find "$work/dl" -name '*.zip' | head -n 1)
mkdir "$work/sd"
unzip -q "$zip" -d "$work/sd"
# Keep the device's own settings: new config files are only added, never overwritten
mv "$work/sd/.tmp_update/config" "$work/sd/.tmp_update/config.new"

echo "Copying to $host..."
tar -C "$work/sd" -cf - . | ssh "root@$host" 'cd /mnt/SDCARD && tar -xf - && mkdir -p .tmp_update/config &&
	(cd .tmp_update/config.new && for f in *; do [ -e "../config/$f" ] || cp -r "$f" ../config/; done) &&
	rm -rf .tmp_update/config.new && sync && reboot'
echo "Done. The Miyoo is rebooting."
