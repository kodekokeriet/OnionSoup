# Changelog

All notable changes to Onion Soup are listed here. Onion Soup is a fork of [Onion](https://github.com/OnionUI/Onion) 4.4.0 beta; everything Onion includes is still part of Onion Soup unless it is listed as removed below.

## 0.1.0 (first release)

### Added

#### App Store
- New graphical **App Store** app (`App/AppStore`) with the tabs Featured, Apps, Themes and Installed. The front page shows shelves with a few items each and a "See all" tile for the full list.
- Apps, themes and ports are downloaded over WiFi (Mini+), and every download is checked against a checksum from the catalog before it is installed. The App Store turns WiFi on by itself when it needs it.
- Installed apps show **Update** when the catalog has a newer version. Updating replaces the app's own files and keeps files you created in the app folder (samples, projects and so on).
- **Themes** from [OnionUI/Themes](https://github.com/OnionUI/Themes): install, apply, pick a variant in a theme pack, and remove. The theme in use cannot be removed. The featured list from OnionUI/Themes is shown as "Popular themes" on the front page.
- **Extra catalog sources**: add catalog URLs, one per line, to `.tmp_update/config/appstore_sources.txt`. App Store and Game Store merge them into the Onion Soup catalog.
- Long lists scroll vertically. Left/Right move a page at a time, L2/R2 jump to the previous/next first letter.
- The catalog is built automatically from `appstore/apps/` by the `App Store catalog` workflow. See [appstore/README.md](appstore/README.md) for how to add an app.

#### Game Store
- New **Game Store** app (`App/GameStore`) with the tabs Featured, Ports, Homebrew and Installed.
- **Ports** from [OnionUI/Ports-Collection](https://github.com/OnionUI/Ports-Collection), split into free ports and ports that need your own game files. The list updates itself weekly from the latest Ports-Collection release. Removing a port keeps files that other installed ports still use.
- **Homebrew games** from the [retrobrews](https://github.com/retrobrews) collections (homebrew whose authors allow free distribution), downloaded straight from the source repositories. Games are saved to `Roms/<system>/`.
- Only games for systems that have an emulator on the device are shown. Press SELECT to filter by system.
- Support for **rom hack** entries from extra sources: the Game Store finds your own copy of the original ROM by checksum, makes a copy under the hack's name and saves the patch next to it, so RetroArch applies it when the game starts. No ROMs are shipped.

#### Apps (installed from the App Store)
- **Jellyfin** (Mini+): finds Jellyfin servers on the local network and signs in with Quick Connect or user name and password. Browse libraries as poster grids and lists, play music in the background, and play video in its own player with seeking, subtitles and audio track selection. Playback progress is reported back to the server. Movies, episodes and music can be downloaded (transcoded for the Miyoo) to watch offline. No server address is built in.
- **Bookshelf**: e-book reader for EPUB and TXT. Shows the books in `Media/Books` on a shelf with covers, remembers where you are in each book, and has settings for text size, font, theme (Dark, Sepia, Light, Night), line spacing, margins and justification. Replaces PixelReader.
- **YouTube** (Mini+): popular videos, search, history and downloads for offline viewing, through Invidious (no account needed). Videos play in 360p while they download. SELECT switches to the next Invidious server.
- **MiiPC**: sample-based groovebox. 16 pads (4 banks on A/B/X/Y), chopping, linear sequencer with live recording, song mode, grid editor, mixer with per-channel effects, master effects, sample editing, resampling and WAV export, undo/redo and projects with autosave. Loads WAV files from the SD card and runs on all Miyoo Mini models. See [src/miipc/README.md](src/miipc/README.md).
- **Hello Soup**: a small example app that shows how App Store apps are packaged.

#### Apps tab
- Built-in apps are sorted into two groups, **Media** and **Settings**, so the Apps tab is shorter. App Store is listed first, then other apps, then the groups.
- Each group opens as a list in the MainUI style, with an icon made from the apps inside it.
- Press SELECT on an app inside a group to move it to another group or back to the top level, or use "+ Add app" to put an app into a group. Your choices are kept.
- Apps from the App Store can choose their group with a `"group"` key in `config.json`.
- Tweaks, Package Manager and icon packs keep working with apps that are inside a group.

#### Look and branding
- New default theme **Onion Soup**: a warm orange recolor of Silky by DiMo, with a new logo. The fallback skin and boot screen use the same colors.
- The system is called Onion Soup in the installer, Tweaks, the telnet welcome message, the web file browser and the OTA update app.
- OTA updates come from the Onion Soup releases in this repository.

#### Other
- `themeSwitcher --apply "<theme folder>"` applies a theme from the command line (used by the App Store).
- Builds: GitHub Actions builds the release zip on every push and publishes the App Store catalog.

#### Performance
- The Apps tab group sync is skipped at startup when no app, config or group setting has changed, so the main menu starts without extra work.
- App Store, Jellyfin and YouTube redraw the screen only on input or finished work (otherwise a few times a second), so background downloads get more CPU time.
- The App Store downloads cover images directly and reuses a catalog that is less than 30 minutes old.

### Fixed
- **Game list**: game names are no longer drawn under the box art in the game list. MainUI now cuts the name off where the box art starts. The earlier "Fit box art" workaround app is removed automatically and the original box art images are put back.
- **Main menu top bar**: the battery icon and percentage no longer cover the WiFi icon.

### Removed
- PixelReader, replaced by Bookshelf.

### Known limitations
- WiFi features (App Store and Game Store downloads, Jellyfin, YouTube) need a Miyoo Mini+. On the Miyoo Mini, app folders can be copied to `App/` on the SD card by hand.
- YouTube depends on public Invidious servers. Many of them currently do not serve video streams, so playback can fail depending on which servers are available.
