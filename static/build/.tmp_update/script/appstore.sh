#!/bin/sh
# OnionSoup App Store: browse, install, update and remove apps from an online catalog.
#
# The catalog is a JSON file built by .github/workflows/appstore.yml from appstore/apps/.
# Environment overrides (used by tests): SDCARD, APPSTORE_CATALOG_URL, APPSTORE_NO_NET, SHELLECT
#
# Without arguments it runs a terminal menu. The graphical App Store (bin/appStore) uses the
# commands below instead and reads progress from $workdir/status ("<state> <percent> <text>"):
#   appstore.sh fetch            enable wifi and download the catalog
#   appstore.sh install <id>     download, verify and install an app, port or theme
#   appstore.sh remove <id>      remove an app, port or theme installed by the App Store
#   appstore.sh apply <id> [dir] switch to an installed theme (folder <dir> of a theme pack)
#   appstore.sh image <id>       download the cover image to $workdir/img/<id>.png

SDCARD=${SDCARD:-/mnt/SDCARD}
sysdir=$SDCARD/.tmp_update
GITHUB_REPOSITORY=kodekokeriet/OnionSoup
CATALOG_URL=${APPSTORE_CATALOG_URL:-https://github.com/$GITHUB_REPOSITORY/releases/download/appstore/catalog.json}
SHELLECT=${SHELLECT:-$sysdir/script/shellect.sh}
workdir=$sysdir/appstore
catalog=$workdir/catalog.json
appdir=$SDCARD/App
themedir=$SDCARD/Themes

RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
NC='\033[0m'

status_file=$workdir/status
batch=0

status() {
	[ "$batch" = "1" ] && echo "$1" > "$status_file"
}

pause() {
	[ "$batch" = "1" ] && return
	echo -ne "${YELLOW}"
	read -n 1 -s -r -p "${1:-Press A to continue}"
	echo -ne "${NC}"
}

die() {
	if [ "$batch" = "1" ]; then
		echo "error 0 $(printf '%b' "$1" | tr '\n' ' ')" > "$status_file"
		exit 1
	fi
	echo -e "${RED}$1${NC}"
	pause "Press A to exit"
	exit 1
}

device_model() {
	cat /tmp/deviceModel 2> /dev/null
}

enable_wifi() {
	IP=$(ip route get 1 2> /dev/null | awk '{print $NF;exit}')
	if [ "$IP" = "" ]; then
		echo "Wifi is disabled - trying to enable it..."
		insmod $SDCARD/8188fu.ko
		ifconfig lo up
		/customer/app/axp_test wifion
		sleep 2
		ifconfig wlan0 up
		wpa_supplicant -B -D nl80211 -iwlan0 -c /appconfigs/wpa_supplicant.conf
		udhcpc -i wlan0 -s /etc/init.d/udhcpc.script
		sleep 3
		clear
	fi
}

sha1_of() {
	if command -v sha1sum > /dev/null 2>&1; then
		sha1sum | awk '{print $1}'
	else
		openssl dgst -sha1 | awk '{print $NF}'
	fi
}

# Git blob id of a file: sha1 of "blob <size>\0" + content
git_sha1_of() {
	{
		printf 'blob %s\000' "$(wc -c < "$1" | tr -d ' ')"
		cat "$1"
	} | sha1_of
}

sha256_of() {
	if command -v sha256sum > /dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		openssl dgst -sha256 "$1" | awk '{print $NF}'
	fi
}

fetch_catalog() {
	mkdir -p "$workdir"
	echo -n "Fetching app catalog... "
	if curl -k -s -L -f -o "$catalog.tmp" "$CATALOG_URL" && jq -e '.apps' "$catalog.tmp" > /dev/null 2>&1; then
		mv "$catalog.tmp" "$catalog"
		echo -e "${GREEN}OK${NC}"
	elif [ -f "$catalog" ]; then
		rm -f "$catalog.tmp"
		echo -e "${YELLOW}offline, using saved catalog${NC}"
	else
		rm -f "$catalog.tmp"
		die "Could not download the catalog. Check your wifi connection."
	fi
}

installed_dir=$workdir/installed

# Version of an installed app or port, empty if not installed by the App Store
installed_version() {
	case "$1" in
		port-* | theme-*) cat "$installed_dir/$1.version" 2> /dev/null ;;
		*) cat "$appdir/$1/.appstore_version" 2> /dev/null ;;
	esac
}

# Menu group of a catalog entry
group_of() {
	jq -r --arg id "$1" '.apps[] | select(.id == $id) | if .type == "app" then "Apps" else .category end' "$catalog"
}

app_field() {
	jq -r --arg id "$1" --arg f "$2" '.apps[] | select(.id == $id) | .[$f] // empty' "$catalog"
}

