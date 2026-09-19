# media — idea (2026-09-19)
Now playing, with the artwork large: what the house is watching or
listening to, and the controls.

## Sources

- Plex: a local token, `/status/sessions` for what is playing, the poster
  as a JPEG the board decodes.
- Jellyfin: an API key, `/Sessions`, the primary image.
- Sonos: the local UPnP API, keyless on the LAN, for what is playing and
  transport control.
- Spotify needs OAuth with a refresh token: not for the board.

## On the panel

- **Now playing.** The poster or album art at 300px, the title, the
  artist or show, the progress bar, who is watching on what.
- **Controls.** Play, pause, next as sheets' pills, volume for Sonos.
- **Recently added.** A row of posters, for Plex and Jellyfin.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
