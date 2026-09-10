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

Apps that use Wi-Fi need credentials first. `secrets.h` is gitignored:

```sh
cd esp32-c6-lcd-1.47          # all paths below are relative to here
cp lib/board/secrets.h.example lib/board/secrets.h && $EDITOR lib/board/secrets.h
~/.platformio-venv/bin/pio run -e desk-clock -t upload
```

[`lib/board/secrets.h.example`](lib/board/secrets.h.example) is the tracked
template; `secrets.h` beside it is gitignored and shared by every app here.

**Gitignoring it protects the repo, not the board.** Credentials are compiled
into the firmware, so anyone holding the device can read them out of flash. Use
an isolated IoT VLAN you can rotate — see [SECURITY.md](../SECURITY.md).

**TLS needs a bigger app partition.** The default 4MB table splits into two OTA
slots and leaves ~1.31MB for the app — WiFi + TLS + ArduinoJson + Arduino_GFX
alone reaches 93% of it. `platformio.ini` sets `huge_app.csv` (single ~3MB
partition, no OTA), which drops that to 39%.

Add an app by creating `apps/<name>/src/main.cpp` and appending to
`platformio.ini`:

```ini
[env:<name>]
build_src_filter = -<*> +<<name>/>
```

## Orientation

The panel works either way up, so **most apps should ship both**. It's a build
flag, not a fork:

```ini
[env:<name>]                                    ; vertical, 172x320
build_src_filter = -<*> +<<name>/>

[env:<name>-h]                                  ; horizontal, 320x172
build_src_filter = -<*> +<<name>/>
build_flags = ${env.build_flags} -DBOARD_LANDSCAPE
```

`board.h` turns that flag into a rotation and swaps `LCD_W`/`LCD_H`, so **write
layouts against `LCD_W`/`LCD_H`, never against literal 172/320**. Keep the
orientation-dependent coordinates in one constant block per app — see
[desk-clock](apps/desk-clock/src/main.cpp), where orientation touches only that
block and the rules in `drawChrome()`.

Two things worth knowing:

- **`(34, 0, 34, 0)` is correct in all four rotations**, not just portrait. The
  driver picks a different offset pair per rotation, and 240 − 172 − 34 = 34
  makes the panel symmetric, so both column offsets are the same number.
- **Assert your layout bounds** in the app's self-check. A rotated layout that
  overruns the panel edge draws silently wrong; an assert panics at boot instead.
  This is how the date footer's 4px overrun got caught.

Each orientation gets its own preview: `preview.svg` (vertical) and
`preview-h.svg` (horizontal).

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
| [desk-clock](apps/desk-clock/) | **Built** (v + h) | NTP clock + open-meteo weather and forecast |
| [mac-mini](apps/mac-mini/) | Idea | Mac mini stats + sleep/wake button |
| [ticker](apps/ticker/) | Idea | Scrolling stock/crypto price list |
| [wifi-scanner](apps/wifi-scanner/) | Idea | Live ranked list of nearby APs |
| [zigbee-hub](apps/zigbee-hub/) | Idea | Read 802.15.4 sensors direct, no hub |
| [sd-photo-frame](apps/sd-photo-frame/) | Idea | Slideshow off the TF card |
| [notifier](apps/notifier/) | Idea | MQTT/webhook alerts on screen + LED |
