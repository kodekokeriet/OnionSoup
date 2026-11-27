# Onion Soup App Store

The App Store app (`App/AppStore` on the SD card) downloads apps and themes over WiFi on the Miyoo Mini+.
The Game Store (`App/GameStore`, which runs `appStore --games`) does the same for ports and homebrew games.
The catalog is built automatically from this folder.

## Adding an app

1. Create a folder `appstore/apps/<AppId>/`. The name becomes the folder the app is installed to: `App/<AppId>/`.
2. Add the files the app needs on the SD card, at least `config.json` and `launch.sh`, just like a regular Onion app.
3. Add an `app.json` with the store information:

```json
{
    "name": "Display name",
    "description": "Short description",
    "version": "1.0.0",
    "author": "You",
    "category": "Tools",
    "devices": ["283", "354"]
}
```

`devices`: `283` = Miyoo Mini, `354` = Miyoo Mini+.

4. Merge to `main`. The `App Store catalog` workflow packages each app as a zip, computes its sha256 and uploads
   the zips and `catalog.json` to the `appstore` release.

**Every app fix bumps `version` in `app.json`** in the same change, and is merged to `main`.
The workflow then builds a new package, and the App Store shows **Update** on devices that have an older version.
An update replaces the app's own files and keeps files the user created in `App/<AppId>/` (samples, projects and so on).

Apps from the App Store are placed directly in the Apps tab. To place an app in a group instead, add
`"group": "media"` (Media) or `"group": "settings"` (Settings) to the app's `config.json`.
Users can always move an app themselves with SELECT inside a group. See `.tmp_update/script/app_groups.sh`.

## Ports from Ports-Collection

All ports in [OnionUI/Ports-Collection](https://github.com/OnionUI/Ports-Collection) appear in the Game Store automatically.
`build_ports.sh` reads the latest release there and creates one entry per port. The original `.7z` file is downloaded
directly from Ports-Collection and unpacked at the root of the SD card, the same as a manual install.

- **Ports (free)**: complete, free ports.
- **Ports (own game files)**: the engine only. You add the original game files yourself (see `_required_files.txt`).

The workflow runs every Monday, so new ports show up by themselves. The store remembers which files each port
installed, and when a port is removed, files that other installed ports also use are kept.
On the device, "Ports Collection" must be enabled in Package Manager.

## Themes from OnionUI/Themes

All themes in [OnionUI/Themes](https://github.com/OnionUI/Themes) appear in the **Themes** tab automatically.
`build_themes.sh` reads the `release/` folder there and creates one entry per theme or theme pack, with a
preview made from the theme's `preview.png`. The zip is downloaded directly from OnionUI/Themes and unpacked to
`Themes/` on the SD card. The device checks the download against the git blob id (`git_sha1`), so CI does not have
to download every theme (about 1 GB) to compute checksums.

In the App Store you can install a theme, apply it (`themeSwitcher --apply "<folder>"`), pick a variant in a
theme pack and remove it again. The theme in use cannot be removed. The featured list from OnionUI/Themes becomes
the "Popular themes" shelf on the front page.

## Navigation

Long lists scroll up and down, not sideways. The front page shows a few items per shelf and a "See all" tile that
opens the full list. In lists, Left/Right move one page at a time, and L2/R2 jump to the previous/next first letter.

## Testing locally

```sh
appstore/build_catalog.sh /tmp/out "file:///tmp/out"
```

## Game Store: homebrew games

`build_homebrew.py` builds the homebrew games list for the Game Store (`App/GameStore`, which runs
`appStore --games`) from the [retrobrews](https://github.com/retrobrews) collections: homebrew whose authors
allow free distribution, one repo per system. The device downloads each ROM straight from that repo, pinned
to a commit, so we never re-host the games. (romhacking.net is not used: its robots.txt disallows
automated downloads.) One entry:

```json
{"id": "game-<slug>", "type": "game", "name": "...", "author": "...", "description": "...",
 "version": "1.0", "system": "NES", "url": "https://...", "sha256": "...", "size": 40976,
 "file": "<name>.nes", "extract": false, "image": "https://...", "genre": "Platformer"}
```

`system` is the `Emu/<system>` folder the game needs: the Game Store hides games for systems
the device has no emulator for, and Select filters by system. `file` is the name saved in
`Roms/<system>/`; with `"extract": true` the download is a zip unpacked into `Roms/<system>/<file without extension>/`.

## Extra sources

Add catalog URLs, one per line, to `.tmp_update/config/appstore_sources.txt` on the SD card. App Store and
Game Store merge them into the OnionSoup catalog on every fetch (an id already in the catalog is not replaced,
and a source that fails to load is skipped).

## Rom hacks

An entry with `"type": "romhack"` (via an extra source) is a patch for a ROM the user already owns:

```json
{"id": "game-sfc-<slug>", "type": "romhack", "name": "...", "system": "SFC", "url": "https://.../hack.bps",
 "sha256": "<patch sha256>", "size": 123, "file": "<Hack name>.sfc", "base_sha1": "<sha1 of the unzipped original ROM>",
 "base_name": "Original game (USA)"}
```

The Game Store finds the original by sha1 in `Roms/<system>/`, copies it to `file` and saves the patch next to it
with the same name, so RetroArch applies it when the game starts. We never ship ROMs.

## Testing an app from a pull request

The App Store workflow also saves the app packages as an artifact on pull requests. `tools/miyoo_app.sh <miyoo-address> <AppId> [branch]`
fetches `<AppId>.zip` from the latest successful run on the branch and unpacks it to `App/<AppId>` over SSH.
