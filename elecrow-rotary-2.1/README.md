# Elecrow 2.1" Rotary Display — 480×480 round

**Status: wishlist.** Specs below are from the product listing; confirm the
pinout and panel driver on Elecrow's wiki before writing any board config.

**Buy:** [amazon.ca (search)](https://www.amazon.ca/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) ·
[amazon.com (search)](https://www.amazon.com/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) ·
[elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html)
&nbsp;&nbsp;**Docs:** [elecrow wiki](https://www.elecrow.com/pub/wiki/)

> No Amazon ASIN surfaced for this one — verify Canadian availability before
> planning around it.

| | |
|---|---|
| SoC | ESP32-S3 — dual-core Xtensa LX7 @240MHz |
| Display | 2.1" round IPS, 480×480, RGB parallel |
| Touch | Capacitive |
| Input | **Rotary encoder + press knob** — the reason to buy it |
| Toolchain | Arduino, ESP-IDF, PlatformIO, MicroPython, ESPHome, LVGL/SquareLine |

## Why buy it

The knob. Touch is bad at continuous adjustment — setting a temperature or a
volume by dragging a slider with a fingertip is fiddly and imprecise. A physical
detented encoder is the right input for any one-dimensional value, and it works
without looking at the screen.

480×480 round also looks like an *appliance* rather than a dev board, which
matters for anything that lives in a living room.

## What to expect

- **Round means round.** Corners don't exist. Layouts must be radial — arcs,
  dials, centred readouts. A rectangular list design will look broken, and LVGL
  widgets need deliberate placement inside the inscribed circle.
- **RGB parallel panel + PSRAM**, same class of setup as the 7" board — 480×480
  is ~460KB of framebuffer. Not an SPI board, so C6 code does not port.
- Encoder handling needs interrupt-driven quadrature decoding with debouncing.
  Polling in `loop()` drops steps when the UI is busy, which feels broken.
- Decide early whether touch is primary with the knob as a shortcut, or the knob
  is primary with touch as a fallback. Mixing both loosely produces a confusing
  interface.

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 480×480,
clipped to the circle so the unusable corners are visible.

| App | Idea |
|---|---|
| [thermostat](apps/thermostat/) | Nest-style dial — the natural fit |
| [media-knob](apps/media-knob/) | BLE HID volume/transport control for the desktop |
| [light-dimmer](apps/light-dimmer/) | Turn-to-dim, press-to-toggle smart light control |
