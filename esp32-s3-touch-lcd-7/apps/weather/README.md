# weather — idea (2026-09-19)
A weather board: now, the next hours, the week. Big and readable from
across the room, the way the ticker is.

## Source

[Open-Meteo](https://open-meteo.com), keyless, no account: current
conditions, hourly and 7-day forecast, air quality, pollen, UV. The
rotary's world-clock app already reads it. One request every ten minutes
returns everything; a second for air quality.

## On the panel

- **Now.** The temperature in the tall digits, feels-like, the condition
  as a drawn glyph (sun, cloud, rain, snow, storm -- primitives, or a
  Twemoji set through the logo tool the way Game Day's sports are), wind,
  humidity, UV, air quality, sunrise and sunset.
- **Hours.** A strip of the next 24: a glyph, the temperature, the chance
  of rain as a bar under each.
- **Week.** Seven rows: the day, a glyph, high and low, a bar spanning the
  week's range, rain.
- **Settings.** Place (typed, geocoded by Open-Meteo's own search),
  units, plus the shared rows.

No radar: there is no keyless radar tile source worth relying on.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
