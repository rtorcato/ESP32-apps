# Elecrow 1.28" Rotary Display — 240×240 round

**Status: wishlist.** Specs from the product listing; confirm the pinout and
panel driver on Elecrow's wiki before writing board config.

**Buy:** [amazon.ca (search)](https://www.amazon.ca/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) ·
[amazon.com (search)](https://www.amazon.com/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) ·
[elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html)
&nbsp;&nbsp;**Docs:** [elecrow wiki](https://www.elecrow.com/pub/wiki/)

> No Amazon ASIN surfaced for this one — verify Canadian availability before
> planning around it.

| | |
|---|---|
| SoC | ESP32-S3 — dual-core Xtensa LX7 @240MHz |
| Display | 1.28" round IPS, 240×240, **SPI** |
| Touch | Capacitive |
| Input | Rotary encoder + press knob |
| Toolchain | Arduino, ESP-IDF, PlatformIO, MicroPython, ESPHome, LVGL |

## Why buy it

**It's the cheapest way to add a knob**, and unlike its 2.1" sibling the panel
is **SPI with a 240×240 framebuffer (~115KB)** — close enough to the C6 board's
code path that display helpers and layout habits carry over. No RGB-parallel
detour, no mandatory PSRAM framebuffer.

That makes it the lowest-risk addition to the collection: new input type, old
familiar display plumbing.

## What to expect

- 240×240 round is **small**. Roughly one number and one arc — that's the honest
  budget. Anything with two data points fighting for the centre won't read.
  Design for one value per screen and use the knob to page between them.
- Round layout constraints are the same as the 2.1": radial only, no corners.
- Same encoder caveat: interrupt-driven quadrature decoding with debounce, not
  polling in `loop()`.
- Dual-core ESP32-S3 is more chip than these apps need, which is fine — it means
  network work never blocks the UI.

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 240×240,
clipped to the circle — which makes the "one value per screen" budget obvious.

| App | Idea |
|---|---|
| [desk-timer](apps/desk-timer/) | Turn to set minutes, press to start |
| [scroll-knob](apps/scroll-knob/) | BLE HID scroll/zoom wheel for the desktop |
| [single-gauge](apps/single-gauge/) | One sensor, one dial, done well |
