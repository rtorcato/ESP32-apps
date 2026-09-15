# world-clock — **built**

<img src="preview.svg" alt="world-clock preview" width="240">

One city per screen, on a globe. Turn the knob and the globe turns so the
selected city faces you, with a yellow dot on it and a cyan one on home. The
dark side is the actual night side right now: the terminator is computed from
the date and time. The ring of dots around the edge is every zone at its UTC
offset relative to the one on screen, 15° per hour, so it turns with the globe.
Press to jump home.

The digits go cool blue when it is night *in that zone*, so 03:41 in Tokyo reads
as the middle of the night even when it is a Toronto afternoon.

Under the date: the city's weather right now, from open-meteo — `22C cloudy`.
It is fetched once a zone has sat selected for a moment, so spinning past
cities costs nothing, and kept for 15 minutes per zone.

- **Turn** — next / previous zone, wrapping. Also snaps the globe back to the
  city if you had dragged it.
- **Drag** — spins the globe freely under your finger, half a turn across the
  face. The city dots follow; the ring and the clock stay put.
- **Press** — sleep: backlight off, nothing drawn. Press, turn or touch wakes
  it with a full redraw.

## The globe

Rendered on the device, not stored as pictures.
[`tools/make-landmask.py`](tools/make-landmask.py) bakes Natural Earth's 1:110m
land polygons into a 720×360 one-bit mask, 32KB in flash, with a scanline fill
in plain Python (no PIL needed). At boot [`src/globe.h`](src/globe.h) inverse-
projects every pixel of the disc once, for the configured tilt, into two lookup
tables in PSRAM: which mask row and which mask column that pixel sees before
the globe is turned. Turning is then an offset on the column, and a render is
two table reads, a mask bit and one compare for day/night per pixel.

| | |
|---|---|
| boot table build | 1.3s, during the Wi-Fi join |
| full redraw on a click | ~130ms: render, blit, ring, text, dots |
| PSRAM used | 460KB background + 920KB tables |

The rendered globe lives in a background buffer and every text field erases by
copying its rectangle back from it, which is what lets white digits sit on the
picture without punching black holes in it.

Sun position uses the standard declination approximation and ignores the
equation of time: at most ~4° of longitude, a few pixels of terminator. The view
tilt is `globe.tilt` in config, 30°N by default so the northern cities sit
comfortably above the digits; `lat`/`lon` per zone place the dots.

A zone with no `lat`/`lon` still works: the globe turns to the meridian its
UTC offset implies, and it just gets no dot and no weather.

## How it keeps time

NTP once over Wi-Fi, in UTC, and the RTC carries it from there. Each zone is
rendered by switching the C library's `TZ` to that zone's POSIX rule string and
asking `localtime_r`, so daylight-saving transitions are the rule's problem and
happen on the right day without a code change. The same format desk-clock's
`timezone` uses. A few to copy:

| City | `tz` |
|---|---|
| Toronto / New York | `EST5EDT,M3.2.0/2,M11.1.0/2` |
| Vancouver / LA | `PST8PDT,M3.2.0/2,M11.1.0/2` |
| London | `GMT0BST,M3.5.0/1,M10.5.0` |
| Paris / Berlin | `CET-1CEST,M3.5.0,M10.5.0/3` |
| Dubai | `<+04>-4` |
| Mumbai | `IST-5:30` |
| Tokyo | `JST-9` |
| Sydney | `AEST-10AEDT,M10.1.0,M4.1.0/3` |

Until the first sync the screen says `NO TIME` and whether it is still joining
Wi-Fi or waiting for NTP. If Wi-Fi drops later the clock keeps running on the
RTC and re-syncs when the link returns.

`selfCheck()` renders two fixed instants (one each side of DST) in every default
zone and asserts the hour, and checks the offset maths, the ring angle and the
centring helper. It runs at boot and halts the app loudly if any of it is wrong,
which is how a bad TZ string shows up as a message rather than a wrong clock.

```sh
pio run -e world-clock -t upload -t monitor
./push-config world-clock
```

## Settings

[`data/config.json`](data/config.json): the zone list (first is home, up to
16, each with `name`, `tz`, `lat`, `lon`), 12/24 hour, the globe tilt, day/night
brightness, and the night window judged by home's clock. Every key is optional; the defaults are the eight cities above with
Toronto home. Wi-Fi credentials are compiled in from `secrets.h` — see
[SECURITY.md](../../../SECURITY.md).
