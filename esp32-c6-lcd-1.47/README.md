# Waveshare ESP32-C6-LCD-1.47

**Status: owned.** Pinout verified, `hello` builds and runs.

**Buy:** [amazon.ca](https://www.amazon.ca/dp/B0DHTMYTCY) ·
[amazon.com](https://www.amazon.com/dp/B0DHTMYTCY) ·
[waveshare](https://www.waveshare.com/esp32-c6-lcd-1.47.htm)
&nbsp;&nbsp;**Docs:** [wiki](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47) ·
[docs](https://docs.waveshare.com/ESP32-C6-LCD-1.47)

| | |
|---|---|
| SoC | ESP32-C6 — single-core RISC-V @160MHz, plus an LP core @20MHz |
| Memory | 512KB HP SRAM, 16KB LP SRAM, 4MB flash, **no PSRAM** |
| Radio | Wi-Fi 6 (2.4GHz), BLE 5, **802.15.4** (Thread / Zigbee / Matter) |
| Display | 1.47" IPS, 172×320, ST7789 over SPI |
| Extras | WS2812 RGB LED, microSD, native USB, BOOT button |
| Power | USB-C only — no battery connector or charger |

## Pinout

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| LCD MOSI | 6 | LCD SCLK | 7 |
| LCD CS | 14 | LCD DC | 15 |
| LCD RST | 21 | LCD BL | 22 |
| RGB LED | 8 | BOOT button | 9 |
| SD MISO | 5 | SD CS | 4 |

All of this lives in [`lib/board/board.h`](lib/board/board.h) — include `<board.h>`
rather than re-typing pin numbers per app.

## Three things that will bite you

1. **34px column offset.** The 172px panel sits on a 240-wide ST7789 controller.
   Without `col_offset1 = col_offset2 = 34` everything draws shifted left with a
   junk stripe on the right. `boardDisplay()` already handles it.
2. **TFT_eSPI does not work.** It has no Arduino-ESP32 core 3.x support, and the
   C6 exists only in core 3.x. Use Arduino_GFX (this repo), LVGL, or ESP-IDF
   `esp_lcd`. Half the tutorials you'll find are for TFT_eSPI — skip them.
3. **SD shares the LCD bus** (MOSI 6 / SCLK 7). Two CS lines, one bus: never
   hold both low, and expect display stalls while streaming off the card.

Also: no PSRAM and 512KB of SRAM means no full-screen framebuffer for anything
elaborate — draw incrementally. And it's single-core, so a blocking network call
freezes the UI unless you push it onto a task or use non-blocking calls.

## Design notes

The display is **tall and narrow** (172×320 portrait). That suits vertical
lists, tickers, and stacked stat rows; it fights anything wide, like a graph with
a time axis. **There is no touchscreen** — the only input is the BOOT button on
GPIO9, so interaction is one button or none. Prefer apps that display rather than
apps that need control.

The 802.15.4 radio is the board's real differentiator. Nothing else on my
wishlist can speak Zigbee or Thread natively.

## Build

```sh
pio run -e hello -t upload -t monitor
```

Add an app by creating `apps/<name>/src/main.cpp` and appending to
`platformio.ini`:

```ini
[env:<name>]
build_src_filter = -<*> +<<name>/>
```

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 172×320,
so what you see is what actually fits.

| App | Status | Idea |
|---|---|---|
| [hello](apps/hello/) | **Built** | Smoke test — display offset, backlight, LED, button |
| [unifi-status](apps/unifi-status/) | Idea | UniFi site health, WAN throughput, per-device rows |
| [wan-watchdog](apps/wan-watchdog/) | Idea | ISP flap log that survives reboots |
| [protect-doorbell](apps/protect-doorbell/) | Idea | UniFi Protect ring + motion alerts |
| [poe-cycle](apps/poe-cycle/) | Idea | Switch port view + power-cycle a wedged PoE device |
| [mac-mini](apps/mac-mini/) | Idea | Mac mini stats + sleep/wake button |
| [desk-clock](apps/desk-clock/) | Idea | NTP clock + weather, stacked vertically |
| [ticker](apps/ticker/) | Idea | Scrolling stock/crypto price list |
| [wifi-scanner](apps/wifi-scanner/) | Idea | Live ranked list of nearby APs |
| [zigbee-hub](apps/zigbee-hub/) | Idea | Read 802.15.4 sensors direct, no hub |
| [sd-photo-frame](apps/sd-photo-frame/) | Idea | Slideshow off the TF card |
| [notifier](apps/notifier/) | Idea | MQTT/webhook alerts on screen + LED |