# Groups present in the catalog, apps first
group_lines() {
	jq -r '[.apps[] | if .type == "app" then "Apps" else .category end] | unique | sort_by(. != "Apps") | .[]' "$catalog"
}

# One line per entry in group $1: "<id>|<label>"
app_lines() {
	model=$(device_model)
	jq -r --arg m "$model" --arg g "$1" '.apps[]
		| select((if .type == "app" then "Apps" else .category end) == $g)
		| select(($m == "") or (.devices == null) or (.devices | index($m)))
		| .id + "|" + .name + "|" + .version' "$catalog" |
		while IFS='|' read -r id name version; do
			inst=$(installed_version "$id")
			if [ "$inst" = "" ]; then
				state=""
			elif [ "$inst" = "$version" ]; then
				state=" [installed]"
			else
				state=" [update]"
			fi
			echo "$id|$name$state"
		done
}

download_verified() {
	id=$1
	out=$2
	url=$(app_field "$id" url)
	sha=$(app_field "$id" sha256)
	git_sha=$(app_field "$id" git_sha1)
	size=$(app_field "$id" size)
	[ -n "$url" ] || die "Unknown app: $id"
	need_mb=$(((${size:-0} * 3) / 1048576 + 20))
	free_mb=$(df -m "$SDCARD" 2> /dev/null | awk 'NR==2{print $4}')
	if [ -n "$free_mb" ] && [ "$free_mb" -lt "$need_mb" ]; then
		die "Not enough space on the SD card (${need_mb}MB needed)."
	fi
	if [ "$batch" = "1" ]; then
		rm -f "$out"
		curl -k -L -f -s -o "$out" "$url" &
		pid=$!
		while kill -0 $pid 2> /dev/null; do
			got=0
			[ -f "$out" ] && got=$(wc -c < "$out")
			[ "${size:-0}" -gt 0 ] && status "download $((got * 100 / size)) Downloading"
			sleep 1
		done
		wait $pid || {
			rm -f "$out"
			die "Download failed."
		}
		status "verify 100 Checking"
	else
		curl -k -L -f -# -o "$out" "$url" || {
			rm -f "$out"
			die "Download failed."
		}
	fi
	if [ -n "$sha" ]; then
		actual=$(sha256_of "$out")
	elif [ -n "$git_sha" ]; then
		sha=$git_sha
		actual=$(git_sha1_of "$out")
	else
		actual=""
	fi
	if [ "$actual" != "$sha" ]; then
		rm -f "$out"
		die "Checksum mismatch, the download is corrupt."
	fi
}

install_port() {
	id=$1
	name=$(app_field "$id" name)
	version=$(app_field "$id" version)
	archive="$workdir/$id.7z"

	if [ ! -d "$SDCARD/Emu/PORTS" ]; then
		die "Ports are not enabled.\nOpen Package Manager and install\n\"Ports Collection\" first."
	fi

	echo -e "\n${BLUE}Downloading $name...${NC}"
	download_verified "$id" "$archive"

	echo "Installing..."
	status "install 100 Installing"
	mkdir -p "$installed_dir"
	7z l -slt "$archive" | awk -F' = ' '
		/^Path = / { path = $2 }
		/^Attributes = / { if (path != "" && index($2, "D") != 1) print path; path = "" }
	' > "$installed_dir/$id.files.tmp"
	if ! 7z x -aoa -y -o"$SDCARD" "$archive" > /dev/null 2>&1; then
		rm -f "$archive" "$installed_dir/$id.files.tmp"
		die "Could not unpack the port."
	fi
	mv "$installed_dir/$id.files.tmp" "$installed_dir/$id.files"
	echo -n "$version" > "$installed_dir/$id.version"
	rm -f "$archive"
	echo -e "${GREEN}$name installed.${NC}"
	status "done 100 Installed"
	if [ "$(app_field "$id" requires_game_files)" = "true" ]; then
		echo -e "\n${YELLOW}This port needs the original game files.${NC}"
		echo "See _required_files.txt in Roms/PORTS/Games/."
	fi
	echo -e "\nOpen Ports and run \"~Import ports\" to\nadd it to the game list."
	pause
}

