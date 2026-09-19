# solar — idea (2026-09-19)
The house's power, live: what the panels make, what the house uses, what
goes to or from the grid and the battery, and today's totals.

## Sources, one key each

- Enphase Envoy: local API on the gateway, a token from the Enphase site.
- Tesla Powerwall: local API on the gateway, a password.
- Emporia Vue, Sense, Shelly EM: each has a local or cloud endpoint with a
  token.
- Home Assistant, if it already has the numbers, is the simplest source:
  one long-lived token and the entity names.

## On the panel

- **Flow.** Four nodes -- sun, house, battery, grid -- with the power on
  each link in the tall digits and the direction as a moving dash.
- **Today.** Made, used, exported, imported, the battery's level, as bars
  against yesterday.
- **Chart.** The day's curves, production over consumption, the way the
  ticker draws a chart.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
