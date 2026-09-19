# Waveshare ESP32-S3-Touch-LCD-7 — 7" 800×480

**Status: board layer and a first app built, 2026-09-16.** `lib/board/board.h`
carries the pinout from Espressif's ESP32_Display_Panel board file and the
ESPHome package for this device (they agree on every GPIO); `apps/hello`
lit the panel and found the touch controller on the first try, and
[`apps/ticker`](apps/ticker/) is the CYD ticker ported. Nothing has been
checked by eye yet. Three things learned the hard way: the USB-C is a CH343
UART bridge, not the chip's USB (no CDC flags, and never toggle DTR/RTS on
it -- that is download mode); the CH422G's EXIO5 must stay low or the
connector switches to the CAN side; and the module is the same N16R8 as
the rotary, so its board manifest carries over.

**Buy:** [amazon.ca (search)](https://www.amazon.ca/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) ·
[amazon.com (search)](https://www.amazon.com/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) ·
[waveshare](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm)
&nbsp;&nbsp;**Docs:** [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) ·
[docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7)

> No Amazon ASIN surfaced for this one — the search links may not resolve to a
> Canadian listing. Check availability before planning around it; it may have to
> come direct from Waveshare.

| | |
|---|---|
| SoC | ESP32-S3 — dual-core Xtensa LX7 @240MHz |
| Memory | 512KB SRAM + 384KB ROM, 16MB flash, **8MB PSRAM** |
| Radio | Wi-Fi 4 (b/g/n), BLE 5 |
| Display | 7" 800×480, 65K colour, **RGB parallel** (not SPI) |
| Touch | 5-point capacitive, GT911 over I2C, with interrupt |
| Extras | CAN, RS485, I2C headers, microSD, full-speed USB |

## Why buy it

It's the showpiece. 800×480 with real capacitive multi-touch is a different
class of interface from everything else here — this is the board for a wall
dashboard you actually look at from across the room.

The **CAN and RS485 headers** are the sleeper feature: no other board here can
talk to a vehicle bus or industrial Modbus devices without add-on hardware.

## What you're signing up for

This board shares almost no code with the SPI-panel boards. Budget real time.

- **RGB parallel, not SPI.** You need `Arduino_ESP32RGBPanel` (or ESP-IDF
  `esp_lcd_rgb_panel`), not `Arduino_ESP32SPI`. Different driver, different
  init, different timing parameters to get wrong.
- **PSRAM is mandatory.** An 800×480×16bpp framebuffer is ~750KB — it cannot
  live in SRAM. PSRAM bandwidth then becomes the bottleneck, which is why
  Waveshare measures only **~26fps** on the LVGL benchmark (41 interface fps at
  21MHz PCLK). Treat it as a dashboard, not an animation target.
- **A CH422G I/O expander** handles panel reset and backlight because the RGB
  interface consumes nearly every free GPIO. Anything you want to bit-bang goes
  through I2C, and free pins are scarce — check the pinout *before* planning
  extra sensors.
- LVGL is effectively required at this resolution; hand-drawing 800×480 with
  primitives is not practical.

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 800×480.

| App | Idea |
|---|---|
| [launcher](apps/launcher/) | The base app: owns the board, the settings and the picker; one app loaded at a time |
| [social](apps/social/) | Built as a demo, parked: a ticker of your own numbers |
| [unifi](apps/unifi/) | **Built.** Network: the UniFi console's devices, clients and cameras |
| [sports](apps/sports/) | **Built.** Game Day: scores across eleven leagues with logos |
| [ticker](apps/ticker/) | **Built.** Ticker Tape: stocks, indices, FX, coins, news, charts |
| [wall-dashboard](apps/wall-dashboard/) | Calendar, weather, transit — the flagship use |
| [ha-panel](apps/ha-panel/) | Full-size Home Assistant control surface |
| [obd2-gauge](apps/obd2-gauge/) | Live car telemetry over the CAN header |
| [modbus-readout](apps/modbus-readout/) | Poll RS485/Modbus sensors and chart them |
| [weather](apps/weather/) | Now, the hours, the week from Open-Meteo, keyless |
| [flights](apps/flights/) | Every aircraft overhead on a map of the sky, from OpenSky, keyless |
| [transit](apps/transit/) | A departure board for the stops near the house, GTFS-realtime |
| [space](apps/space/) | The ISS, its next pass, the picture of the day, this week's launches |
| [earthquakes](apps/earthquakes/) | The last day of quakes on a map, from USGS, keyless |
| [calendar](apps/calendar/) | Today and the week from any iCal URL, the next thing counting down |
| [solar](apps/solar/) | The house's power flow and today's totals, one key |
| [media](apps/media/) | Now playing with the artwork large: Plex, Jellyfin, Sonos |
| [pihole](apps/pihole/) | Queries and blocks today, the top domains, a pause button |
| [printer](apps/printer/) | The 3D printer's job, temperatures and camera frame |
| [ci-status](apps/ci-status/) | Green and red tiles for workflow runs and uptime monitors |