# Remove the files a port installed, keeping files that another installed port also uses
remove_port() {
	id=$1
	list="$installed_dir/$id.files"
	[ -f "$list" ] || return
	others=$(ls "$installed_dir"/*.files 2> /dev/null | grep -vxF "$list")
	while IFS= read -r f; do
		case "$f" in
			"" | /* | *..*) continue ;;
		esac
		if [ -n "$others" ] && grep -qxF -- "$f" $others 2> /dev/null; then
			continue
		fi
		rm -f "$SDCARD/$f"
		d=$(dirname "$f")
		while [ "$d" != "." ] && [ "$d" != "Roms" ] && [ "$d" != "Roms/PORTS" ]; do
			rmdir "$SDCARD/$d" 2> /dev/null || break
			d=$(dirname "$d")
		done
	done < "$list"
	rm -f "$list" "$installed_dir/$id.version"
	echo -e "${GREEN}Removed.${NC}"
	status "done 100 Removed"
	pause
}

# Active theme folder name, e.g. "Analogue by Aemiii91"
active_theme() {
	t=$(cat "$sysdir/config/active_theme" 2> /dev/null)
	[ -n "$t" ] || t=$(jq -r '.theme // empty' "$SDCARD/.tmp_update/config/system.json" 2> /dev/null)
	basename "$t" 2> /dev/null
}

# Theme folders an installed theme added to Themes/, one per line
theme_dirs() {
	cat "$installed_dir/$1.dirs" 2> /dev/null
}

install_theme() {
	id=$1
	name=$(app_field "$id" name)
	version=$(app_field "$id" version)
	zip="$workdir/$id.zip"

	echo -e "\n${BLUE}Downloading $name...${NC}"
	download_verified "$id" "$zip"

	echo "Installing..."
	status "install 100 Installing"
	mkdir -p "$installed_dir" "$themedir"
	# Top-level folders in the zip, keeping only real theme folders
	7z l -slt "$zip" | awk -F' = ' '/^Path = / { p = $2; sub(/[\/\\].*/, "", p); if (p != "") print p }' | sort -u |
		while IFS= read -r d; do
			case "$d" in
				.* | *..*) ;;
				*) echo "$d" ;;
			esac
		done > "$installed_dir/$id.dirs.tmp"
	if [ ! -s "$installed_dir/$id.dirs.tmp" ]; then
		rm -f "$zip" "$installed_dir/$id.dirs.tmp"
		die "The theme package is empty."
	fi
	if ! 7z x -aoa -y -o"$themedir" "$zip" > /dev/null 2>&1; then
		rm -f "$zip" "$installed_dir/$id.dirs.tmp"
		die "Could not unpack the theme."
	fi
	mv "$installed_dir/$id.dirs.tmp" "$installed_dir/$id.dirs"
	echo -n "$version" > "$installed_dir/$id.version"
	rm -f "$zip"
	echo -e "${GREEN}$name installed.${NC}"
	echo "Pick it in Themes, or choose it in the App Store."
	status "done 100 Installed"
	pause
}

remove_theme() {
	id=$1
	active=$(active_theme)
	if [ -n "$active" ] && theme_dirs "$id" | grep -qxF -- "$active"; then
		die "This theme is in use.\nSwitch to another theme first."
	fi
	theme_dirs "$id" | while IFS= read -r d; do
		case "$d" in
			"" | .* | */* | *..*) continue ;;
		esac
		rm -rf "$themedir/$d" "$themedir/.previews/$d"
	done
	rm -f "$installed_dir/$id.dirs" "$installed_dir/$id.version"
	echo -e "${GREEN}Removed.${NC}"
	status "done 100 Removed"
	pause
}

apply_theme() {
	id=$1
	d=${2:-$(theme_dirs "$id" | head -n 1)}
	theme_dirs "$id" | grep -qxF -- "$d" && [ -f "$themedir/$d/config.json" ] || die "The theme is not installed."
	status "install 100 Applying"
	themeswitcher=${THEMESWITCHER:-$sysdir/bin/themeSwitcher}
	"$themeswitcher" --apply "$d" > /dev/null 2>&1 || die "Could not switch theme."
	sync
	echo -e "${GREEN}Theme changed to $d.${NC}"
	status "done 100 Applied"
}

install_app() {
	id=$1
	name=$(app_field "$id" name)
	version=$(app_field "$id" version)
	zip="$workdir/$id.zip"

	case "$id" in
		*/* | .* | "") die "Invalid app id: $id" ;;
	esac

	echo -e "\n${BLUE}Downloading $name $version...${NC}"
	download_verified "$id" "$zip"

	echo "Installing..."
	status "install 100 Installing"
	tmp="$workdir/extract-$id"
	rm -rf "$tmp"
	mkdir -p "$tmp"
	if ! 7z x -y -o"$tmp" "$zip" > /dev/null 2>&1; then
		rm -rf "$tmp" "$zip"
		die "Could not unpack the app."
	fi
	if [ ! -f "$tmp/config.json" ]; then
		rm -rf "$tmp" "$zip"
		die "The app package has no config.json."
	fi

	rm -rf "$appdir/$id"
	mkdir -p "$appdir"
	mv "$tmp" "$appdir/$id"
	find "$appdir/$id" -name "*.sh" -exec chmod +x {} \;
	echo -n "$version" > "$appdir/$id/.appstore_version"
	rm -f "$zip"
	echo -e "${GREEN}$name $version installed.${NC}"
	status "done 100 Installed"
	pause
}

