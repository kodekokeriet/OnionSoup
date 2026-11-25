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

## Test lokalt

```sh
appstore/build_catalog.sh /tmp/out "file:///tmp/out"
```
