#!/bin/sh
# Put the newest OnionSoup build on a Miyoo over SSH, in one go.
# Usage: tools/miyoo_update.sh [--full] <miyoo-address> [branch]   (default: main; a PR branch works too)
# The SSH user is "onion" (Onion's default); use user@address or MIYOO_USER for another.
# Needs: gh (logged in), unzip, ssh, and 7z (p7zip) for quick updates.
#
# Quick update (default): unpacks the system package (onion.pak) here and sends
# only the files that differ from what is on the Miyoo. Files that get replaced
# are backed up to /mnt/SDCARD/Backup/os-update/ first (only the last update is
# kept). Settings in .tmp_update/config are never overwritten, only new ones
# are added. ROMs and saves are not touched.
#
# Full update (--full, or when 7z is missing or RetroArch changed): sends the
# whole release zip; the Onion installer then updates the system on boot.
set -eu
full=0
if [ "${1:-}" = "--full" ]; then
	full=1
	shift
fi
host=${1:?usage: $0 [--full] <miyoo-address> [branch]}
branch=${2:-main}
case "$host" in *@*) ;; *) host=${MIYOO_USER:-onion}@$host ;; esac
repo=kodekokeriet/OnionSoup
work=$(mktemp -d)
trap 'ssh -O exit -o ControlPath="$work/ssh" x 2> /dev/null; rm -rf "$work"' EXIT

# One SSH connection for all steps (asks for the password only once)
ssh_miyoo() { ssh -o ControlMaster=auto -o ControlPath="$work/ssh" -o ControlPersist=300 "$host" "$@"; }

run=$(gh run list -R "$repo" -b "$branch" -w build.yml -s success -L 1 --json databaseId -q '.[0].databaseId')
[ -n "$run" ] || { echo "No successful build on $branch"; exit 1; }
echo "Downloading build $run ($branch)..."
gh run download "$run" -R "$repo" -D "$work/dl"
zip=$(find "$work/dl" -name '*.zip' | head -n 1)
mkdir "$work/sd"
unzip -q "$zip" -d "$work/sd"
sd=$work/sd
pak=$sd/miyoo/app/.tmp_update/onion.pak

sevenzip=$(command -v 7z || command -v 7za || command -v 7zz || true)
[ -n "$sevenzip" ] || { [ $full -eq 1 ] || echo "7z not found (install p7zip-full): doing a full update."; full=1; }
[ -f "$pak" ] || full=1

# RetroArch is the biggest part and rarely changes: the quick update needs the
# Miyoo to have the same version already
ra_pkg=$sd/RetroArch/ra_package_version.txt
if [ -f "$ra_pkg" ]; then
	if [ "$(ssh_miyoo cat /mnt/SDCARD/RetroArch/onion_ra_version.txt 2> /dev/null)" = "$(cat "$ra_pkg")" ]; then
		rm -f "$sd/RetroArch/retroarch.pak" "$ra_pkg"
	elif [ $full -eq 0 ]; then
		echo "RetroArch on the Miyoo is a different version: doing a full update."
		full=1
	fi
fi

if [ $full -eq 1 ]; then
	echo "Copying $(du -sh "$sd" | cut -f1) to $host (full update)..."
	tar -C "$sd" -cf - . | ssh_miyoo 'cd /mnt/SDCARD && rm -f .tmp_update/.os_manifest && tar -xf - && sync && reboot'
	echo "Done. The Miyoo is rebooting and installs the update."
	exit 0
fi

# ---- quick update ----
new=$work/new
mkdir "$new"
"$sevenzip" x -y -bso0 -bsp0 -o"$new" "$pak"
for d in BIOS Media Themes; do
	[ -d "$sd/$d" ] && cp -a "$sd/$d" "$new/"
done
(cd "$new" && find . -type f | sed 's|^\./||' | sort) > "$work/files"
(cd "$new" && tr '\n' '\0' < "$work/files" | xargs -0 md5sum) > "$work/manifest"

echo "Comparing with the Miyoo..."
# What the Miyoo has: the manifest of the last quick update (or md5 of every
# file the first time), and which of the files exist at all
ssh_miyoo 'cd /mnt/SDCARD; m=.tmp_update/.os_manifest
	while IFS= read -r f; do [ -f "$f" ] && echo "E $f"; done
	if [ -f $m ]; then sed "s/^/M /" $m; fi' < "$work/files" > "$work/device"
if ! grep -q '^M ' "$work/device"; then
	echo "First quick update: checking the files on the Miyoo (takes a minute)..."
	sed -n 's/^E //p' "$work/device" | ssh_miyoo 'cd /mnt/SDCARD; while IFS= read -r f; do md5sum "$f"; done' |
		sed 's/^/M /' >> "$work/device"
fi

# A file is sent when it is missing on the Miyoo or its md5 differs. Settings
# (.tmp_update/config) are only sent when missing; the default config package
# (configs.pak) is sent when it changed and unpacked without overwriting.
awk '
	FNR == NR {
		if (substr($0, 1, 2) == "E ") have[substr($0, 3)] = 1
		else if (substr($0, 1, 2) == "M ") old[substr($0, 37)] = substr($0, 3, 32)
		next
	}
	{
		sum = substr($0, 1, 32); f = substr($0, 35)
		if (!(f in have)) { print f; next }
		if (f ~ /^\.tmp_update\/config\// && f != ".tmp_update/config/configs.pak") next
		if (old[f] != sum) print f
	}' "$work/device" "$work/manifest" > "$work/changed"

count=$(wc -l < "$work/changed" | tr -d ' ')
if [ "$count" -eq 0 ]; then
	echo "The Miyoo is already up to date."
	exit 0
fi
size=$(cd "$new" && tr '\n' '\0' < "$work/changed" | du -ch --files0-from=- | tail -n 1 | cut -f1)
echo "Sending $count changed files ($size)..."
head -n 20 "$work/changed" | sed 's/^/  /'
[ "$count" -gt 20 ] && echo "  ..."

# Back up the files that get replaced, then unpack the new ones
ssh_miyoo 'cd /mnt/SDCARD; b=Backup/os-update; rm -rf $b; mkdir -p $b
	while IFS= read -r f; do
		[ -f "$f" ] && mkdir -p "$b/$(dirname "$f")" && cp -p "$f" "$b/$f"
	done; true' < "$work/changed"
tar -C "$new" -cf - -T "$work/changed" | ssh_miyoo 'cd /mnt/SDCARD && tar -xf -'
ssh_miyoo 'cat > /mnt/SDCARD/.tmp_update/.os_manifest' < "$work/manifest"
ssh_miyoo 'cd /mnt/SDCARD
	export LD_LIBRARY_PATH=/lib:/config/lib:/customer/lib:/mnt/SDCARD/.tmp_update/lib
	if [ -f .tmp_update/config/configs.pak ]; then .tmp_update/bin/7z x -aos .tmp_update/config/configs.pak > /dev/null 2>&1; fi
	sync && reboot'
echo "Done. The Miyoo is rebooting. Replaced files are in /mnt/SDCARD/Backup/os-update/"
