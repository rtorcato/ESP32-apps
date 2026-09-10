# desk-clock — **built**

<img src="preview.svg" alt="desk-clock preview" width="172">

NTP clock with current weather and a three-day forecast. Time on top, current
conditions in the middle, forecast columns, date footer. Backlight dims
overnight, and the RGB LED tints by temperature.

Weather comes from [open-meteo](https://open-meteo.com) — **no API key needed**,
which is why it's the right source for this.

## Run it

Two things to set first.

**1. Wi-Fi credentials.** Copy the example and fill it in — `secrets.h` is
gitignored, so it never gets committed:

```sh
cp lib/board/secrets.h.example lib/board/secrets.h
$EDITOR lib/board/secrets.h
```

**2. Your location and timezone.** The defaults at the top of
[`src/main.cpp`](src/main.cpp) are **Toronto** — change them if that's wrong:

```cpp
static const char *TZ_STRING = "EST5EDT,M3.2.0/2,M11.1.0/2";
static const float LAT = 43.6532f, LON = -79.3832f;
static const char *PLACE = "TORONTO";
```

Use a real POSIX TZ string, not a fixed UTC offset — that's what makes DST
automatic instead of a twice-yearly reflash.

```sh
~/.platformio-venv/bin/pio run -e desk-clock -t upload
```

On boot the serial log tells you where you stand:

```
wifi ok TORCATO-5G -48dBm ip 192.168.1.42
ntp ok
selfcheck ok
wx ok -4.0C snow
```

## Tunables

| Constant | Default | Why you'd change it |
|---|---|---|
| `BL_DAY` / `BL_NIGHT` | 200 / 50 | Backlight brightness. Tune in the dark — 200 is harsh at night |
| `NIGHT_FROM` / `NIGHT_TO` | 23 / 7 | When to dim |
| `WX_PERIOD_MS` | 15 min | Weather poll. open-meteo is free; don't hammer it |

## How it stays flicker-free

Every draw goes through `field()`, which erases an **exact box** then draws into
it. Boxes are fixed at compile time, so a shorter string can never leave stale
pixels from a longer one. `loop()` never calls `fillScreen()` — the minute block
repaints once a minute and the seconds once a second, nothing else.

This is easier than it sounds here because Arduino_GFX has **no bundled
proportional fonts** — only the built-in 6×8, which scales by integer factor. A
glyph is exactly `6*size × 8*size`, so the dirty rectangles are exact rather than
measured. `setTextSize(5)` gives 30×40px digits, and `"14:32"` is exactly 150px
wide on a 172px panel.

## What building it actually taught me

Four things that weren't obvious from the plan:

- **The default partition table is too small.** WiFi + TLS + ArduinoJson +
  Arduino_GFX reached **93.2%** of the default 1.31MB app partition, because a
  4MB flash gets split into two OTA slots. `board_build.partitions =
  huge_app.csv` gives a single ~3MB partition and drops it to **38.8%**. Any app
  on this board that does TLS will hit this.
- **Never call `WiFi.reconnect()` from a fast loop.** My first version called it
  every 250ms while disconnected and the log filled with
  `E wifi:sta is connecting, return error`. `WiFi.setAutoReconnect(true)` in
  `setup()` already handles retries — the fix was deleting code.
- **USB CDC eats your boot logs.** The port takes ~2s to enumerate, so anything
  printed at the top of `setup()` is simply never seen. The self-check runs early
  (where its asserts can abort before anything draws) but logs at the *end* of
  `setup()`.
- **The JSON filter is mandatory, not an optimization.** open-meteo's full
  response will not fit in this heap alongside the display buffers.

## Self-check

`selfCheck()` asserts the WMO weather-code → label map and that `"14:32"` fits
the panel width. That map is the only non-trivial pure logic here and the only
part that can be silently wrong on screen. A failed assert panics the chip, which
is loud and obvious. It prints `selfcheck ok` when it passes.

## Not done

- **Weather icons.** Text labels only. Icons mean either a font with glyphs or
  bitmaps in flash; the labels read fine at this size.
- **Runtime config.** Location and TZ are compile-time constants. A captive
  portal to set them would be more code than reflashing when you move.
- **Sunrise/sunset, wind, UV.** open-meteo returns them, and the panel has room
  in the footer if you want them.
