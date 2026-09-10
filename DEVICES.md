# Devices

Purchase links and cross-device comparison. Full specs, verified pinouts and
per-board gotchas live in each device's own README.

## Buy

| Device | Buy | Vendor docs |
|---|---|---|
| **Waveshare ESP32-C6-LCD-1.47** — owned | [CA](https://www.amazon.ca/dp/B0DHTMYTCY) · [US](https://www.amazon.com/dp/B0DHTMYTCY) · [waveshare](https://www.waveshare.com/esp32-c6-lcd-1.47.htm) | [wiki](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47) |
| **DIYmalls ESP32-2432S028R** — 2.8" CYD | [CA](https://www.amazon.ca/dp/B0CG2WQGP9) · [US](https://www.amazon.com/dp/B0CG2WQGP9) | [community repo](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) |
| **Waveshare ESP32-S3-Touch-LCD-7** — 7" | [CA search](https://www.amazon.ca/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) · [US search](https://www.amazon.com/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) · [waveshare](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm) | [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) |
| **Elecrow 2.1" Rotary** — 480×480 round | [CA search](https://www.amazon.ca/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) · [US search](https://www.amazon.com/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) · [elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html) | [elecrow wiki](https://www.elecrow.com/pub/wiki/) |
| **Elecrow 1.28" Rotary** — 240×240 round | [CA search](https://www.amazon.ca/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) · [US search](https://www.amazon.com/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) · [elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html) | [elecrow wiki](https://www.elecrow.com/pub/wiki/) |

**Link accuracy:** only the CYD ASIN (`B0CG2WQGP9`) was confirmed live on both
amazon.ca and amazon.com. The C6 ASIN (`B0DHTMYTCY`) was confirmed on amazon.com
and assumed to carry over to .ca. The Elecrow and Waveshare-7" rows are *search*
links because no Amazon ASIN surfaced for them — they may only be available direct
from the vendor or via AliExpress. Verify before ordering.

CYD variants if the single-board listing is out of stock: 2-pack
[CA](https://www.amazon.ca/dp/B0DNM4SKSJ) · [US](https://www.amazon.com/dp/B0DNM4SKSJ),
or with acrylic case [CA](https://www.amazon.ca/dp/B0D8W9DSYZ) · [US](https://www.amazon.com/dp/B0D8W9DSYZ).

## Compare

| | [C6-LCD-1.47](esp32-c6-lcd-1.47/) | [CYD 2.8"](esp32-2432s028r-cyd/) | [S3-LCD-7](esp32-s3-touch-lcd-7/) | [Rotary 2.1"](elecrow-rotary-2.1/) | [Rotary 1.28"](elecrow-rotary-1.28/) |
|---|---|---|---|---|---|
| SoC | C6, 1× RISC-V | WROOM-32, 2× | S3, 2× LX7 | S3, 2× LX7 | S3, 2× LX7 |
| Resolution | 172×320 | 240×320 | 800×480 | 480×480 round | 240×240 round |
| Panel bus | SPI | SPI | RGB parallel | RGB parallel | SPI |
| PSRAM | none | none | 8MB | yes | yes |
| Touch | **none** | resistive | 5-pt capacitive | capacitive | capacitive |
| Knob | — | — | — | **yes** | **yes** |
| Audio | — | speaker hdr | — | — | — |
| SD | yes (shared bus) | yes (own bus) | yes | — | — |
| Wi-Fi 6 | **yes** | no | no | no | no |
| 802.15.4 | **yes** | no | no | no | no |
| Industrial I/O | — | — | **CAN, RS485** | — | — |

## What each one is actually for

Every board has one capability the others don't, which is the only real reason to
own more than one:

- **C6** — Thread/Zigbee/Matter. The only board that can talk to 802.15.4 sensors
  without a dongle. Also the only one with no touch at all, so it's a *display*,
  not a panel.
- **CYD** — documentation. Best-supported cheap ESP32 display anywhere, plus audio
  out. The board to use when you want to finish something rather than debug a
  driver.
- **S3-LCD-7** — size, and CAN/RS485. Wall dashboards, and the only route to a
  vehicle bus or Modbus devices.
- **Rotary 2.1"** — a knob on a big round face. Thermostats and dimmers, where
  continuous adjustment beats touch.
- **Rotary 1.28"** — a knob, cheaply, on an SPI panel. Code ports from the C6.

## 2U rack display — the honest options

A 2U front panel is a **482 × 89 mm strip** (1U = 44.45 mm, so 2U = 88.9 mm),
which is a lovely shape for a UniFi status wall. One constraint decides
everything though:

> **The wide "bar" LCDs cannot be driven by an ESP32.** The 8.8" 1920×480 and
> 11.9" 320×1480 panels are **MIPI-DSI natively, sold with HDMI bridge boards** —
> SPI is not offered on either family, and 1920×480 at 60 Hz is orders of
> magnitude beyond practical SPI bandwidth. Every "AIDA64 rack monitor" build you
> find is a PC or Raspberry Pi driving a normal HDMI monitor.

So there are three real paths, and they're genuinely different projects:

| Path | Hardware | Verdict |
|---|---|---|
| **Pi + 8.8" bar LCD** | HannStar HSD088IPW1 panel (~232 × 57 mm, fits 2U easily) + HDMI→MIPI board, ~$90–110 | Easiest and best-looking. But it's a Pi/browser dashboard — **leaves this repo entirely.** |
| **2–3 × C6 boards in a 2U panel** | Boards you already understand, rotated to landscape 320×172 | Cheapest, modular, **stays in this repo.** One board per concern: WAN, clients, PoE. |
| **ESP32-P4 + MIPI bar panel** | P4 is the only ESP32 with MIPI-DSI | The "one wide ESP32-driven strip" answer. Newest, thinnest ecosystem — a project, not an afternoon. |

**The 7" board does not fit 2U.** Its active area is ~152 × 91 mm and 2U is
88.9 mm *total*, before any bezel. It needs 3U.

### Recommendation

Given the C6 is what you own and understand: **2U blank panel + three C6 boards.**
Blank and vented 2U panels are cheap on Amazon
([CA](https://www.amazon.ca/s?k=2U+rack+blank+panel) ·
[US](https://www.amazon.com/s?k=2U+rack+blank+panel)), and cutting three
rectangular openings — or 3D-printing a bezel insert that drops into one — is
well inside "make a holder" territory. Each board is a separate PlatformIO env
already, so [unifi-status](esp32-c6-lcd-1.47/apps/unifi-status/),
[wan-watchdog](esp32-c6-lcd-1.47/apps/wan-watchdog/) and
[poe-cycle](esp32-c6-lcd-1.47/apps/poe-cycle/) can each own a panel with no new
code architecture. Three cheap boards also means one dying doesn't blank the
whole display.

Two things to plan for regardless of path: **USB power inside the rack** (a
short 2U-mounted USB hub beats three wall bricks), and **heat** — a rack is warm
and these panels dim and age faster hot, so keep them off the exhaust side and
run the backlight below full.

## Buying order

1. **Rotary 1.28"** — cheapest genuinely new capability (encoder input), and its
   SPI panel reuses C6 display habits. Lowest risk.
2. **CYD 2.8"** — adds touch and audio for very little money, with the best docs
   in the ecosystem to lean on.
3. **Rotary 2.1"** — only if a knob app outgrows the 1.28" face.
4. **S3-LCD-7** — buy last and on purpose. RGB-parallel + CH422G expander +
   PSRAM framebuffer is a real detour, and ~26fps LVGL means it's a dashboard,
   not an interactive toy.

Two boards cover the useful ground: the C6 for radio work and the CYD for touch.
Everything past that is want, not need.
