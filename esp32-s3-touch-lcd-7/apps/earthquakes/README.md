# earthquakes — idea (2026-09-19)
The last day of quakes on a map, and the last ten as rows. Quiet most
days, which is the point.

## Source

USGS's live GeoJSON feeds (all quakes of the past day / week, or M2.5+),
keyless, refreshed every five minutes.

## On the panel

- **Map.** A world map (a land mask) with a circle per quake, sized by
  magnitude, coloured by age; the biggest of the day named.
- **List.** Rows: magnitude in the tall digits, place, depth, how long
  ago; a banner and a chime-less banner for anything over 6.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
