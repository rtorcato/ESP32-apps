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
- [ ] `setCpuFrequencyMhz(80)`, and `netTune()` + `netJoinBest()` from `<netjoin.h>`
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

Swept again later, holding everything else fixed (80MHz, `MIN_MODEM`, Wi-Fi up,
~3 minutes per step to reach steady state):

| Backlight duty | Die temp | LED current vs 140 |
|---|---|---|
| 140 | 45.1 °C | — |
| 100 | ~43.5 °C | −29 % |
| 60 | ~40.5 °C | −57 % |
| 0 | 37.1 °C | −100 % |

So the panel is worth ~8 °C across its full range, roughly linear in duty, and
the SoC-only floor is confirmed at 37 °C.

**The backlight is the dial. Almost nothing else is.**

- Backlight 200 → 140 is worth ~6 °C. Keep `blDay` modest; per-scheme values are
  in `ui.h`.
- **Duty is proportional to LED current, so the saving is real even where the
  die barely moves.** The backlight LEDs sit on the panel, not on the die, so
  the internal sensor *under-reports* what dimming saves. The die thermometer is
  the only instrument here; a USB power meter would settle actual mA, and until
  someone puts one inline, treat duty as the honest proxy and temperature as a
  lower bound on the benefit.
- **A dark UI can go much dimmer than a light one.** ticker runs duty 96 on a
  black screen with high-contrast text and stays perfectly legible, against the
  shared themes' 140.

**Automatic light sleep is not available.** `esp_pm_configure()` with
`light_sleep_enable = true` returns **`ESP_ERR_NOT_SUPPORTED`** on this
Arduino-ESP32 core — `CONFIG_PM_ENABLE`/tickless idle are not compiled in. Tested,
not assumed. Getting it would mean a custom IDF build, so the tricks left are the
backlight, not polling when the screen is blanked, and a longer `delay()` in
`loop()` so the idle task can park the core.

**Don't do network work while the screen is blanked.** A blanked panel is a
panel nobody is reading, so polling is pure waste — and it needs no catch-up
logic if the interval timers keep running, because the first pass after a wake
is already overdue and refetches on its own.

**Trim Wi-Fi transmit power only when the measured RSSI says there is margin.**
`netTune()` asks for 19.5 dBm because a weak spot once failed to associate at
all; at −50 to −62 dBm most of that is margin spent as heat, so dropping to
13 dBm is free. Gate it on the actual RSSI rather than doing it unconditionally,
and don't expect to see it on the die thermometer — the transmitter is only
active in short bursts.
- CPU 160 → 80MHz is worth ~2 °C. Take it — nothing here is compute-bound and
  80MHz is the floor that still supports Wi-Fi.
- **The radio costs ~1 °C *when parked with `MAX_MODEM`*.** That qualifier is
  load-bearing: see the Wi-Fi section. Running the receiver continuously costs
  **12 °C**, and reusing the 1 °C figure without it is how that regression
  happened. Either way, do not disconnect Wi-Fi between polls — it buys about a
  degree in exchange for reconnect delays and new failure modes.
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

**`setAutoReconnect(true)` is not enough on its own — re-issue `begin()`.** After
a cold boot where the connect window expired, the panel sat on `NO WIFI`
indefinitely and only a power cycle fixed it. Since the association failure is
transient, never retrying *was* the bug. Re-issue `WiFi.begin()` every ~20s
while disconnected and log the attempt number:

```
wifi retry #1
wifi retry #2
```

**Set power save before joining, not after — but set it to `MIN_MODEM`.** The
ordering mattered when the setting was `MAX_MODEM`, because parking the radio
mid-handshake makes that handshake time out. With `MIN_MODEM` the radio wakes
for every beacon, so it is safe either side of association and `netTune()` does
it up front.

**Pin to the strongest BSSID — use `lib/board/netjoin.h`.** Where two APs
broadcast one SSID, letting the stack choose means it can latch onto the far one
or bounce between them, and that shows up as reason 34 `MISSING_ACKS` plus
repeated 204 `HANDSHAKE_TIMEOUT` — which reads exactly like a wrong password.
Measured here: a link that sat at −76/−86 dBm with constant reconnects became a
steady −53 dBm that connects first try. Re-scan on every retry so a genuinely
better AP still wins.

**Use `WIFI_PS_MIN_MODEM`. Both obvious alternatives are wrong.** Measured on
this board:

| Setting | Result |
|---|---|
| `MAX_MODEM` | ~2 °C cooler, but drops ACKs on a marginal link → reason 34 |
| `setSleep(false)` | Rock solid, and **+12 °C** (43 °C → 55 °C) |
| **`MIN_MODEM`** | Wakes for every beacon, sleeps between. Nothing missed, ~44 °C |

And a reasoning trap worth naming: the heat table above shows "the radio costs
about 1 °C", but that compared radio *off* against `MAX_MODEM` — **not** against
no-sleep. Reusing that number to justify `setSleep(false)` is how the 12 °C
regression happened.

**Back off the retry interval.** Retrying every 20s forever at full TX power is
its own heat source — the board ran ~9 °C hotter while failing than while
connected. `netRetryDelay()` doubles to a 2 minute ceiling.

**Signal strength is the hidden variable.** The same firmware connected first
try at −58 dBm and failed a 20s window at −86 dBm. Log `WiFi.RSSI()` on every
connect, because "it worked yesterday" often means "it was 28 dB stronger
yesterday".

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

**Capture button edges in an interrupt, never by polling in `loop()`.** This is
the mistake that made the button feel broken: `loop()` has a `delay(100)` plus a
`getLocalTime()` that can block another 100ms plus drawing, so the button was
only sampled every ~250ms. A genuine quick tap that began *and ended* between
two samples was never seen at all, which presents as "the button needs a long
press to work". `ui.h` now records the edges and the held duration in an ISR and
`loop()` collects a finished press whenever it gets round to it.

