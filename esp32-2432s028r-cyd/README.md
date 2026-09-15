# DIYmalls ESP32-2432S028R — 2.8" "Cheap Yellow Display"

**Status: owned** (bought 2026-09 on amazon.ca, connected 2026-09-15). The
chip is an ESP32-D0WD-V3 per esptool, USB is a CH340 (`/dev/cu.usbserial-*`),
and the pinout below is verified on this unit by [`apps/hello`](apps/hello/)
using [`lib/board/board.h`](lib/board/board.h).

**Buy:** [amazon.ca](https://www.amazon.ca/dp/B0CG2WQGP9) ·
[amazon.com](https://www.amazon.com/dp/B0CG2WQGP9)
&nbsp;&nbsp;**Docs:** [community repo (witnessmenow)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

Variants, if the single-board listing is out of stock: 2-pack
[CA](https://www.amazon.ca/dp/B0DNM4SKSJ) / [US](https://www.amazon.com/dp/B0DNM4SKSJ),
with acrylic case [CA](https://www.amazon.ca/dp/B0D8W9DSYZ) / [US](https://www.amazon.com/dp/B0D8W9DSYZ).

| | |
|---|---|
| SoC | ESP32-WROOM-32 — **dual-core** Xtensa @240MHz |
| Memory | 520KB SRAM, 4MB flash, no PSRAM |
| Radio | Wi-Fi 4 (b/g/n), BLE 4.2 — no Wi-Fi 6, no 802.15.4 |
| Display | 2.8" 240×320 ILI9341 over SPI |
| Touch | XPT2046 **resistive** (SPI) — needs a stylus-ish press, not multi-touch |
| Extras | microSD (separate SPI bus), speaker header, RGB LED, LDR |

## Why buy it

It is the best-documented cheap ESP32 display in existence. The community repo
above has a verified pinout, a large example set, and answers to most problems
you'll hit — which is the opposite of the situation on the C6, where half the
tutorials target a library that doesn't work.

It also fills two gaps in the collection: **touch input** and a **second core**
(so network work can run off the UI core), plus audio output.

## Two cautions

1. **Buy the exact model.** DIYmalls sells near-identical boards that are not
   this one — `E32R28T`, `JC2432W328C` (capacitive, ST7789). Only
   `ESP32-2432S028R` matches the community pinout.
2. **Power it from a 5V supply,** not a laptop port. The listing is explicit and
   people do hit brownouts.

Also note the touch is *resistive*, not capacitive: fine for buttons, poor for
swipes and useless for pinch. Design chunky targets. And it's the only board here
that predates Arduino core 3.x requirements, so TFT_eSPI *does* work — much of
the ecosystem assumes it.

## Toolchain

PlatformIO with the same pioarduino fork as the other boards; one env per app,
`huge_app.csv` because 4MB. Config lives in `apps/<app>/data/config.json` via
the C6's shared `appcfg.h` and `./push-config <app>`. The three SPI devices
are on three separate pin sets: display on HSPI, SD on the VSPI pins, touch
bit-banged on its own pins.

```sh
pio run -e hello -t upload -t monitor
./push-config hello
```

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 240×320.

| App | Idea |
|---|---|
| [hello](apps/hello/) | **Built.** Smoke test: panel, colour order, touch with raw readout, LED, LDR, BOOT, speaker click |
| [ticker](apps/ticker/) | **Built** (list + detail). The C6 watchlist with a sparkline per row and tap-to-open pages; fetches on core 0 |
| [cars/…](../cars/) | The OBD-II apps -- gauge cluster, track page, trip log, code reader -- mostly target this board; they live in `cars/` because the dongle path works from any of them |
| [touch-dashboard](apps/touch-dashboard/) | Home Assistant / smart-home control panel |
| [pomodoro](apps/pomodoro/) | Work timer with touch controls and an audible chime |
| [soundboard](apps/soundboard/) | Tap-to-play sample grid over the speaker header |
| [sd-file-browser](apps/sd-file-browser/) | Browse and view files on the card |
| [bike-buddy](apps/bike-buddy/) | Mountain bike computer: GPS, BLE sensors, offline map, no phone |
