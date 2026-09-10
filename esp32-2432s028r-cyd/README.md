# DIYmalls ESP32-2432S028R — 2.8" "Cheap Yellow Display"

**Status: wishlist.** Pinout below is from community docs, not verified on
hardware — confirm before writing a `lib/board/board.h`.

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

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 240×320.

| App | Idea |
|---|---|
| [touch-dashboard](apps/touch-dashboard/) | Home Assistant / smart-home control panel |
| [pomodoro](apps/pomodoro/) | Work timer with touch controls and an audible chime |
| [soundboard](apps/soundboard/) | Tap-to-play sample grid over the speaker header |
| [sd-file-browser](apps/sd-file-browser/) | Browse and view files on the card |
