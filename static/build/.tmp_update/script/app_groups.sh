#!/bin/sh
# OnionSoup app groups for the Apps tab in the main menu.
#
# The Apps tab shows every folder in App/ that has a config.json. To put an app
# in a group, this script renames its config.json to config.grouped.json, so
# MainUI no longer lists it, and shows a group entry (App/Group_*) instead.
# The app folder itself never moves, so apps with absolute paths keep working.
#
#   app_groups.sh sync [--order]   hide/show apps and groups, refresh group
#                                  icons; --order also sorts the Apps tab
#                                  (App Store, other apps, then groups)
#   app_groups.sh open <group>     show a group (run by App/Group_*/launch.sh)
#   app_groups.sh move <app> <group|top>
#
# Which group an app is in, first match wins:
#   1. config/app_groups          lines "<App folder>=<group|top>" (the user's moves)
#   2. "group" key in the app's config.json (apps from the App Store can set it)
#   3. res/app_groups.default     the built-in apps
#   4. otherwise the app stays at the top level of the Apps tab

sysdir=${SYSDIR:-/mnt/SDCARD/.tmp_update}
appdir=${APPDIR:-/mnt/SDCARD/App}
user_conf=$sysdir/config/app_groups
default_conf=$sysdir/res/app_groups.default
APPGROUP=${APPGROUP_BIN:-$sysdir/bin/appGroup}
PROMPT=${PROMPT_BIN:-$sysdir/bin/prompt}

# id folder label
APP_GROUPS="media Group_Media Medier
settings Group_Settings Innstillinger"

# Apps that hand control back to the main menu (switch tab, start a game, ...)
HANDOFF_APPS=" Expert_Mode StartGameSwitcher RandomGamePicker AdvanceMENU Guest_Mode "

json_value() { # file key
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" 2> /dev/null | head -n 1
}

config_of() { # app folder name -> path of its (possibly hidden) config
    if [ -f "$appdir/$1/config.json" ]; then
        echo "$appdir/$1/config.json"
    elif [ -f "$appdir/$1/config.grouped.json" ]; then
        echo "$appdir/$1/config.grouped.json"
    fi
}

conf_lookup() { # file app
    [ -f "$1" ] && sed -n "s/^$2=//p" "$1" | tail -n 1
}

group_ids() { echo "$APP_GROUPS" | cut -d' ' -f1; }
group_field() { echo "$APP_GROUPS" | awk -v id="$1" -v f="$2" '$1 == id { print $f }'; }
is_group_folder() { echo "$APP_GROUPS" | awk '{ print $2 }' | grep -qx "$1"; }

group_of() { # app folder name -> group id or "top"
    g=$(conf_lookup "$user_conf" "$1")
    [ -z "$g" ] && g=$(json_value "$(config_of "$1")" group)
    [ -z "$g" ] && g=$(conf_lookup "$default_conf" "$1")
    if [ -n "$g" ] && [ "$g" != top ] && [ -n "$(group_field "$g" 2)" ]; then
        echo "$g"
    else
        echo top
    fi
}

icon_path() { # app folder name -> absolute icon path
    icon=$(json_value "$(config_of "$1")" icon)
    case "$icon" in
        "") ;;
        /*) echo "$icon" ;;
        *) echo "$appdir/$1/$icon" ;;
    esac
}

list_apps() { # all app folders (not groups) that have a config
    for d in "$appdir"/*/; do
        name=$(basename "$d")
        is_group_folder "$name" && continue
        [ -n "$(config_of "$name")" ] && echo "$name"
    done
}

