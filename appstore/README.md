# OnionSoup App Store

App Store-appen (`App/AppStore` på SD-kortet) laster ned apper over WiFi på Miyoo Mini+.
Katalogen bygges automatisk fra denne mappen.

## Slik legger du til en app

1. Lag en mappe `appstore/apps/<AppId>/`. Navnet blir mappen appen havner i: `App/<AppId>/`.
2. Legg inn filene appen trenger på SD-kortet, minst `config.json` og `launch.sh`, akkurat som en vanlig Onion-app.
3. Legg til `app.json` med info til butikken:

```json
{
    "name": "Visningsnavn",
    "description": "Kort beskrivelse",
    "version": "1.0.0",
    "author": "Deg",
    "category": "Tools",
    "devices": ["283", "354"]
}
```

`devices`: `283` = Miyoo Mini, `354` = Miyoo Mini+.

4. Merge til `main`. Workflowen `App Store catalog` pakker hver app som zip, regner ut sha256 og laster opp
   zip-filene og `catalog.json` til releasen `appstore`.

For å øke versjonen endrer du `version` i `app.json`. Da viser App Store `[update]` på enheter som har en eldre versjon.

## Porter fra Ports-Collection

Alle portene i [OnionUI/Ports-Collection](https://github.com/OnionUI/Ports-Collection) havner automatisk i App Store.
`build_ports.sh` leser den siste releasen der og lager én oppføring per port. Den originale `.7z`-filen lastes ned
direkte fra Ports-Collection og pakkes ut på roten av SD-kortet, akkurat som når du installerer manuelt.

- **Ports (free)**: komplette og gratis porter.
- **Ports (own game files)**: bare motoren. Du må selv legge inn originale spillfiler (se `_required_files.txt`).

Workflowen kjører hver mandag, så nye porter dukker opp av seg selv. App Store husker hvilke filer hver port
installerte, og når du fjerner en port, beholdes filer som andre installerte porter også bruker.
På enheten må «Ports Collection» være slått på i Package Manager.

## Temaer fra OnionUI/Themes

Alle temaene i [OnionUI/Themes](https://github.com/OnionUI/Themes) havner automatisk i fanen **Temaer**.
`build_themes.sh` leser `release/`-mappen der og lager én oppføring per tema eller temapakke, med en
forhåndsvisning laget fra temaets `preview.png`. Zip-filen lastes ned direkte fra OnionUI/Themes og pakkes ut i
`Themes/` på SD-kortet. Enheten sjekker nedlastingen mot git-blob-id-en (`git_sha1`), så CI trenger ikke laste ned
alle temaene (ca. 1 GB) for å regne ut sjekksummer.

I App Store kan man installere et tema, ta det i bruk (`themeSwitcher --apply "<mappe>"`), velge variant i en
temapakke og fjerne det igjen. Temaet som er i bruk kan ikke fjernes. Featured-listen fra OnionUI/Themes blir
hyllen «Populære temaer» på forsiden.

## Navigering

Lange lister blar opp og ned, ikke sidelengs. Forsiden viser noen få elementer per hylle og en «Se alle»-flis som
åpner hele listen. I listene gir venstre/høyre en side om gangen, og L2/R2 hopper til forrige/neste forbokstav.

## Test lokalt

```sh
appstore/build_catalog.sh /tmp/out "file:///tmp/out"
```
