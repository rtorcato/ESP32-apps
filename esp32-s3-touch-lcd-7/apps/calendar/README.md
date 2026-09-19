# calendar — idea (2026-09-19)
Today and the next few days from the calendars you already keep, and the
one thing that is next, counting down.

## Sources

- Any iCal URL, keyless: Apple's shared calendars, Google's secret iCal
  address, Fastmail, Nextcloud. A small ICS parser on the board (VEVENT,
  DTSTART, SUMMARY, the common recurrence rules).
- Google Calendar or Todoist proper need an OAuth token: not for the
  board.

## On the panel

- **Today.** The date large, then the day's events as rows with times,
  the current one lit, the next one with a countdown.
- **Week.** Seven columns, a chip per event, tap a day for its list.
- **Settings.** Calendar URLs (typed once on the keyboard, or in
  config.local.json), the shared rows.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
