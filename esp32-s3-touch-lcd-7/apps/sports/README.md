# sports — **built** (first flash 2026-09-18)

A scores board: every game today across the leagues you follow, live ones
first with the clock, then the day's fixtures by start time, then finals,
your teams first in each. A tab per league, a page per game with the two
logos large and the score in the tall digits.

```sh
python3 tools/make-team-logos.py --size 32 --out data/logo/32     # the teams playing this week
python3 tools/make-team-logos.py --size 128 --out data/logo/128
pio run -e sports -t upload
./push-config sports                                              # config + logos
```

## Where the scores come from

ESPN's own site scoreboard, `site.web.api.espn.com/apis/v2/scoreboard/header`,
keyless. (The public `site.api.espn.com` host refuses every request with an
Akamai denial; this one, the strip at the top of espn.com, answers.) One
request per league, 10KB to 260KB each, read whole into a 640KB PSRAM buffer
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
| `leagues` | `{id, sport, label}` each, in tab order: `nfl/football`, `nhl/hockey`, `nba/basketball`, `mlb/baseball`, `usa.1/soccer` (MLS); any league espn.com shows works the same way |
| `teams` | abbreviations of your teams; their games sort first and wear a gold bar |
| `refresh` | `liveSeconds`, `idleSeconds` |
| `tz` | POSIX zone for the clock and the start times |

The Wi-Fi network is the one the ticker saved (NVS `ticker`: ssid/pass): set
the board up once through the ticker's portal and both apps use it.

## Logos

`tools/make-team-logos.py` asks the same feed for each league, collects every
team playing this week with its logo URL, and converts them through the
ticker's `make-logos.py` pipeline. Files are `<league>_<ABBR>.565` because
TOR is a different team in the NHL and the NBA. A team without a file gets a
tile with its letters. Sixty-six teams at both sizes is 2.3MB of the 3.4MB
data partition.

## On the panel

- **List.** Text tabs ALL and one per league; rows of 52px: away mark,
  letters and score, home the same, the league in ALL, the state on the
  right (green while live, the broadcaster under it before the game). A drag
  scrolls; a swipe left or right changes the tab; a tap opens the game.
- **Game.** Both marks at 128px, the names, the scores in the 50px digits,
  the state between, the broadcaster, "your team" for a favourite. Left and
  right step through the games; down or the header's mark return.

Not yet: line scores per period (the header feed has none; the full
scoreboard at `cdn.espn.com/core/<league>/scoreboard?xhr=1` does, at 500KB),
settings, themes, sleep, the crawl. The ticker has all of those and this app
copies its helpers rather than sharing them -- hoist into `lib/ui.h` when the
third app arrives.
