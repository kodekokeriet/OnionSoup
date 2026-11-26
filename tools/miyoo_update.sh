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

# One SSH connection for all steps (asks for the password only once)
ssh_miyoo() { ssh -o ControlMaster=auto -o ControlPath="$work/ssh" -o ControlPersist=120 "root@$host" "$@"; }

# RetroArch is the biggest part of the build and rarely changes: leave it out
# when the Miyoo already has the same version (the installer skips it anyway)
pkg=$work/sd/RetroArch/ra_package_version.txt
if [ -f "$pkg" ] && [ "$(ssh_miyoo cat /mnt/SDCARD/RetroArch/onion_ra_version.txt 2> /dev/null)" = "$(cat "$pkg")" ]; then
	echo "RetroArch $(cat "$pkg") is already on the Miyoo, skipping it."
	rm -f "$work/sd/RetroArch/retroarch.pak" "$pkg"
fi

echo "Copying $(du -sh "$work/sd" | cut -f1) to $host..."
tar -C "$work/sd" -cf - . | ssh_miyoo 'cd /mnt/SDCARD && tar -xf - && sync && reboot'
echo "Done. The Miyoo is rebooting."
