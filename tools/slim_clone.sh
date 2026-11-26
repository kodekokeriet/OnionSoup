#!/bin/sh
# Make the local OnionSoup repo small: a fresh partial clone (file contents are
# only downloaded when needed) without the big prebuilt binaries from upstream
# Onion (RetroArch cores, ScummVM, ports, BIOS files). CI builds the real
# release, so they are not needed locally. Nothing changes on GitHub.
# Usage: sh tools/slim_clone.sh [repo-folder]   (default: ~/dev/onion-soup)
# Local-only folders (.miyoo) are moved into the new clone. Show the hidden
# files again with: git sparse-checkout disable
set -eu
dir=$(cd "${1:-$HOME/dev/onion-soup}" && pwd)
url=$(git -C "$dir" remote get-url origin)
branch=$(git -C "$dir" branch --show-current)

if [ -n "$(git -C "$dir" status --porcelain --ignore-submodules)" ]; then
	echo "There are uncommitted changes in $dir. Commit or stash them first."
	exit 1
fi
git -C "$dir" fetch -q origin
if [ -n "$(git -C "$dir" log --branches --not --remotes --oneline | head -n 1)" ]; then
	echo "There are commits that are not pushed (git log --branches --not --remotes)."
	exit 1
fi

echo "Before: $(du -sh "$dir" | cut -f1)"
tmp=$(mktemp -d)
git clone -q --filter=blob:none --no-checkout "$url" "$tmp/repo"
cd "$tmp/repo"
git sparse-checkout set --no-cone '/*' \
	'!/static/build/RetroArch/' \
	'!/static/packages/' \
	'!/static/dist/BIOS/' \
	'!/static/build/.tmp_update/lib/parasyte/' \
	'!/static/build/.tmp_update/bin/adv/'
git checkout -q "${branch:-main}"
for keep in .miyoo; do
	[ -e "$dir/$keep" ] && mv "$dir/$keep" "$tmp/repo/" && echo "/$keep" >> .git/info/exclude
done
cd /
rm -rf "$dir"
mv "$tmp/repo" "$dir"
rmdir "$tmp"
echo "After:  $(du -sh "$dir" | cut -f1)"
