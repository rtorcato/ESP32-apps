# flights — idea (2026-09-19)
Every aircraft overhead, on a map of the sky above the house, crawling.
The thing people stop and look at.

## Source

[OpenSky Network](https://opensky-network.org), keyless for the state
vectors in a bounding box: callsign, position, altitude, heading, speed,
vertical rate. One request every ten seconds (anonymous limits allow
about that). Routes and airline names need a second source (adsbdb,
keyless, per callsign, cached).

## On the panel

- **Sky.** A 40km box around home drawn as a plan view: home at the
  centre, range rings, each aircraft as a small triangle pointing along
  its heading with its callsign and altitude; a short tail of its last
  positions. Higher altitude, dimmer.
- **List.** The same aircraft as rows: callsign, from -> to when known,
  altitude, speed, distance and bearing from home.
- **Aircraft page.** Tap one: the callsign large, the route, the numbers,
  a bigger tail.
- **Settings.** Home position (typed once), the box size, the shared rows.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
