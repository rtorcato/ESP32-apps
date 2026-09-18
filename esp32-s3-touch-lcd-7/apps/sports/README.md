# sports — **Game Day**, built (first flash 2026-09-18)

A scores board: every game today across the leagues you follow, live ones
first with the clock, then the day's fixtures by start time, then finals,
your teams first in each. A tab per sport (soccer holds MLS, the Premier League, LaLiga, the Bundesliga, Serie A, Ligue 1 and the Champions League), the league named on each row, a page per game with the two
logos large and the score in the tall digits.

```sh
python3 tools/make-team-logos.py --size 32 --out data/logo/32     # the teams playing this week
python3 tools/make-team-logos.py --size 96 --out data/logo/96
pio run -e sports -t upload
./push-config sports                                              # config + logos
```

## Where the scores come from

ESPN's own site scoreboard, `site.web.api.espn.com/apis/v2/scoreboard/header`,
keyless. (The public `site.api.espn.com` host refuses every request with an
Akamai denial; this one, the strip at the top of espn.com, answers.) One
request per league, 10KB to 260KB each, read whole into a 1MB PSRAM buffer (a Champions League matchday is 725KB, 75 games)
and parsed through an ArduinoJson filter with the nesting limit raised to 40.
Each event carries the two teams (abbreviation, name, score, winner), the
state (pre / in / post), a summary ("Final", "3rd 4:12", "9/20 - 1:00 PM
EDT"), the start time in UTC, the broadcaster, and a 500px PNG logo per team.

A league with a live game is refreshed every `refresh.liveSeconds` (60), the
others every `refresh.idleSeconds` (600). One league per pass, so the screen
stays live between.

## config.json

| key | what |
|---|---|
| `leagues` | `{id, sport, label}` each; the tabs are the distinct `sport`s in the order first named. Shipped: `nfl/football`, `nhl/hockey`, `nba/basketball`, `mlb/baseball`, and under soccer `usa.1` `eng.1` `esp.1` `ger.1` `ita.1` `fra.1` `uefa.champions`; any league espn.com shows works the same way |
| `teams` | your teams, `"TOR"` (every league's TOR) or `"nhl:TOR"`; their games sort first, wear a gold bar and fill the star tab. Once a star is tapped on the device the Teams page's list (NVS) replaces this one |
| `refresh` | `liveSeconds`, `idleSeconds` |
| `tz` | POSIX zone for the clock and the start times |

The Wi-Fi network is the one the ticker saved (NVS `ticker`: ssid/pass): set
the board up once through the ticker's portal and both apps use it.

## Logos

`tools/make-team-logos.py` asks the same feed for each league, collects every
team playing this week with its logo URL, and converts them through the
ticker's `make-logos.py` pipeline. Files are `<league>_<ABBR>.565` because
TOR is a different team in the NHL and the NBA. A team without a file gets a
tile with its letters. The big mark is 96px: 112 teams at 32 and 96 is 2.4MB of the 3.4MB data partition (128px overflowed it).

## On the panel

- **List.** Tabs: ALL, a star for your teams, a glyph per sport; rows of 52px: away mark,
  letters and score, home the same, the league in ALL, the state on the
  right (green while live, the broadcaster under it before the game). A drag
  scrolls; a swipe left or right changes the tab; a tap opens the game.
- **Game.** Both marks at 96px, the names, the scores in the 50px digits,
  the state between, the broadcaster, "your team" for a favourite. Left and
  right step through the games; down or the header's mark return.

- **Splash.** GAME DAY in Logisoso, the sports under it, a scoreboard panel;
  it stays until the first league lands.
- **Settings.** The sliders icon by the clock. Teams (every team in this
  week's games by league, a star each), Clock (12/24h), Auto return (the
  game page comes back to the scores after 15s, 60s or never), Sleep (never,
  or dark 23:00-06:00 with a touch lighting it for a minute), Theme (the
  ticker's twelve, a page of tiles), Info, Wi-Fi (read-only: set up in
  Ticker Tape), Shut down (a sheet; also a two-second hold on the header).
  All in NVS `sports`. BOOT wakes it.

`lib/ui/ui.h` holds what this app and the ticker share -- faces, text, the
themes, the header's marks, the sheet, the tile page, the gesture recogniser,
the body reader -- lifted from the ticker on 2026-09-18. The ticker still
carries its own copies; migrate it when it is next open.

Not yet: line scores per period (the header feed has none; the full
scoreboard at `cdn.espn.com/core/<league>/scoreboard?xhr=1` does, at 500KB),
sleep, the crawl.
