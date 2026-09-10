# Every app on this board should do these

Hard-won on this hardware, mostly by getting it wrong first. Each item says what
to do and — where it was measured — the number that justifies it.

Read this before writing a new app in [`apps/`](apps/). The reference
implementation for nearly all of it is
[`apps/desk-clock/src/main.cpp`](apps/desk-clock/src/main.cpp).

## The short version

- [ ] `#include <ui.h>` — schemes, 4 rotations, gestures, backlight, blanking
- [ ] `uiBegin(gfx, "<appname>")` with its **own** NVS namespace
- [ ] Layout from a `Layout` struct chosen on `uiLandscape()`, never literal 172/320
- [ ] `selfCheck()` asserting **both** layouts, run early, logged at the *end* of `setup()`
- [ ] Draw through an exact-box `field()` helper; **never `fillScreen()` in `loop()`**
- [ ] `POWER_SAVE`: `setCpuFrequencyMhz(80)` + `WiFi.setSleep(WIFI_PS_MAX_MODEM)`
- [ ] Wi-Fi: 20s connect window, `setAutoReconnect(true)`, `persistent(false)`
- [ ] A real offline/degraded screen that says what's wrong and what to fix
- [ ] `ArduinoJson` **filter**, and validate the payload before trusting it
- [ ] Credentials from `<secrets.h>`, on an isolated IoT VLAN
- [ ] `preview.svg` (+ `preview-h.svg`) at native resolution, and a README

## Display

**Use `ui.h`, don't re-implement it.** [`lib/board/ui.h`](lib/board/ui.h) owns the
five colour schemes, all four rotations, per-scheme backlight, screen blanking
and the button gestures. Layout stays per-app because coordinates are
app-specific — only the pattern is shared.

**All four rotations, not two.** The USB-C socket is on a fixed edge, so which
way up the board sits depends on where the cable has to go. Rotations 0/2 are
172×320 and 1/3 are 320×172, so two layouts cover four rotations.

**`(34, 0, 34, 0)` is correct in all four rotations.** The 172px panel sits on a
240-wide ST7789 controller. The driver picks a different offset pair per
rotation, and 240 − 172 − 34 = 34 makes the panel symmetric, which is what makes
runtime rotation safe. Without the offset everything draws shifted left with a
junk stripe on the right.

**Never `fillScreen()` in `loop()`** — that is what makes large digits flicker.
Erase an exact box and draw into it. This is easy here because Arduino_GFX
bundles **no proportional fonts**: the built-in 6×8 scales by integer factor, so
a glyph is exactly `6*size × 8*size` and dirty rects are exact rather than
measured.

**Size boxes by character count and assert they fit.** A 14-char box at size 2 is
168px, which overruns the 172px panel — the date footer did exactly that, and
because the text still centred it *looked* fine while the erase rect clipped.

**`RGB565_*` prefixed colour constants.** `BLACK`/`WHITE` do not exist in
Arduino_GFX 1.6.x.

**TFT_eSPI does not work at all** — no Arduino-ESP32 core 3.x support, and the C6
exists only in core 3.x. Half the tutorials online are unusable here.

## Power and heat

Measured with `temperatureRead()` at steady state, not estimated:

| Configuration | Die temp |
|---|---|
| 160MHz, no radio power save, backlight 200 | 51.1 °C |
| 80MHz + `WIFI_PS_MAX_MODEM`, backlight 200 | 49.1 °C |
| 80MHz + `MAX_MODEM`, backlight 140 | 44.1 °C |
| Radio **off**, backlight 140 | 43.1 °C |
| Radio off + backlight 0 | 38.1 °C, falling → floor ≈ 37 °C |

**The backlight is the dial. Almost nothing else is.**

- Backlight 200 → 140 is worth ~6 °C. Keep `blDay` modest; per-scheme values are
  in `ui.h`.
- CPU 160 → 80MHz plus radio power save is worth ~2 °C. Take it — nothing here is
  compute-bound and 80MHz is the floor that still supports Wi-Fi.
- **The radio costs ~1 °C.** Do *not* disconnect Wi-Fi between polls to save
  power: it buys one degree in exchange for reconnect delays and new failure
  modes.
- **~37 °C is the floor** (CPU + the 5V→3.3V LDO + regulators). The whole
  controllable range is ~37–48 °C.

For context the C6 is rated to **105 °C** junction, so 44 °C is not a concern
even though the board feels warm to the touch. It does, however, bias any
onboard temperature reading — use a remote sensor if you need real ambient.

## Flash and memory

**Set `board_build.partitions = huge_app.csv`.** The default 4MB table splits
into two OTA slots leaving ~1.31MB for the app, and WiFi + TLS + ArduinoJson +
Arduino_GFX alone reaches **93.2%** of that. `huge_app` gives a single ~3MB
partition and drops the same build to ~39%. Already set in
[`platformio.ini`](platformio.ini) for every env.

**No PSRAM, 512KB SRAM.** No full-screen framebuffer for anything elaborate —
draw incrementally. TLS wants ~40KB of *contiguous* heap; measured free heap
with the clock running is ~310KB, so there is room, but a fragmented heap fails a
handshake in a way that looks like a network problem.

**Single core.** A blocking network call freezes the UI. Either push it to a
task, or make the UI self-correcting — the clock reads the RTC rather than
counting ticks, so a 2s stall just makes the seconds jump.

