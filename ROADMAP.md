# OnionSoup roadmap

OnionSoup er en fork av [Onion](https://github.com/OnionUI/Onion) for Miyoo Mini og Mini+.

## Ferdig / under arbeid

| Område | Status | PR |
| --- | --- | --- |
| Nytt navn, egen OTA og CI-bygg | Klar for merge | #1 |
| OnionSoup-tema (varm Silky) som standard | Klar for merge | #2 |
| App Store v1 (apper og alle porter fra Ports-Collection) | Klar for merge | #3 |

## Neste

- **Native app-mal**: mal og eksempel i C/SDL for apper som kjører direkte på Miyoo uten emulator, publisert i App Store.
- **App Store med grafisk grensesnitt**: SDL-versjon med ikoner og skjermbilder i stedet for terminalmenyen.

## På sikt

### Velge egne kilder i App Store
Brukeren kan legge til egne kataloger (URL til en `catalog.json`) i tillegg til den offisielle.
- Kildeliste i `.tmp_update/config/appstore_sources` (én URL per linje), med en meny i App Store for å legge til og fjerne kilder.
- Bruker samme katalogformat som i dag (`appstore/build_catalog.sh`), så hvem som helst kan lage en kilde fra et GitHub-repo.
- Hver oppføring viser hvilken kilde den kommer fra. Tydelig advarsel når man legger til en uoffisiell kilde.
- Sjekksum (sha256) er påkrevd som i dag.

### Jellyfin-kompatibel app
Spille av musikk og video fra en Jellyfin-server over WiFi (Mini+).
- Jellyfin har et åpent REST-API. Appen logger inn, blar i biblioteket og strømmer.
- Avspilling via FFplay (video) og GMU eller en egen spiller (musikk), som allerede følger med Onion.
- Må omkode på serveren til lav oppløsning/bitrate (640x480, H.264 baseline) fordi CPU-en er svak.

### YouTube-app
Søke og spille YouTube-video på Mini+.
- Hente strømme-URL-er via et lettvekts API (f.eks. Invidious- eller Piped-instans) i stedet for å kjøre yt-dlp på enheten.
- Velge lavest mulige oppløsning og spille med FFplay.
- Risiko: tredjeparts-API-er endrer seg ofte, så appen må kunne bytte instans.

### MiiPC
Musikkapp inspirert av Akai MPC, fra det andre prosjektet. Skal kjøre native på Miyoo.
- Pads på knappene (D-pad + ABXY + skulderknapper, med bank-veksling på SELECT), sampler fra SD-kortet, enkel sequencer med tempo og mønstre.
- Bygges i C med SDL og SDL_mixer via verktøykjeden, basert på native app-malen.
- Lav lydforsinkelse er viktig: små lydbuffere og forhåndslastede samples (WAV, 16-bit mono) fordi minnet er begrenset (128 MB).
- Distribueres gjennom App Store.
