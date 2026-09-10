# Waveshare ESP32-S3-Touch-LCD-7 — 7" 800×480

**Status: wishlist.** No verified pinout here — the panel uses ~20 GPIOs plus an
I/O expander, so copy Waveshare's own board config rather than typing pins by
hand.

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
| [wall-dashboard](apps/wall-dashboard/) | Calendar, weather, transit — the flagship use |
| [ha-panel](apps/ha-panel/) | Full-size Home Assistant control surface |
| [obd2-gauge](apps/obd2-gauge/) | Live car telemetry over the CAN header |
| [modbus-readout](apps/modbus-readout/) | Poll RS485/Modbus sensors and chart them |