**SD shares the LCD SPI bus** (MOSI 6 / SCLK 7). Two CS lines, one bus: never
hold both low, and expect display stalls while streaming off the card.

## Wi-Fi

**A 20-second connect window, not 10.** The WPA2 handshake can time out once
(`204 HANDSHAKE_TIMEOUT`) and succeed on the next attempt. A 10s window reports
a failure that would have connected — this cost real time and looked exactly
like a wrong password.

**`WiFi.setAutoReconnect(true)` and never `WiFi.reconnect()` in a fast loop.**
Calling it every 250ms floods the log with
`E wifi:sta is connecting, return error`. The fix is deleting code.

**`WiFi.persistent(false)`** so the credential isn't also copied into NVS. The
core defaults to `true`, which puts the PSK in two places.

**Diagnose, don't guess.** `"wifi FAILED"` cannot distinguish its own causes, and
each wrong guess costs a build-flash-observe cycle. Log:

- **The disconnect reason by name.** `201 NO_AP_FOUND` means the AP was never
  seen (wrong SSID, or 5GHz-only — **this board is 2.4GHz only**).
  `202`/`204`/`15` point at the handshake.
- **The advertised auth mode** from a scan, not just "encrypted" — that is what
  rules out WPA3-only or enterprise as a cause.
- **Don't over-claim.** Finding the SSID in a scan only rules out the *name*.
  `204` is also what you get from PMF-required or 802.11r, or from simply needing
  another attempt.
- **Ignore your own teardown.** `WiFi.disconnect()` before a scan produces
  `ASSOC_LEAVE`, which will mask the real reason if you record it.

**A build-freshness checksum is worth 5 lines.** PlatformIO caches objects, and a
stale build holding an old credential is indistinguishable from a wrong password.
Log an FNV-1a of the secret and compare it to the file on disk — non-reversible,
and it settles the question instantly.

## Network and APIs

**Never a guest network.** A UniFi guest WLAN intercepts everything until portal
auth, so the board associates, gets a DHCP lease, and nothing works. The failure
is deliberately confusing: a portal accepts TCP to *any* destination to serve its
redirect, so a TCP probe to 443 **succeeds** and only the TLS handshake fails
with `SSL - The connection indicated an EOF`. Plain HTTP exposes it:

```
http status: HTTP/1.1 302 Moved Temporarily
redirected to: http://10.0.10.1:8880/guest/s/default/?ap=...   <-- captive portal
```

Use a dedicated IoT WLAN with no portal. See [SECURITY.md](../SECURITY.md).

**Test the layers separately when associated but broken**: DNS resolution,
gateway reachability, a TCP probe, a plain-HTTP portal check, and free heap. That
is the difference between a diagnosis and a guess.

**`ArduinoJson` filters are mandatory, not an optimization** — a full API document
will not fit alongside the display buffers.

**Validate the payload before trusting it.** A 200 whose body isn't what you
expect deserializes fine, filters to nothing, and the `| default` operators turn
that into plausible-looking garbage — a real run printed `wx ok 0.0C ?` off a
portal page. Read the body into a `String` so a failure can show what arrived.

## Interaction

**Only one button is readable.** BOOT is GPIO9; **RESET is wired to the chip's EN
pin**, so it can only reboot — it cannot be read in software. All gestures share
BOOT. (Hold BOOT + press RESET = download mode, the manual flash recovery.)

**A long press cannot power the board down.** GPIO9 is not an RTC pin — the C6's
RTC-capable pins are GPIO0–7 (`SOC_RTCIO_PIN_COUNT == 8`), so deep sleep could
only be woken by RESET or a timer. Blank the panel instead and leave the app
running, so its data is current when the screen returns.

**Debounce by requiring a stable level**, not by filtering short presses. Contact
bounce during a hold registers as release + press, which makes every fragment
look like a tap — so hold gestures appear not to work at all.

**Log every press with its measured duration.** One line turns "the gesture
doesn't work" into a fact.

## Failure states

**An app with no data must say so, loudly and specifically.** A blank or nearly
blank panel reads as "broken", not "not connected". Show what's wrong *and* what
to change, with advice matched to the actual cause — generic advice sends you to
the wrong place.

**Degrade, don't hide.** If the clock has valid time but the weather failed, keep
the clock and mark the weather unavailable. Only take over the whole screen when
there is nothing useful to show.

**Show staleness.** A frozen panel displaying good data is worse than one
admitting it lost the network.

## Boot and logging

**USB CDC eats your early logs.** The port takes ~2s to enumerate, so anything
printed at the top of `setup()` is never seen. `delay(300)` after
`Serial.begin()` helps, but anything you actually need to read should be logged
at the *end* of `setup()`. Run the self-check early (where its asserts can abort
before drawing) and log its result late.

**`pio device monitor` needs a TTY.** To read serial from a script, use pyserial
directly — it ships in the venv. Note the port re-enumerates on reset, so a
handle held across a reboot dies.

## Per-app deliverables

- **`README.md`** — what it does, why this board, the hard parts, and what you
  learned building it.
- **`preview.svg`**, and `preview-h.svg` if the layout differs by orientation,
  drawn at **native resolution** with `textLength` pinning the real `6*size`
  glyph advance, so if it fits the wireframe it fits the panel.
- **One runnable check.** A `selfCheck()` with asserts over the app's
  non-trivial pure logic and its layout bounds. A failed assert panics the chip,
  which is loud and obvious.
