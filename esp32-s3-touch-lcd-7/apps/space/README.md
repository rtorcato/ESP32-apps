# space — idea (2026-09-19)
The sky beyond the weather: where the ISS is, when it passes over, the
picture of the day, what launches this week.

## Sources, all keyless

- [wheretheiss.at](https://wheretheiss.at): the ISS position every few
  seconds; passes over a location.
- NASA APOD: the astronomy picture of the day, a JPEG the board decodes
  (a demo key is enough for the rate this needs).
- [Launch Library 2](https://thespacedevs.com): upcoming launches, with
  the rocket, the pad, the window.
- Open-Meteo for cloud cover tonight, so a pass is worth going outside for.

## On the panel

- **ISS.** A world map (a baked land mask, the way the rotary's world clock
  has one) with the station's track and its next pass over home: when,
  how high, how bright.
- **Picture.** Today's APOD at 800x480 with its title; the board's own
  screensaver.
- **Launches.** Rows: when (local), the rocket, the mission, the pad; a
  countdown for the next.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