**Debounce inside the ISR by ignoring edges too soon after the last.** Contact
bounce arrives as a burst; without that filter a single hold registers as
release + press and every fragment looks like a tap.

**Log every press with its measured duration.** One line turns "the gesture
doesn't work" into a fact.

**Acknowledge the press instantly, and decide the gesture on release.** Deciding
on release is what stops a hold-for-the-third-action from firing the second on
the way past — but it means nothing happens until the user lets go, and a 250ms
press is 250ms of apparent lag. Measured here, the redraw after release is only
~52 ms, so the lag was pure perception. `uiPressDot()` draws a small corner dot
the instant the button goes down; the hold hints take over from 2s.

**An app may repurpose a gesture, but it must relabel the hint.** `uiPoll()`
shows what releasing now would do, so a hold that promises `COLOUR` and switches
layout instead is a lie the UI tells. Claim the gesture by checking for it
*before* calling `uiHandle()` (the pattern `ui.h` already documents for
`Setup`), and set `uiHoldSchemeLabel`. ticker does both: it pins one scheme —
black, white symbols, green and red numbers — and spends the 2s hold on cycling
its layout.

**Not every app wants nine themes.** Selectable schemes are right for a clock
that lives in a bedroom; for a data panel where colour *is* the data, a fixed
palette is better and frees the gesture. Pin it with `uiApply(uiRot(), 0,
false)` after `uiBegin()` — `persist=false`, so pinning doesn't become an NVS
write on every boot — and note that this also overrides whatever a previous app
left in the same NVS namespace.

**Measure before blaming a subsystem.** I attributed this lag to the two NVS
writes in `uiApply()` and deferred them out of the press path — which was worth
doing anyway, since it collapses a run of taps into one write — but the
instrumented figure showed the nvs commit costs **3 ms**. The theory was wrong
and one `Serial.printf` of elapsed milliseconds settled it.

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

## Configuration

**Every app gets `apps/<app>/data/config.json`, read through `lib/board/appcfg.h`.**
The split is one rule:

| | Where | Why |
|---|---|---|
| anything worth changing without a rebuild | `<app>/data/config.json` | on LittleFS, pushed in ~4s |
| anything **secret** — Wi-Fi PSK, API keys, portal passwords | `lib/board/secrets.h` | gitignored, compiled in |

A credential in `config.json` is a credential published: it is plain text on a
filesystem anyone holding the board can dump with `esptool read_flash`. That is
not a hypothetical — see [SECURITY.md](SECURITY.md), including why flash
encryption is not the fix here.

```sh
./push-config <app>          # just the config, no rebuild, ~4s
pio run -e <app> -t upload   # firmware, only when src/ changes
```

`push-config` validates the JSON **before** touching the board, because
`uploadfs` will happily build an image from a malformed file and the app then
boots on its defaults looking like the edit did nothing.

`data_dir` is a `[platformio]` option and cannot be set per-env, so
`scripts/appdata.py` sets it from the env name — an explicit
`PLATFORMIO_DATA_DIR` still wins, because overriding a default is helpful and
overriding an explicit instruction is a trap. (That one bit me: the hook
originally clobbered the variable unconditionally and reported success, so a
test that meant to upload an empty filesystem silently uploaded the real one.)

**Rules that make a config file safe to hand-edit:**

- **Every key is optional, and the app must run with the file absent.** The file
  *tunes* an app, it does not *enable* one — verified by uploading an empty
  filesystem and watching it boot. The single exception is an app whose config
  *is* its data (ticker's watchlist), which says so on screen when it's missing.
- **Reject and name bad values; never clamp silently.** `cfgInt()` keeps the
  previous value and logs `config x: 999 out of range 8..255, keeping 96`. A
  typo quietly rounded to the nearest legal value is a setting that "doesn't
  work" with no explanation.
- **Sanity-check combinations, not just ranges.** Each value can be individually
  legal and jointly nonsense: an inverted market window makes `marketOpen()`
  permanently false, and a stale timeout under two poll intervals marks a
  healthy feed dead. Both are caught and corrected out loud.
- **Floor a brightness at 8, never 0.** Duty 0 reads as a dead board with no way
  back except the serial log.
- **Don't return pointers into the parsed document.** `cfgStr()` copies into a
  caller buffer; the tree is freed with `cfgRelease()` after boot, so a returned
  pointer would dangle and work right up until it didn't.
- **An override should be able to defer to the default.** Backlight keys are
  opt-in: omit one and that level still comes from the active colour scheme,
  because the nine schemes carry their own duty and a blanket override throws
  that away. `uiBacklightFromScheme()` is exposed for exactly this.

`cfgSelfCheck()` asserts the accessors — dotted paths, absent keys, wrong types,
out-of-range, paths through a scalar, absent arrays, and `HH:MM` parsing. Call it
from an app's `selfCheck()`. It silences its own deliberate rejections, which
otherwise print into the boot log looking like real errors.

## Per-app deliverables

- **`README.md`** — what it does, why this board, the hard parts, and what you
  learned building it.
- **`data/config.json`** — with a `_readme` key explaining each setting and its
  range. Comments aren't legal JSON, and a settings file nobody can read is a
  settings file nobody changes.
- **`preview.svg`**, and `preview-h.svg` if the layout differs by orientation,
  drawn at **native resolution** with `textLength` pinning the real `6*size`
  glyph advance, so if it fits the wireframe it fits the panel.
- **One runnable check.** A `selfCheck()` with asserts over the app's
  non-trivial pure logic and its layout bounds. A failed assert panics the chip,
  which is loud and obvious.
