# Elecrow 2.1" Rotary Display — 480×480 round

**Status: owned** (black bezel, model DHE03921D, bought 2026-09 on amazon.ca
for CAD $59.39 on a 10% deal, list $65.99). The pinout below is verified against
the factory sketch and on this unit by [`apps/hello`](apps/hello/).

**Buy:** [amazon.ca (search)](https://www.amazon.ca/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) ·
[amazon.com (search)](https://www.amazon.com/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) ·
[elecrow](https://www.elecrow.com/crowpanel-2-1inch-hmi-esp32-rotary-display-480-480-ips-round-touch-knob-screen.html)
&nbsp;&nbsp;**Docs:** [wiki](https://www.elecrow.com/wiki/CrowPanel_2.1inch-HMI_ESP32_Rotary_Display_480_IPS_Round_Touch_Knob_Screen.html) ·
[factory code](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen)

| | |
|---|---|
| SoC | ESP32-S3 N16R8 — dual-core LX7 @240MHz, 16MB quad flash, 8MB octal PSRAM |
| Display | 2.1" round IPS, 480×480, ST7701 over 16-bit RGB parallel (init over 3-wire SPI) |
| Touch | CST816 capacitive, I2C 0x15 |
| Expander | PCF8574 at I2C 0x21 — LCD power/reset, touch reset/INT and the knob switch are behind it |
| USB | Native USB-Serial/JTAG (`/dev/cu.usbmodem*`), no bridge chip |
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

## Pinout (verified)

Everything lives in [`lib/board/board.h`](lib/board/board.h). Beware the
`Simple example/Encoder_code.ino` in Elecrow's repo — its 45/42/41 pins are a
different board's knob.

| Signal | GPIO |
|---|---|
| RGB DE / VSYNC / HSYNC / PCLK | 40 / 7 / 15 / 41 |
| R0–R4 | 46, 3, 8, 18, 17 |
| G0–G5 | 14, 13, 12, 11, 10, 9 |
| B0–B4 | 5, 45, 48, 47, 21 |
| Panel init SPI CS / SCK / SDA | 16 / 2 / 1 |
| Backlight PWM | 6 |
| I2C SDA / SCL | 38 / 39 |
| Encoder A / B | 42 / 4 |
| BOOT | 0 |

| PCF8574 bit | Function |
|---|---|
| P0 | touch reset |
| P2 | touch interrupt (driven high at boot) |
| P3 | LCD power |
| P4 | LCD reset |
| P5 | knob switch, active low |

RGB timing that works: hsync/vsync polarity 1, porches 10/4/20 front/pulse/back
on both axes, pixel clock 16MHz **inverted**. The Arduino_GFX `type5` example
uses a non-inverted 12MHz clock; ESPHome's config for this board uses inverted
18MHz. Both knobs are `#define`s at the top of `board.h`.

## Building

PlatformIO, same pioarduino platform as the C6 project. The board manifest is
`esp32-s3-devkitc1-n16r8` — the plain devkitc-1 one assumes no PSRAM, and the
480×480 framebuffer has nowhere to live without it. Config loader, data-dir hook
and `push-config` are the C6 project's, reached by symlink / relative path.

```sh
pio run -e hello -t upload -t monitor
./push-config hello
```

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 480×480,
clipped to the circle so the unusable corners are visible.

| App | Idea |
|---|---|
| [hello](apps/hello/) | **Built.** Smoke test: expander, panel timing, backlight, encoder, switch, touch |
| [thermostat](apps/thermostat/) | Nest-style dial — the natural fit |
| [media-knob](apps/media-knob/) | BLE HID volume/transport control for the desktop |
| [light-dimmer](apps/light-dimmer/) | Turn-to-dim, press-to-toggle smart light control |