restore_interrupted_order() {
    # an interrupted reorder leaves folders in App.temp: put them back
    if [ -d "$appdir.temp" ]; then
        for d in "$appdir.temp"/*/; do
            [ -d "$d" ] && mv "$d" "$appdir/"
        done
        rmdir "$appdir.temp" 2> /dev/null
    fi
}

sync_groups() {
    tmp=/tmp/app_groups.$$
    mkdir -p $tmp

    for name in $(list_apps); do
        g=$(group_of "$name")
        cfg=$appdir/$name/config.json
        hidden=$appdir/$name/config.grouped.json
        if [ "$g" = top ]; then
            if [ -f "$cfg" ]; then
                rm -f "$hidden"
            else
                mv "$hidden" "$cfg"
            fi
        else
            # an update or the app itself may have written a new config.json
            [ -f "$cfg" ] && mv -f "$cfg" "$hidden"
            printf '%s\t%s\n' "$(json_value "$hidden" label)" "$name" >> $tmp/$g
        fi
    done

    for id in $(group_ids); do
        folder=$appdir/$(group_field "$id" 2)
        label=$(group_field "$id" 3)
        mkdir -p "$folder"

        if [ ! -s $tmp/$id ]; then
            # empty group: hide it
            rm -f "$folder/config.json" "$folder/members"
            continue
        fi

        sort -f $tmp/$id | cut -f2 > "$folder/members"
        count=$(wc -l < "$folder/members" | tr -d ' ')
        names=$(sort -f $tmp/$id | cut -f1 | head -n 3 | awk 'NR > 1 { printf ", " } { printf "%s", $0 }')
        [ "$count" -gt 3 ] && names="$names …"

        # group icon: collage of the first four apps, rebuilt when they change
        set --
        for name in $(head -n 4 "$folder/members"); do
            icon=$(icon_path "$name")
            [ -n "$icon" ] && set -- "$@" "$icon"
        done
        icons="$*"
        if [ $# -gt 0 ] && { [ ! -s "$folder/icon.png" ] || [ "$icons" != "$(cat "$folder/icon.sig" 2> /dev/null)" ]; }; then
            "$APPGROUP" icon "$folder/icon.png" "$@" > /dev/null 2>&1 &&
                printf '%s' "$icons" > "$folder/icon.sig"
        fi

        new_config=$(printf '{\n"label":"%s",\n"icon":"%s",\n"launch":"launch.sh",\n"description":"%s apper · %s"\n}\n' \
            "$label" "$folder/icon.png" "$count" "$names")
        if [ "$new_config" != "$(cat "$folder/config.json" 2> /dev/null)" ]; then
            echo "$new_config" > "$folder/config.json"
        fi
    done

    rm -rf $tmp
}

# MainUI lists the Apps tab in the order the folders were created on the SD card
# (see also script/app_sorter.sh), so recreating the entries sorts the tab.
sort_apps_tab() {
    wanted=/tmp/app_groups_wanted.$$
    {
        [ -f "$appdir/AppStore/config.json" ] && echo AppStore
        for name in $(list_apps); do
            [ "$name" = AppStore ] && continue
            [ -f "$appdir/$name/config.json" ] && printf '%s\t%s\n' "$(json_value "$appdir/$name/config.json" label)" "$name"
        done | sort -f | cut -f2
        for id in $(group_ids); do
            folder=$(group_field "$id" 2)
            [ -f "$appdir/$folder/config.json" ] && echo "$folder"
        done
    } > $wanted

    current=$(find "$appdir" -maxdepth 1 -type d | while read -r d; do
        [ "$d" != "$appdir" ] && [ -f "$d/config.json" ] && basename "$d"
    done)

    if [ "$current" != "$(cat $wanted)" ]; then
        mkdir -p "$appdir.temp"
        for d in "$appdir"/*/; do
            mv "$d" "$appdir.temp/"
        done
        while read -r name; do
            mv "$appdir.temp/$name" "$appdir/"
        done < $wanted
        for d in "$appdir.temp"/*/; do
            [ -d "$d" ] && mv "$d" "$appdir/"
        done
        rmdir "$appdir.temp"
        sync
    fi
    rm -f $wanted
}

set_group() { # app group
    mkdir -p "$(dirname "$user_conf")"
    touch "$user_conf"
    sed -i "/^$1=/d" "$user_conf"
    echo "$1=$2" >> "$user_conf"
    sync_groups
}

choose() { # title options... -> prints the chosen index, empty on cancel
    title=$1
    shift
    "$PROMPT" -t "$title" "$@"
    rc=$?
    [ $rc -ge 0 ] && [ $rc -lt $# ] && echo $rc
}

move_menu() { # app current-group
    app=$1
    current=$2
    label=$(json_value "$(config_of "$app")" label)
    set -- "Apps-fanen"
    targets="top"
    for id in $(group_ids); do
        [ "$id" = "$current" ] && continue
        set -- "$@" "$(group_field "$id" 3)"
        targets="$targets $id"
    done
    i=$(choose "Flytt $label til" "$@")
    [ -z "$i" ] && return
    set_group "$app" "$(echo $targets | cut -d' ' -f$((i + 1)))"
}

add_menu() { # group
    set --
    apps=""
    for name in $(list_apps); do
        [ "$name" = AppStore ] && continue
        [ "$(group_of "$name")" = top ] || continue
        set -- "$@" "$(json_value "$(config_of "$name")" label)"
        apps="$apps $name"
    done
    if [ $# -eq 0 ]; then
        "$PROMPT" -t "Legg til app" -m "Alle appene ligger allerede i en gruppe." "OK"
        return
    fi
    i=$(choose "Legg til i $(group_field "$group" 3)" "$@")
    [ -z "$i" ] && return
    set_group "$(echo $apps | cut -d' ' -f$((i + 1)))" "$group"
}

handed_off() {
    [ -f "$sysdir/.runGameSwitcher" ] || [ -f /tmp/quick_switch ] || [ -f /tmp/run_advmenu ]
}

open_group() {
    group=$1
    gfolder=$appdir/$(group_field "$group" 2)
    gtitle=$(group_field "$group" 3)
    sel=0

    while true; do
        [ -s "$gfolder/members" ] || sync_groups
        "$APPGROUP" list "$gtitle" "$gfolder/members" -s $sel
        rc=$?
        app=$(cat /tmp/app_group_choice 2> /dev/null)
        sel=$(cat /tmp/app_group_index 2> /dev/null)
        [ -z "$sel" ] && sel=0

        case $rc in
            0)
                cfg=$(config_of "$app")
                launch=$(json_value "$cfg" launch)
                [ -z "$launch" ] && continue
                (cd "$appdir/$app" && chmod a+x "./$launch" 2> /dev/null && "./$launch")
                case "$HANDOFF_APPS" in *" $app "*) break ;; esac
                handed_off && break
                ;;
            2) move_menu "$app" "$group" ;;
            3) add_menu "$group" ;;
            *) break ;;
        esac

        # the group may be empty after a move
        [ -s "$gfolder/members" ] || break
    done
}

restore_interrupted_order

case "$1" in
    sync)
        sync_groups
        if [ "$2" = "--order" ]; then
            sort_apps_tab
        fi
        ;;
    open) open_group "$2" ;;
    move) set_group "$2" "$3" ;;
    *)
        echo "usage: $0 sync [--order] | open <group> | move <app> <group|top>" >&2
        exit 64
        ;;
esac
