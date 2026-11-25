#!/bin/sh
# Build App Store packages and catalog.json from appstore/apps/<id>/.
# Each app folder holds app.json (store metadata) plus the files that go in App/<id>/ on the SD card.
# Usage: build_catalog.sh <out_dir> <download_base_url> [ports.json]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$1
base=$2
ports=${3:-}
mkdir -p "$out"
out=$(cd "$out" && pwd)
entries=""
for dir in "$here"/apps/*/; do
	id=$(basename "$dir")
	[ -f "$dir/app.json" ] || { echo "skip $id: no app.json" >&2; continue; }
	[ -f "$dir/config.json" ] || { echo "error: $id has no config.json" >&2; exit 1; }
	zip="$out/$id.zip"
	rm -f "$zip"
	(cd "$dir" && zip -q -r -X "$zip" . -x app.json -x cover.png -x build.sh)
	image=""
	if [ -f "$dir/cover.png" ]; then
		python3 "$here/thumb.py" "$dir/cover.png" "$out/app-$id.png"
		image="$base/app-$id.png"
	fi
	sha=$(sha256sum "$zip" | awk '{print $1}')
	size=$(wc -c < "$zip" | tr -d ' ')
	entry=$(jq -c --arg id "$id" --arg url "$base/$id.zip" --arg sha "$sha" --argjson size "$size" --arg image "$image" \
		'. + {id: $id, type: "app", url: $url, sha256: $sha, size: $size} + (if $image != "" then {image: $image} else {} end)' "$dir/app.json")
	entries="$entries$entry
"
	echo "packaged $id ($size bytes)" >&2
done
if [ -n "$ports" ]; then
	printf '%s' "$entries" | jq -s --slurpfile p "$ports" '{version: 1, apps: ((. | sort_by(.name)) + $p[0])}' > "$out/catalog.json"
else
	printf '%s' "$entries" | jq -s '{version: 1, apps: (. | sort_by(.name))}' > "$out/catalog.json"
fi
echo "wrote $out/catalog.json" >&2
