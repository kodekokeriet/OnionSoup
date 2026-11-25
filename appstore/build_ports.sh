#!/bin/bash
# Build App Store entries for every port in OnionUI/Ports-Collection.
# Ports are installed straight from that repo's latest GitHub release (one .7z per port,
# unpacked at the SD card root), so the original files are used unchanged.
# Usage: build_ports.sh <out_json> [img_dir img_base_url]
#   (needs curl, jq, git, sha256sum, python3-pillow; GH_TOKEN optional)
#   With img_dir, a cover thumbnail per port is written there as <id>.png.
set -euo pipefail
SRC_REPO=${PORTS_REPO:-OnionUI/Ports-Collection}
out=$1
img_dir=${2:-}
img_base=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

auth=()
[ -n "${GH_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GH_TOKEN")
if [ -n "${PORTS_RELEASE_JSON:-}" ]; then
	cp "$PORTS_RELEASE_JSON" "$work/release.json" # for local testing
else
	curl -fsSL "${auth[@]}" "https://api.github.com/repos/$SRC_REPO/releases/latest" > "$work/release.json"
fi
tag=$(jq -r .tag_name "$work/release.json")
echo "Ports-Collection release $tag: $(jq '.assets|length' "$work/release.json") assets" >&2

# Folder list + which ports need the user's own game files (_required_files.txt)
git clone -q --depth 1 --filter=blob:none --no-checkout "https://github.com/$SRC_REPO" "$work/src"
if [ -n "$img_dir" ]; then
	mkdir -p "$img_dir"
	git -C "$work/src" sparse-checkout set --no-cone '/*/Roms/PORTS/Imgs/*' > /dev/null
	git -C "$work/src" checkout -q
fi
git -C "$work/src" ls-tree -d --name-only HEAD | grep -v '^\.' > "$work/folders.txt"
git -C "$work/src" ls-tree -r --name-only HEAD | grep '/_required_files.txt$' | cut -d/ -f1 | sort -u > "$work/engines.txt"

# GitHub renames uploaded assets: every run of unusual characters becomes a dot.
asset_name() {
	printf '%s' "$1" | sed -E 's/[^A-Za-z0-9._-]+/./g; s/\.+/./g; s/^\.//; s/\.$//'
}

: > "$work/entries.jsonl"
while IFS= read -r folder; do
	asset="$(asset_name "$folder").7z"
	a=$(jq -c --arg n "$asset" '.assets[] | select(.name == $n)' "$work/release.json")
	if [ -z "$a" ]; then
		echo "warning: no release asset for '$folder' (expected $asset)" >&2
		continue
	fi
	url=$(jq -r .browser_download_url <<< "$a")
	size=$(jq -r .size <<< "$a")
	sha=$(jq -r '.digest // "" | sub("^sha256:"; "")' <<< "$a")
	if [ -z "$sha" ]; then
		echo "hashing $asset" >&2
		sha=$(curl -fsSL "$url" | sha256sum | awk '{print $1}')
	fi
	if grep -qxF "$folder" "$work/engines.txt"; then
		category="Ports (own game files)"
		needs=true
		desc="Open source engine. Copy the original game files you own to the folder named in _required_files.txt."
	else
		category="Ports (free)"
		needs=false
		desc="Free and complete port, ready to play."
	fi
	id="port-$(asset_name "$folder")"
	genre=$(git -C "$work/src" ls-tree -d --name-only "HEAD:$folder/Roms/PORTS/Shortcuts" 2> /dev/null | head -1 || true)
	image=""
	if [ -n "$img_dir" ]; then
		src_img="$work/src/$folder/Roms/PORTS/Imgs/$folder.png"
		[ -f "$src_img" ] || src_img=$(ls "$work/src/$folder/Roms/PORTS/Imgs/"*.png 2> /dev/null | head -1 || true)
		if [ -n "$src_img" ] && python3 "$here/thumb.py" "$src_img" "$img_dir/$id.png"; then
			image="$img_base/$id.png"
		fi
	fi
	jq -nc --arg id "$id" --arg name "$folder" --arg v "$tag" --arg url "$url" --arg sha "$sha" \
		--argjson size "$size" --arg cat "$category" --argjson needs "$needs" --arg desc "$desc" \
		--arg genre "$genre" --arg image "$image" \
		'{id: $id, type: "port", name: $name, version: $v, author: "Onion Ports-Collection",
		  category: $cat, requires_game_files: $needs, description: $desc,
		  devices: ["283", "354"], url: $url, sha256: $sha, size: $size}
		 + (if $genre != "" then {genre: $genre} else {} end)
		 + (if $image != "" then {image: $image} else {} end)' >> "$work/entries.jsonl"
done < "$work/folders.txt"

jq -s 'sort_by(.name)' "$work/entries.jsonl" > "$out"
echo "wrote $(jq length "$out") ports to $out" >&2
