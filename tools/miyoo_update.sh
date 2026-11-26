#!/bin/sh
# Put the newest OnionSoup build on a Miyoo over SSH, in one go.
# Usage: tools/miyoo_update.sh <miyoo-address> [branch]   (default: main; a PR branch works too)
# Needs: gh (logged in), unzip, ssh. Takes the release zip from the newest successful
# "build" run of the branch and unpacks it over /mnt/SDCARD, then reboots: the zip
# is an installer (.tmp_update/updater + onion.pak), which updates the system on
# boot like a normal Onion update. ROMs, saves and settings are kept.
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

echo "Copying to $host..."
tar -C "$work/sd" -cf - . | ssh "root@$host" 'cd /mnt/SDCARD && tar -xf - && sync && reboot'
echo "Done. The Miyoo is rebooting."