remove_app() {
	id=$1
	case "$id" in
		*/* | .* | "") return ;;
	esac
	if [ -f "$appdir/$id/.appstore_version" ]; then
		rm -rf "$appdir/$id"
		echo -e "${GREEN}Removed.${NC}"
	fi
	status "done 100 Removed"
	pause
}

show_app() {
	id=$1
	name=$(app_field "$id" name)
	version=$(app_field "$id" version)
	inst=$(installed_version "$id")
	clear
	echo -e "${BLUE}$name${NC} $version"
	echo "by $(app_field "$id" author)"
	echo
	app_field "$id" description | fold -s -w 38
	echo
	[ "$inst" != "" ] && echo -e "Installed: ${GREEN}$inst${NC}\n"

	if [ "$inst" = "" ]; then
		choices="Install\nBack"
	elif [ "$inst" = "$version" ]; then
		choices="Reinstall\nRemove\nBack"
	else
		choices="Update\nRemove\nBack"
	fi
	case "$id" in
		theme-*) [ "$inst" != "" ] && choices="Use theme\n$choices" ;;
	esac
	pause
	action=$(echo -e "$choices" | $SHELLECT -t "$name" -b "Press A to choose")
	clear
	case "$action" in
		Install | Update | Reinstall)
			case "$id" in
				port-*) install_port "$id" ;;
				theme-*) install_theme "$id" ;;
				*) install_app "$id" ;;
			esac
			;;
		"Use theme") apply_theme "$id" && echo "Restart the menu to see it." && pause ;;
		Remove)
			case "$id" in
				port-*) remove_port "$id" ;;
				theme-*) remove_theme "$id" ;;
				*) remove_app "$id" ;;
			esac
			;;
	esac
}

main() {
	clear
	echo -e "${BLUE}===== OnionSoup App Store =====${NC}\n"

	prepare_network
	fetch_catalog

	while true; do
		group=$( (group_lines; echo "Exit") | $SHELLECT -t "App Store" -b "A: open  B: exit")
		clear
		[ "$group" = "" ] || [ "$group" = "Exit" ] && break
		browse_group "$group"
	done
}

browse_group() {
	while true; do
		lines=$(app_lines "$1")
		if [ "$lines" = "" ]; then
			echo "Nothing available for this device."
			pause
			return
		fi
		choice=$( (echo "$lines" | cut -d'|' -f2-; echo "Back") | $SHELLECT -t "$1" -b "A: open  B: back")
		clear
		[ "$choice" = "" ] || [ "$choice" = "Back" ] && return
		id=$(echo "$lines" | while IFS='|' read -r i label; do
			[ "$label" = "$choice" ] && echo "$i" && break
		done)
		[ "$id" != "" ] && show_app "$id"
	done
}

prepare_network() {
	if [ "$APPSTORE_NO_NET" != "1" ]; then
		if [ "$(device_model)" = "283" ]; then
			die "The App Store needs WiFi (Miyoo Mini+).\nOn the Miyoo Mini, copy app folders\nto App/ on the SD card instead."
		fi
		enable_wifi > /dev/null 2>&1
	fi
}

fetch_image() {
	id=$1
	case "$id" in
		*/* | .* | "") exit 1 ;;
	esac
	mkdir -p "$workdir/img"
	dest="$workdir/img/$id.png"
	[ -s "$dest" ] && exit 0
	url=$(app_field "$id" image)
	[ -n "$url" ] || exit 1
	curl -k -L -f -s -o "$dest.tmp" "$url" && mv "$dest.tmp" "$dest" || {
		rm -f "$dest.tmp"
		exit 1
	}
}

run_command() {
	batch=1
	mkdir -p "$workdir"
	case "$1" in
		fetch)
			status "fetch 0 Connecting"
			prepare_network
			fetch_catalog > /dev/null
			status "done 100 Ready"
			;;
		install)
			case "$2" in
				port-*) install_port "$2" > /dev/null ;;
				theme-*) install_theme "$2" > /dev/null ;;
				*) install_app "$2" > /dev/null ;;
			esac
			;;
		remove)
			case "$2" in
				port-*) remove_port "$2" > /dev/null ;;
				theme-*) remove_theme "$2" > /dev/null ;;
				*) remove_app "$2" > /dev/null ;;
			esac
			;;
		apply) apply_theme "$2" "$3" > /dev/null ;;
		image) fetch_image "$2" ;;
		*)
			echo "unknown command: $1" >&2
			exit 2
			;;
	esac
}

if [ "$APPSTORE_SOURCE_ONLY" != "1" ]; then
	if [ $# -gt 0 ]; then
		run_command "$@"
	else
		main
	fi
fi
