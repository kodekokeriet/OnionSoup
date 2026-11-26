#!/bin/bash
# Build App Store entries for every theme in OnionUI/Themes.
# Themes are downloaded straight from that repo's release/ folder (one .zip per theme or theme pack,
# holding one folder per theme), so the original files are used unchanged. They unpack into Themes/.
# Usage: build_themes.sh <out_json> [img_dir img_base_url]
#   (needs curl, jq, git, python3-pillow; GH_TOKEN optional)
#   With img_dir, a preview thumbnail per theme is written there as <id>.png.
# The zip files are not downloaded here: the device checks each download against the git blob id
# (field git_sha1), which git already knows.
set -euo pipefail
SRC_REPO=${THEMES_REPO:-OnionUI/Themes}
BRANCH=${THEMES_BRANCH:-main}
RAW=https://raw.githubusercontent.com/$SRC_REPO/$BRANCH
out=$1
img_dir=${2:-}
img_base=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

auth=()
[ -n "${GH_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GH_TOKEN")

git clone -q --depth 1 --filter=blob:none --no-checkout -b "$BRANCH" "https://github.com/$SRC_REPO" "$work/src"
git -C "$work/src" config gc.auto 0
# Only the small files we read: theme configs, previews and the featured list
git -C "$work/src" sparse-checkout set --no-cone \
	'/themes/*/config.json' '/themes/*/preview.png' '/themes/*/*/preview.png' '/.github/data/*.txt' > /dev/null
git -C "$work/src" checkout -q

# "<blob sha> <path>" for every released zip (no download needed)
git -C "$work/src" ls-tree HEAD release/ | awk -F'\t' '$2 ~ /\.zip$/ { split($1, m, " "); print m[3] "\t" $2 }' > "$work/release.tsv"
git -C "$work/src" ls-tree -r --name-only HEAD themes/ > "$work/files.txt"
echo "OnionUI/Themes: $(wc -l < "$work/release.tsv") released themes" >&2

# Sizes from the GitHub API (one call); per-file HEAD requests if that fails
tree_sha=$(git -C "$work/src" rev-parse HEAD:release)
if curl -fsSL "${auth[@]}" "https://api.github.com/repos/$SRC_REPO/git/trees/$tree_sha" > "$work/tree.json" 2> /dev/null; then
	jq -r '.tree[] | select(.type == "blob") | .sha + "\t" + (.size | tostring)' "$work/tree.json" > "$work/sizes.tsv"
else
	: > "$work/sizes.tsv"
fi

urlencode() {
	jq -rn --arg s "$1" '$s | @uri'
}

# GitHub-style safe id: every run of unusual characters becomes a dot
safe_id() {
	printf '%s' "$1" | sed -E 's/[^A-Za-z0-9._-]+/./g; s/\.+/./g; s/^\.//; s/\.$//'
}

featured_file="$work/src/.github/data/featured.txt"
touch "$featured_file"

: > "$work/entries.jsonl"
while IFS=$'\t' read -r sha path; do
	theme=$(basename "$path" .zip)
	dir="$work/src/themes/$theme"
	if ! grep -q "^themes/$theme/" "$work/files.txt"; then
		echo "warning: no themes/ folder for '$theme'" >&2
		continue
	fi
	url="$RAW/release/$(urlencode "$theme").zip"
	size=$(awk -F'\t' -v s="$sha" '$1 == s { print $2; exit }' "$work/sizes.tsv")
	if [ -z "$size" ]; then
		size=$(curl -fsSIL "$url" | tr -d '\r' | awk 'tolower($1) == "content-length:" { n = $2 } END { print n + 0 }')
	fi

	# Theme folders inside the zip: the theme itself, or one per variant in a pack
	if grep -qxF "themes/$theme/config.json" "$work/files.txt"; then
		variants=$(jq -n --arg t "$theme" '[$t]')
	else
		variants=$(grep -F "themes/$theme/" "$work/files.txt" | awk -F/ -v t="$theme" 'NF == 4 && $4 == "config.json" { print $3 }' |
			sort -u | jq -R . | jq -s .)
	fi
	if [ "$(jq length <<< "$variants")" = "0" ]; then
		echo "warning: '$theme' has no config.json" >&2
		continue
	fi

	name=${theme%% by *}
	author=""
	[ "$name" != "$theme" ] && author=${theme#* by }
	desc=""
	if [ -f "$dir/config.json" ]; then
		cfg=$(tr -d '\r' < "$dir/config.json")
		n=$(jq -r '.name // empty' <<< "$cfg" 2> /dev/null || true)
		a=$(jq -r '.author // empty' <<< "$cfg" 2> /dev/null || true)
		d=$(jq -r '.description // empty' <<< "$cfg" 2> /dev/null || true)
		[ -n "$n" ] && name=$n
		[ -n "$a" ] && author=$a
		desc=$d
	fi
	count=$(jq length <<< "$variants")
	if [ -z "$desc" ]; then
		if [ "$count" -gt 1 ]; then
			desc="Temapakke med $count varianter. Velg hvilken du vil bruke."
		else
			desc="Tema fra Onion-fellesskapet."
		fi
	fi
	featured=$(grep -nxF "$theme" "$featured_file" | head -1 | cut -d: -f1 || true)

	id="theme-$(safe_id "$theme")"
	image=""
	if [ -n "$img_dir" ]; then
		mkdir -p "$img_dir"
		src_img="$dir/preview.png"
		[ -f "$src_img" ] || src_img="$dir/$(jq -r '.[0]' <<< "$variants")/preview.png"
		if [ -f "$src_img" ] && python3 "$here/thumb.py" "$src_img" "$img_dir/$id.png" 256 192; then
			image="$img_base/$id.png"
		fi
	fi
	jq -nc --arg id "$id" --arg name "$name" --arg author "${author:-Ukjent}" --arg v "${sha:0:7}" \
		--arg url "$url" --arg sha "$sha" --argjson size "${size:-0}" --arg desc "$desc" \
		--argjson variants "$variants" --argjson featured "${featured:-0}" --arg image "$image" \
		'{id: $id, type: "theme", name: $name, version: $v, author: $author, category: "Themes",
		  description: $desc, devices: ["283", "354"], url: $url, git_sha1: $sha, size: $size,
		  variants: $variants}
		 + (if $featured > 0 then {featured: $featured} else {} end)
		 + (if $image != "" then {image: $image} else {} end)' >> "$work/entries.jsonl"
done < "$work/release.tsv"

jq -s 'sort_by(.name | ascii_downcase)' "$work/entries.jsonl" > "$out"
echo "wrote $(jq length "$out") themes to $out" >&2
