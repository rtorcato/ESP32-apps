# pihole — idea (2026-09-19)
What the DNS blocker is doing: queries and blocks today, the top blocked
domains, the top clients, and a pause button.

## Sources

- Pi-hole v6: the local API with a password-derived session, or an app
  password; `/api/stats/summary`, `/api/stats/top_domains`,
  `/api/dns/blocking` to pause.
- AdGuard Home: `/control/stats`, basic auth.

This could equally be a fourth tab in the Network app, since it sits on
the same network and the same settings.

## On the panel

- **Today.** Queries and blocked in the tall digits, the block rate as a
  ring, the last hour as a bar chart of blocked over allowed.
- **Top.** Blocked domains and busiest clients, two lists.
- **Pause.** A sheet: 5 minutes, 30 minutes, until morning.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
