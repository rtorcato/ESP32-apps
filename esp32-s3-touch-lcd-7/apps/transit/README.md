# transit — idea (2026-09-19)
A departure board for the stops near the house, like the one at the
station: the next buses and trains, in minutes, live.

## Source

GTFS-realtime trip updates, which most agencies publish (the TTC does,
keyless; some agencies want a free key). Protocol buffers, not JSON: a
small decoder for the two messages the board needs (TripUpdate,
VehiclePosition), or an agency's JSON wrapper where one exists. The
static GTFS (stop names, routes) is too big for the board: a Mac-side
script picks the stops and routes of interest and writes them to
config.json.

## On the panel

- **Board.** A row per upcoming departure: the route number in a coloured
  pill (the agency's colour), the destination, the platform or direction,
  minutes to go in the tall digits, "due" under two minutes, delayed ones
  in amber.
- **Stops.** A tab per stop in the config.
- **Alerts.** Service alerts for those routes, as a banner.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
