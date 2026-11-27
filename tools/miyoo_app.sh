#!/bin/sh
# Install one App Store app on a Miyoo over SSH, built from any branch (also a PR).
# Usage: tools/miyoo_app.sh <miyoo-address> <AppId> [branch]   e.g. tools/miyoo_app.sh 192.168.1.50 YouTube
# Needs: gh (logged in), ssh. SSH user: onion (set MIYOO_USER to change).
# Takes <AppId>.zip from the newest successful "App Store catalog"
# run of the branch and unpacks it into /mnt/SDCARD/App/<AppId>.
set -eu
host=${1:?usage: $0 <miyoo-address> <AppId> [branch]}
app=${2:?usage: $0 <miyoo-address> <AppId> [branch]}
branch=${3:-main}
repo=kodekokeriet/OnionSoup
user=${MIYOO_USER:-onion}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run=$(gh run list -R "$repo" -b "$branch" -w appstore.yml -s success -L 1 --json databaseId -q '.[0].databaseId')
[ -n "$run" ] || { echo "No successful App Store build on $branch"; exit 1; }
echo "Downloading $app from run $run ($branch)..."
gh run download "$run" -R "$repo" -n apps -D "$work"
[ -f "$work/$app.zip" ] || { echo "$app.zip not found in run $run"; exit 1; }
echo "Copying to $host..."
ssh "$user@$host" "cat > /tmp/$app.zip &&
	mkdir -p /mnt/SDCARD/App/$app && cd /mnt/SDCARD/App/$app && unzip -o -q /tmp/$app.zip &&
	chmod +x *.sh $(echo $app | tr A-Z a-z) jfplayer 2>/dev/null; rm -f /tmp/$app.zip; sync" < "$work/$app.zip"
echo "Done. Open Apps > $app on the Miyoo (or Media if it went into a group)."
