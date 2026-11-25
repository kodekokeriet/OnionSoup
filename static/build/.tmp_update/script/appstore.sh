#!/bin/sh
# OnionSoup App Store: browse, install, update and remove apps from an online catalog.
#
# The catalog is a JSON file built by .github/workflows/appstore.yml from appstore/apps/.
# Environment overrides (used by tests): SDCARD, APPSTORE_CATALOG_URL, APPSTORE_NO_NET, SHELLECT

SDCARD=${SDCARD:-/mnt/SDCARD}
sysdir=$SDCARD/.tmp_update
GITHUB_REPOSITORY=kodekokeriet/OnionSoup
CATALOG_URL=${APPSTORE_CATALOG_URL:-https://github.com/$GITHUB_REPOSITORY/releases/download/appstore/catalog.json}
SHELLECT=${SHELLECT:-$sysdir/script/shellect.sh}
workdir=$sysdir/appstore
catalog=$workdir/catalog.json
appdir=$SDCARD/App

RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
NC='\033[0m'

pause() {
	echo -ne "${YELLOW}"
	read -n 1 -s -r -p "${1:-Press A to continue}"
	echo -ne "${NC}"
}

die() {
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

# Version of an installed app, empty if not installed by the App Store
installed_version() {
	cat "$appdir/$1/.appstore_version" 2> /dev/null
}

app_field() {
	jq -r --arg id "$1" --arg f "$2" '.apps[] | select(.id == $id) | .[$f] // empty' "$catalog"
}

# One line per app: "<id>|<label>"
app_lines() {
	model=$(device_model)
	jq -r --arg m "$model" '.apps[] | select(($m == "") or (.devices == null) or (.devices | index($m))) | .id + "|" + .name + "|" + .version' "$catalog" |
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

install_app() {
	id=$1
	name=$(app_field "$id" name)
	url=$(app_field "$id" url)
	sha=$(app_field "$id" sha256)
	version=$(app_field "$id" version)
	zip="$workdir/$id.zip"

	case "$id" in
		*/* | .* | "") die "Invalid app id: $id" ;;
	esac

	echo -e "\n${BLUE}Downloading $name $version...${NC}"
	curl -k -L -f -# -o "$zip" "$url" || {
		rm -f "$zip"
		die "Download failed."
	}

	if [ "$(sha256_of "$zip")" != "$sha" ]; then
		rm -f "$zip"
		die "Checksum mismatch, the download is corrupt."
	fi

	echo "Installing..."
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
	pause
	action=$(echo -e "$choices" | $SHELLECT -t "$name" -b "Press A to choose")
	clear
	case "$action" in
		Install | Update | Reinstall) install_app "$id" ;;
		Remove) remove_app "$id" ;;
	esac
}

main() {
	clear
	echo -e "${BLUE}===== OnionSoup App Store =====${NC}\n"

	if [ "$APPSTORE_NO_NET" != "1" ]; then
		if [ "$(device_model)" = "283" ]; then
			die "The App Store needs WiFi (Miyoo Mini+).\nOn the Miyoo Mini, copy app folders\nto App/ on the SD card instead."
		fi
		enable_wifi
	fi
	fetch_catalog

	while true; do
		lines=$(app_lines)
		[ "$lines" = "" ] && die "No apps available for this device."
		choice=$( (echo "$lines" | cut -d'|' -f2-; echo "Exit") | $SHELLECT -t "App Store" -b "A: open  B: exit")
		clear
		[ "$choice" = "" ] || [ "$choice" = "Exit" ] && break
		id=$(echo "$lines" | while IFS='|' read -r i label; do
			[ "$label" = "$choice" ] && echo "$i" && break
		done)
		[ "$id" != "" ] && show_app "$id"
	done
}

[ "$APPSTORE_SOURCE_ONLY" = "1" ] || main
