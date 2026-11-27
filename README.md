<p>&nbsp;</p>

# Onion Soup

*A new fork of [Onion](https://github.com/OnionUI/Onion) for the Miyoo Mini and Mini+. Onion Soup picks up where Onion left off and adds a spicy life back into your Miyoo, with a set of quality of life upgrades on top of it: an App Store and a Game Store, native media apps, a tidier Apps tab, a new default theme and a number of fixes.*

<p>&nbsp;</p>

## What is Onion Soup?

Onion is an enhanced operating system for the Miyoo Mini and Mini+, with fine-tuned emulation, auto-save and resume and a wealth of customization options. Onion Soup is built on the last state of Onion (4.4.0 beta) and keeps all of that: the same emulators, RetroArch setup, Tweaks, Package Manager and save handling. What Onion Soup adds is aimed at making the device nicer to use day to day.

**Supported devices:** Miyoo Mini (including v4) and Miyoo Mini+. Features that need WiFi (App Store and Game Store downloads, Jellyfin, YouTube) work on the Mini+ only.

## Highlights

- **App Store**: a graphical store for apps, ports and themes, downloaded over WiFi and checked against a checksum before they are installed. Apps can be updated from the store and keep the files you created in their folder. You can add your own catalog sources.
- **Game Store**: ports from [Ports-Collection](https://github.com/OnionUI/Ports-Collection) and freely distributable homebrew games from the [retrobrews](https://github.com/retrobrews) collections. It only shows games for systems you have an emulator for, and you can filter by system.
- **Native apps** (installed from the App Store, no emulator needed):
  - **Jellyfin**: browse and play movies, shows and music from your own Jellyfin server, with downloads for offline viewing.
  - **Bookshelf**: an e-book reader for EPUB and TXT with a cover shelf, replacing PixelReader.
  - **YouTube**: popular videos, search and history through Invidious and Piped, no account needed.
  - **MiiPC**: a sample-based groovebox with pads, sequencer, song mode, mixer and effects.
- **Tidier Apps tab**: built-in apps are sorted into **Media** and **Settings** groups, and you can move any app between groups.
- **New default theme**: a warm recolor of Silky by DiMo.
- **Fixes and speed-ups**: game names are no longer hidden under the box art, the battery icon no longer covers the WiFi icon, and the main menu and apps do less unnecessary work.

See [CHANGELOG.md](CHANGELOG.md) for the full list.

## Installing

Download the latest `OnionSoup-v<version>.zip` from [Releases](https://github.com/kodekokeriet/OnionSoup/releases), unzip it to the root of your SD card and start the device. The installer works like Onion's and offers a fresh install, or an update that keeps your settings.

On the Mini+, Onion Soup can update itself over WiFi from this repository's releases with the **Onion Soup OTA update** app (available in Package Manager). Apps from the App Store are updated from inside the App Store.

## Documentation

Most of Onion's documentation still applies to Onion Soup, since the core system is the same:

- [Onion: Getting started](https://onionui.github.io/docs)
- [Onion: Features](https://onionui.github.io/docs/features)
- [Onion: FAQ](https://onionui.github.io/docs/faq)

Onion Soup specific documentation:

- [App Store and Game Store](appstore/README.md): how apps are packaged, catalog sources, ports, themes and homebrew
- [MiiPC](src/miipc/README.md)
- [Roadmap](ROADMAP.md)

## Building

Onion Soup is built the same way as Onion, inside the Miyoo Mini toolchain Docker image:

```sh
git clone --recursive https://github.com/kodekokeriet/OnionSoup.git
cd OnionSoup
make release   # inside aemiii91/miyoomini-toolchain
```

The GitHub Actions workflows build the release zip on every push, and the `App Store catalog` workflow packages the App Store apps and publishes the catalog.

## Credits and license

Onion Soup is a fork of [Onion](https://github.com/OnionUI/Onion) by the OnionUI team and its contributors. All credit for the operating system it builds on goes to them. The default theme is based on Silky by DiMo; the original credits are kept in `static/dist/Themes/OnionSoup/`.

Like Onion, Onion Soup is free software licensed under the [GNU General Public License v3.0](LICENSE).
