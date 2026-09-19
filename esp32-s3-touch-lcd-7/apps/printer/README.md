# printer — idea (2026-09-19)
A 7" panel beside the 3D printer: progress, temperatures, time left, and
the camera frame.

## Sources

- OctoPrint: an API key, `/api/job` and `/api/printer`, the webcam's
  snapshot URL as a JPEG.
- Bambu: the printer's local MQTT with its access code (a small MQTT
  client), the camera over its own stream (harder; the frame only, if at
  all).
- Klipper / Moonraker: `/printer/objects/query`, keyless on the LAN
  usually; the camera through Crowsnest's snapshot.

## On the panel

- **Job.** The file name, progress as a ring and in the tall digits, time
  left, the layer, the camera frame at 640x360 refreshed every few seconds
  the way Network's cameras are.
- **Temperatures.** Nozzle and bed, target and actual, as two small charts.
- **Idle.** When nothing prints: the last job, the printer's state, and the
  camera as a screensaver.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
