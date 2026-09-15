# Devices

Purchase links and cross-device comparison. Full specs, verified pinouts and
per-board gotchas live in each device's own README.

## Buy

| Device | Buy | Vendor docs |
|---|---|---|
| **Waveshare ESP32-C6-LCD-1.47** — owned | [CA](https://www.amazon.ca/dp/B0DHTMYTCY) · [US](https://www.amazon.com/dp/B0DHTMYTCY) · [waveshare](https://www.waveshare.com/esp32-c6-lcd-1.47.htm) | [wiki](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47) |
| **DIYmalls ESP32-2432S028R** — 2.8" CYD, owned | [CA](https://www.amazon.ca/dp/B0CG2WQGP9) · [US](https://www.amazon.com/dp/B0CG2WQGP9) | [community repo](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) |
| **Waveshare ESP32-S3-Touch-LCD-7** — 7" | [CA search](https://www.amazon.ca/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) · [US search](https://www.amazon.com/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD) · [waveshare](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm) | [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) |
| **Elecrow 2.1" Rotary** — 480×480 round, owned (DHE03921D, black) | [CA search](https://www.amazon.ca/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) · [US search](https://www.amazon.com/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480) · [elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html) | [elecrow wiki](https://www.elecrow.com/pub/wiki/) |
| **Elecrow 1.28" Rotary** — 240×240 round | [CA search](https://www.amazon.ca/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) · [US search](https://www.amazon.com/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240) · [elecrow](https://www.elecrow.com/display/esp-hmi-display/round-rotary-display.html) | [elecrow wiki](https://www.elecrow.com/pub/wiki/) |

**Link accuracy:** only the CYD ASIN (`B0CG2WQGP9`) was confirmed live on both
amazon.ca and amazon.com. The C6 ASIN (`B0DHTMYTCY`) was confirmed on amazon.com
and assumed to carry over to .ca. The Elecrow 2.1" is stocked on amazon.ca by
the ELECROW store (CAD $65.99 list, $59.39 on a 2026-09 deal; the 1.28" sits
beside it at $59.99 / $53.99) — the row stays a search link because the ASIN was
not captured. The 1.28" and Waveshare-7" rows are *search* links for the same
reason. Verify before ordering.

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

## What none of them do

Read the table above again and the five boards are, underneath, **the same kind
of thing**: a mains-powered, always-on, backlit panel that sits indoors. The
differences are real but they are all differences *within* that category. The
gaps below are whole categories, and two of them already constrain apps that are
written down in this repo.

| Gap | Why it matters here | Candidate |
|---|---|---|
| **Reflective display** | All five wash out in sun | Waveshare **ESP32-S3 4.2" RLCD** |
| **Battery + charging** | All five are USB-tethered | Waveshare AMOLED 1.75"/1.8"/2.06" (AXP2101 PMIC) |
| **E-paper** | No access to the low-power regime at all | Inkplate · LilyGO T5 E-Paper S3 Pro · M5Paper |
| **Camera** | [protect-doorbell](esp32-c6-lcd-1.47/apps/protect-doorbell/) has to borrow UniFi's eyes | ESP32-S3-EYE · XIAO ESP32S3 Sense |
| **LoRa** | Nothing reaches past Wi-Fi | Comes free on the T5 E-Paper S3 Pro |
| **LED matrix** | This file already calls it the only real 1U rack option | HUB75 + ESP32 |
| **Headless sensor node** | Every board here is a *display*; nothing feeds them | XIAO ESP32-C6, ~$5 |

### Reflective beats bright, and it is not AMOLED

The instinct for "readable outdoors" is a brighter panel, and it is wrong — or
rather it is the expensive half of right. **AMOLED and sunlight readability pull
in opposite directions**: winning by brightness burns battery and runs hot
outdoors, which is precisely backwards for a portable device. The Hammerhead
Karoo proves brute force *can* work, and it is plugged into a 15-hour battery to
do it.

**RLCD is the other answer.** A mirror layer behind the pixels reflects ambient
light, so the brighter the environment the higher the contrast — the modern
relative of the transflective MIP panels Garmin and Wahoo have always used. And
unlike e-paper it keeps fast refresh with no ghosting, so live numbers and a
moving map still work.

That makes the RLCD board the **highest-value single addition to this
collection**, because it does not merely enable a new app — it answers the
open go/no-go question in
[bike-buddy](esp32-2432s028r-cyd/apps/bike-buddy/), which is currently specified
against a panel that may well be unreadable in the only conditions it will ever
be used in.

### E-paper is the missing regime, not the missing board

Everything here redraws continuously and dies the moment USB does. E-paper
inverts that: **18–25 µA between refreshes**, so a small cell runs for weeks or
months, and the image survives power loss entirely. That enables a class of app
none of the five can attempt — a wall calendar, a room sign, a dashboard that
updates hourly and is never plugged in.

| Board | Pick it for |
|---|---|
| **Inkplate** | Best docs and the least yak-shaving — charger, RTC, microSD and Qwiic already on board, Adafruit GFX-compatible, panels recycled from decommissioned e-readers |
| **LilyGO T5 E-Paper S3 Pro** | Most per dollar — 4.7" touch, 1500mAh, **SX1262 LoRa**, BQ25896 + BQ27220 fuel gauge, MagSafe |
| **M5Paper** | Wanting a finished, enclosed device rather than a bare board |

**Check the revision before ordering.** The original TTGO T5-4.7 is EOL, and
there are community reports of blank screens under ESPHome's `t547` platform
after a chip change — the same "verify before ordering" rule as the Elecrow rows
above, for the same reason.

### A fuel gauge deletes a documented constraint

Worth noting because it is concrete rather than theoretical: bike-buddy's README
states flatly that the device **cannot** show a battery indicator, because a dumb
USB power bank exposes no state of charge and you must not design a gauge you
cannot feed. Any of the AXP2101 or BQ27220 boards above report charge state and
battery voltage directly — so that constraint is a property of the *power
source*, not of the app, and the right board removes it.

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

### Full-width 19" rack, specifically

The GeeekPi 6.91" 1424×280 1U monitor
([CA](https://www.amazon.ca/s?k=GeeekPi+6.91+inch+1424x280+1U+rack+mount+monitor)) is
for a **10" mini rack** (254 mm wide) — its LCD is 176 mm, which is why it fits
1U. Scaling that to 19" needs ~440 mm of width inside 1U's **43.66 mm** usable
height (h = 44.45n − 0.79 mm), a ~10:1 aspect ratio nobody makes.

| Panel | Active area | Verdict for a 19" rack |
|---|---|---|
| **VSDISPLAY 19" 1920×360** | ~474 × 89 mm | **Fits 19" 2U almost exactly.** ~$470, USB video input, VESA holes, no bezel — you print the faceplate |
| 14.1" 1920×550 (BOE NV140DQM) | 344 × 99 mm, outline 350 × 119 mm | Needs 3U for the outline; ~65 mm filler each side |
| 12.6" 1920×515 (BOE NV126B5M) | 309 × 83 mm, outline 316 × 94 mm | Active fits 2U, outline doesn't — custom overlapping bezel or 3U |
| **1U LED matrix** (Etsy, "19in 1RU") | — | **The only real 1U option.** Matrices fit 1U where LCDs can't, and some ship with Prometheus/MQTT input |

All the LCD options are **eDP / HDMI / USB — not SPI**, so none are ESP32-driven;
they need a Pi or PC. The full-width dream and the ESP32 are separate projects.

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

1. ~~**Rotary 2.1"**~~ — bought first after all (2026-09): the knob apps wanted
   the big face, and on amazon.ca it was only $5 more than the 1.28".
2. **CYD 2.8"** — adds touch and audio for very little money, with the best docs
   in the ecosystem to lean on.
3. **Rotary 1.28"** — only if a knob app wants a second, cheaper dial.
4. **S3-LCD-7** — buy last and on purpose. RGB-parallel + CH422G expander +
   PSRAM framebuffer is a real detour, and ~26fps LVGL means it's a dashboard,
   not an interactive toy.

Two boards cover the useful ground: the C6 for radio work and the CYD for touch.
Everything past that is want, not need.

**That ordering is about the boards in the table, though, and the table is all
one category.** Measured against [what none of them do](#what-none-of-them-do),
two purchases buy more than any of the four above:

1. **The RLCD board** — it *answers* a question already written down rather than
   posing a new one, which is the only reason to buy hardware before you need it.
2. **An e-paper board** — the one wholly absent design regime, and the T5 S3 Pro
   closes the LoRa gap in the same order.

Add a **XIAO ESP32-C6** (~$5) to either order as a near-free third. Every board in
this file is a display; nothing in it is a sensor node.
