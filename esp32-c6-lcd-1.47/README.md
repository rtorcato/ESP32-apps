# Waveshare ESP32-C6-LCD-1.47

**Status: owned.** Pinout verified, `hello` and `desk-clock` build and run.

> **Writing an app for this board? Start with
> [APP-CHECKLIST.md](APP-CHECKLIST.md)** — the conventions and measured
> optimisations that every app here should follow, in one place.

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

The panel works either way up, so **most apps should support both**. There are
two ways, and the runtime one is usually better:

**Runtime (preferred).** Keep coordinates in a `Layout` struct, pick one at
runtime, and call `gfx->setRotation(0 or 1)`. One button press changes
orientation with no reflash, and the self-check can assert *both* layouts.
[desk-clock](apps/desk-clock/src/main.cpp) does this — short-press BOOT cycles
dark/light × portrait/landscape.

**Compile-time.** For an app with a fixed layout, build with
`-DBOARD_LANDSCAPE` and add a `-h` env:

```ini
[env:<name>-h]
build_src_filter = -<*> +<<name>/>
build_flags = ${env.build_flags} -DBOARD_LANDSCAPE
```

`board.h` turns that flag into a rotation and swaps `LCD_W`/`LCD_H`. Either way,
**write layouts against the logical width/height, never against literal
172/320**.

Two things worth knowing:

- **`(34, 0, 34, 0)` is correct in all four rotations**, not just portrait. The
  driver picks a different offset pair per rotation, and 240 − 172 − 34 = 34
  makes the panel symmetric, so both column offsets are the same number.
- **Assert your layout bounds** in the app's self-check. A rotated layout that
  overruns the panel edge draws silently wrong; an assert panics at boot instead.
  This is how the date footer's 4px overrun got caught.

Each orientation gets its own preview: `preview.svg` (vertical) and
`preview-h.svg` (horizontal).

## Heat — it's the backlight, measured

The board runs warm. I measured the die sensor (`temperatureRead()`) at steady
state to find out where it actually comes from, rather than guessing:

| Configuration | Die temp |
|---|---|
| 160MHz, no Wi-Fi power save, backlight 200 | **51.1 °C** |
| 80MHz + `WIFI_PS_MAX_MODEM`, backlight 200 | 49.1 °C |
| 80MHz + `MAX_MODEM`, **backlight 140** | **43.1 °C** |
| 80MHz + `MAX_MODEM`, panel blanked | 41.1 °C and still falling |

**The LCD backlight dominates.** Halving the CPU clock and parking the radio
bought only 2 °C; dropping the backlight from 200 to 140 bought 6 °C more, and
blanking the panel entirely is cooler still. That inverts the intuition that a
radio and a 160MHz core are the hot parts.

A second pass removed one consumer at a time to find where the *remaining* heat
goes, and the answer is mostly "the board itself":

| State | Die temp |
|---|---|
| Radio on, backlight 140 | 44.1 °C |
| **Radio off**, backlight 140 | 43.1 °C — only **1 °C** |
| Radio off + **backlight 0** | 38.1 °C and falling → floor ≈ **37 °C** |

**The radio costs about 1 °C.** That is worth knowing because it kills the
obvious next optimisation: disconnecting Wi-Fi between polls would add
reconnect delays and failure modes to buy roughly one degree. Don't.

The **~37 °C floor** is the CPU at 80MHz plus the 5V→3.3V LDO and regulators —
you cannot get below it while the board is powered. So the whole controllable
range is about 37–48 °C, and the backlight is essentially the only dial.

For context: the ESP32-C6 is rated to 105 °C junction. **44 °C is not a
problem**, even though the board feels warm to the touch — surface temperature
runs cooler than the die, and warm is normal for an always-on board with a lit
LCD.

So the levers, in order of effect:

1. **Backlight.** `Theme::blDay` / `blNight` in [`lib/board/ui.h`](lib/board/ui.h).
   Defaults are now 140/40 (dark) and 70/18 (light) rather than 200/50.
2. **Blank it when you're not looking** — hold BOOT 3s, or add a schedule.
   Night dimming already does a softer version of this.
3. **`setCpuFrequencyMhz(80)` and `WiFi.setSleep(WIFI_PS_MAX_MODEM)`** — worth
   the 2 °C since nothing here is compute-bound. 80MHz is the floor that still
   supports Wi-Fi on this chip.

Some warmth is unavoidable: the 5V→3.3V LDO dissipates `(5 − 3.3) × I`, so every
milliamp saved anywhere shows up as less heat there too. None of this is a
reliability concern at these temperatures — but it does bias any onboard
temperature reading, which is why [thermostat](../elecrow-rotary-2.1/apps/thermostat/)
insists on a remote sensor.

## Apps

Each app README has a `preview.svg` wireframe drawn at the panel's real 172×320,
so what you see is what actually fits. New apps should follow
[APP-CHECKLIST.md](APP-CHECKLIST.md).

| App | Status | Idea |
|---|---|---|
| [hello](apps/hello/) | **Built** | Smoke test — display offset, backlight, LED, button |
| [unifi-status](apps/unifi-status/) | Idea | UniFi site health, WAN throughput, per-device rows |
| [wan-watchdog](apps/wan-watchdog/) | Idea | ISP flap log that survives reboots |
| [protect-doorbell](apps/protect-doorbell/) | Idea | UniFi Protect ring + motion alerts |
| [poe-cycle](apps/poe-cycle/) | Idea | Switch port view + power-cycle a wedged PoE device |
| [desk-clock](apps/desk-clock/) | **Built** (v+h, runtime) | NTP clock + open-meteo weather and forecast |
| [host-monitor](apps/host-monitor/) | **Built** (v+h) | Mac/Linux host stats, two auto-cycling pages |
| [ticker](apps/ticker/) | Idea | Scrolling stock/crypto price list |
| [wifi-scanner](apps/wifi-scanner/) | Idea | Live ranked list of nearby APs |
| [zigbee-hub](apps/zigbee-hub/) | Idea | Read 802.15.4 sensors direct, no hub |
| [sd-photo-frame](apps/sd-photo-frame/) | Idea | Slideshow off the TF card |
| [notifier](apps/notifier/) | Idea | MQTT/webhook alerts on screen + LED |
