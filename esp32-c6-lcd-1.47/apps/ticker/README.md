# ticker — built

<img src="preview.svg" alt="ticker list layout" width="172">
<img src="preview-solo.svg" alt="ticker solo layout" width="172">

A watchlist panel. **One colour scheme only** — black background, white
symbols, green and red numbers — and **two layouts**, cycled by a 2s hold:

| Layout | What it shows |
|---|---|
| **LIST** | Six rows: symbol, price, signed change. A longer list pages through six at a time every 8s. |
| **SOLO** | One symbol filling the screen with its logo, advancing every 5s. |

The chosen layout persists in NVS, so it survives a power cycle.

**Flashed and verified** — 26 symbols (22 stocks, 4 coins) all fetching, 26/26
logos present, 43–44°C die temperature, ~287KB heap free (the 18KB logo buffer
accounts for the drop), 41.1% of the 3MB app partition.

Stocks are swept **one request per loop pass**, not in a burst. Yahoo tolerates
the rate fine — measured, 22 requests spaced 0.3s apart drew zero 429s, so the
original 429 really was just the missing User-Agent — but 22 *blocking* requests
back to back freeze `loop()` for ~26s. The ISR still latches a button press so
nothing is lost, yet the clock stops and the press lands half a minute late. One
per pass keeps the UI alive, finishes in ~30s regardless, and fills the rows in
progressively.

## Energy

Measured on this board by sweeping one variable at a time, ~3 minutes per step:

| Backlight duty | Die temp | LED current vs 140 |
|---|---|---|
| 140 (shared theme default) | 45.1 °C | — |
| **96 (ticker's `open`)** | ~43.5 °C | **−31 %** |
| 64 (`closed`) | ~40.5 °C | −54 % |
| 24 (`night`) | ~38 °C | −83 % |
| 0 | 37.1 °C | −100 % |

The panel is worth about 8 °C across its whole range and the SoC-only floor is
37 °C, so **the backlight is the only dial that moves much.** Three things
follow, and one caveat matters more than the table:

- **The die sensor under-reports what dimming saves.** The backlight LEDs are on
  the panel, not the die. Duty is proportional to LED current, so cutting duty
  31 % genuinely cuts backlight power ~31 % even though the thermometer only
  moves 1.6 °C. A USB power meter inline would give real mA; the die
  temperature is a lower bound on the benefit, not a measure of it.
- **A black screen tolerates far more dimming than a light one.** 96 is
  comfortably legible here against the themes' 140. Push `open` to 60 in
  `watchlist.json` for another ~3 °C if it's still too bright.
- **The market-closed level is the easy win.** The stock rows don't change
  between 16:00 and 09:30, or at all on weekends, so most of the week runs at
  64 or 24 rather than 96 — no loss of information at all.

Everything else, in order of what it's actually worth:

| Change | Worth |
|---|---|
| Backlight 140 → 96/64/24 | ~1.6–7 °C, and the only measurable one |
| Don't poll while the screen is blanked | 26 HTTPS requests per cycle saved; the biggest *radio* saving |
| CPU 160 → 80MHz (already on) | ~2 °C. 80MHz is the floor Wi-Fi allows |
| `WIFI_PS_MIN_MODEM` (already on) | vs no power save, **12 °C** |
| Wi-Fi TX 19.5 → 13 dBm | small, and below the die sensor's resolution |
| `loop()` `delay(20)` → `delay(50)` | small; lets the idle task park the core |

**Automatic light sleep is not available.** `esp_pm_configure()` with
`light_sleep_enable = true` returns `ESP_ERR_NOT_SUPPORTED` — tickless idle is
not compiled into this Arduino core. Tested, not assumed; it would need a custom
IDF build. Two things not done, both with a real cost: `WIFI_PS_MAX_MODEM` may
beat `MIN_MODEM` but `MIN_MODEM` was chosen deliberately earlier and relitigating
it needs its own measurement, and deep sleep can't be woken by this button
(GPIO9 is not an RTC pin).

Also fixed here, in `ui.h` and affecting every app: `uiBacklightNow()` was only
ever consulted by `uiApply()` and `uiSetScreen()`, so **the night dimming only
took effect if somebody happened to press the button after 23:00.** A board left
alone overnight burned the day level until morning. `uiTick()` now re-evaluates
every 30s and only touches the PWM when the level actually changes.

## Buttons

Same gestures as every app here, with one deliberate substitution:

| Gesture | Action |
|---|---|
| tap (< 2s) | rotate through the four orientations |
| hold 2s | **next layout** (other apps: next colour scheme) |
| hold 4.5s | screen off |

Ticker pins scheme 0 and spends the colour gesture on the layout instead, since
a stock panel wants exactly one palette. `ui.h` grew one variable for this —
`uiHoldSchemeLabel` — so the hold overlay reads `release: LAYOUT` rather than
promising `COLOUR` and doing something else.

Adding a third layout means an enum value, a geometry constant, a draw function
and a `checkX()` in `selfCheck()`. Nothing else dispatches on the mode.

## Logos

`tools/make-logos.py` fetches each symbol's logo, converts it to raw RGB565 at
96×96 and writes it into `data/logo/`, which then ships via `uploadfs`:

```sh
python3 tools/make-logos.py
PLATFORMIO_DATA_DIR=apps/ticker/data pio run -e ticker -t uploadfs
```

**The firmware carries no PNG decoder and fetches no images.** It opens
`/logo/<LABEL>.565`, checks the length is exactly 96×96×2, and blits it. That
keeps a decoder, a third TLS endpoint and an 18KB decode buffer off a 512KB
single-core part, and a missing logo becomes an ordinary case — the symbol is
drawn large instead — rather than a runtime failure. 12 logos is 221KB, 24% of
the data partition. The boot log inventories them (`logos: 12/12 present`) and
names any that are missing.

Sources, both tested and keyless: `financialmodelingprep.com/image-stock/` for
stocks, `assets.coincap.io/assets/icons/` for coins. `logo.clearbit.com` no
longer resolves at all and `img.logo.dev` wants a token, so neither is used.
Coincap keys on the ticker rather than the CoinGecko id, so a coin's `label` is
what gets looked up — a custom label that isn't a real ticker just falls back to
text.

**The conversion is where the interesting failure was.** These logos arrive in
three different shapes and a black screen punishes two of them:

- Most are transparent, and composite onto black correctly.
- Apple's is **solid black on opaque white**. Flattened naively it produces a
  perfectly valid file that draws *nothing*. The converter detects the light
  background from the image border, knocks it out to black, and inverts the
  dark glyph — giving a white Apple mark.
- Solana's PNG has **no alpha channel at all**, so `sips` emits 24bpp rather
  than 32bpp.

My first heuristic averaged luminance over the whole image. That broke Solana
precisely because it has no alpha: its black background counted as visible
pixels, the mean came out at luma 24, and the "too dark, invert it" rule flipped
the entire logo to a white square. **Read the background from the border, never
from the average.** The decision is made on the Mac, where there is full colour
information, and costs the firmware zero bytes.

## Settings

Everything tunable lives in [`data/watchlist.json`](data/watchlist.json) on the
device's filesystem, so changing any of it is an `uploadfs`, not a rebuild:

| Key | What | Range |
|---|---|---|
| `brightness.open` / `.closed` / `.night` | backlight duty per market state | 8–255 |
| `night.from` / `.to` | hours the night level applies | 0–23 |
| `layout` | which layout to start in | `list` / `solo` |
| `timing.pageSeconds` | **page speed** — how long six rows stay up | 2–600 |
| `timing.soloSeconds` | how long one symbol holds the solo screen | 2–600 |
| `timing.cascadeMs` | per-row stagger on a page flip; 0 = instant | 0–400 |
| `refresh.openMinutes` / `.closedMinutes` / `.coinMinutes` | price refresh intervals | 1–240 / 1–1440 / 1–240 |
| `timezone` | POSIX TZ, for the clock *and* the market window | — |
| `market.open` / `.close` | trading window, `HH:MM`, Mon–Fri | 00:00–23:59 |
| `stocks` / `coins` | the watchlist itself | ≤32 symbols |

Any key may be omitted and the firmware default applies.

**Two settings are deliberately absent.** Wi-Fi credentials stay in
`secrets.h` — see [SECURITY.md](../../../SECURITY.md). Logo size is fixed at
96px because the firmware and `tools/make-logos.py` have to agree on it.

**`timezone` and `market` are coupled on purpose.** Market hours are evaluated
in whatever zone `timezone` names, so pointing it somewhere non-Eastern means
moving the window too. That used to be a buried comment saying "this breaks
silently if you change TZ"; making the window a setting turns a hidden
assumption into an adjustable one — and it means the panel can track a
non-US exchange.

### Bad values are rejected, not clamped

The file is hand-edited, so reading it is a trust boundary. Every numeric
setting goes through one `setting()` helper that range-checks, **keeps the
previous value, and names what it rejected on the serial log.** Silently
clamping a typo to the nearest legal value produces a setting that "doesn't
work" with no explanation; saying so costs one line.

Verified by feeding it a deliberately broken file:

```
setting brightness.open: 999 out of range 8..255, keeping 96
setting brightness.closed: 0 out of range 8..255, keeping 64
setting night.from: 25 out of range 0..23, keeping 23
setting timing.pageSeconds: 0 out of range 2..600, keeping 8
setting timing.soloSeconds: not a number, keeping 5
setting refresh.openMinutes: 99999 out of range 1..240, keeping 5
setting market: close 570 <= open 960, restoring 09:30-16:00
setting layout: 'sideways' is not list or solo, ignored
skipped stock 'TOOLONGNAME' (bad label or list full)
```

Every one of those would otherwise be a plausible-looking failure. A brightness
of 0 reads as a dead board, which is why the floor is 8; an inverted market
window would make `marketOpen()` permanently false and quietly disable both the
open brightness and the 5-minute refresh at once.

`layout` is only a starting point. A 2s hold switches layout and stores that in
NVS, which then **wins over the file** — otherwise a config default would undo
the user's own button press on every boot. The boot log says which is in force:

```
layout: SOLO (file default list, nvs override in effect)
```

## The watchlist is a file on the device, not source code

[`data/watchlist.json`](data/watchlist.json) lives on the board's LittleFS
partition, so **changing symbols does not need a rebuild**:

```sh
# firmware (only when src/ changes)
pio run -e ticker -t upload

# the watchlist (after editing data/watchlist.json)
PLATFORMIO_DATA_DIR=apps/ticker/data pio run -e ticker -t uploadfs
```

`huge_app.csv` already leaves an `0xE0000` (896KB) data partition spare, which
LittleFS formats on first use — no partition change was needed.

The `PLATFORMIO_DATA_DIR` prefix is there because `data_dir` is a `[platformio]`
option and cannot be set per-env. With one app using a filesystem, overriding it
at the call site beats pointing the whole project at one app's data directory.

`addRow()` is a **trust boundary** — that file is hand-edited, so labels longer
than the 5-glyph symbol box are rejected, empty ids are rejected, and the
24-symbol cap is enforced rather than assumed. Rejected entries are named in the
serial log instead of vanishing. If the file is missing or malformed the screen
says so *and prints the uploadfs command*, rather than going blank.

## The API question, tested rather than assumed

Measured from a laptop before writing any firmware:

| Source | Result |
|---|---|
| **Yahoo v8 chart** (`/v8/finance/chart/AAPL?range=1d&interval=1d`) | **Works, no key.** 1298 bytes. `meta.regularMarketPrice` and `meta.chartPreviousClose` are all a percent change needs. **One symbol per request.** |
| **CoinGecko** (`/api/v3/simple/price`) | **Works, no key.** Eight coins in **486 bytes, one request**, with `usd_24h_change` included. |
| Stooq CSV | **Dead.** 404 on every documented URL form. |
| Yahoo v7 `/finance/quote?symbols=` | **401.** Needs a crumb/cookie now, which is not worth doing on an MCU. |

**Yahoo returns HTTP 429 with no User-Agent.** An MCU sends none by default, so
without an explicit header it rate-limits immediately and presents as a broken
app rather than a rejected request. The firmware sets one.

That asymmetry drives the whole polling design: **coins are one cheap request,
stocks are one request per symbol.** So:

- Stock fetches are **sequential on a single reused `NetworkClientSecure`** —
  each TLS session costs ~40KB of a 512KB heap, so they cannot overlap.
- The refresh interval has a **floor of roughly one request per minute**
  (`stockEvery = max(interval, nStocks × 60s)`). Twelve stocks every five
  minutes would be 144 requests/hour and Yahoo starts answering 429. Long lists
  trade freshness for breadth, deliberately and visibly.
- **Polling follows market hours.** 5 min between 09:30 and 16:00 ET on
  weekdays, hourly otherwise; coins keep their own 5-minute cadence because
  crypto never closes. `marketOpen()` is asserted in `selfCheck()`.

## Scrolling: why it pages instead of sliding

The ask was a smooth scroll for a longer list. Three options, in order of cost:

1. **ST7789 hardware vertical scroll** (`VSCRDEF`/`VSCRSAR`) — free, no CPU.
   **Doesn't apply.** It wraps *within* the 320-line frame, so 12 symbols at a
   38px pitch (456px) cannot fit regardless; and Arduino_GFX exposes no scroll
   API, so it would mean raw panel commands. Also scrolls along the native axis,
   which is sideways once the board is rotated to landscape.
2. **Pixel-wise slide** — repaints the entire row region every frame. That is
   172×250px over SPI on one core already down-clocked to 80MHz for heat.
   Expensive, and the thing it buys is decoration.
3. **Paged window with a staggered redraw** — what shipped. Every 8s the page
   flips and the six rows redraw top-to-bottom with a 22ms stagger, so it reads
   as movement rather than a blink. **It costs nothing extra** — the same
   redraws, just spaced out.

The header shows `1/2` so a flip can't be mistaken for prices changing. The row
cache is keyed **per screen slot, not per symbol**, so turning the page
invalidates it — otherwise a slot holding an unchanged string would keep the
previous page's symbol.

## Readability

Six rows at text size 2 is the budget, and it is a layout fact rather than a
preference — `checkLayout()` asserts that both orientations hold exactly one
page, that the symbol box cannot reach the price box, and that the footer does
not land on the last row. Size 1 (6×8px glyphs) is already known to be too small
to read on this panel, which is the complaint that got desk-clock's forecast
enlarged.

Landscape uses two columns of three, so a page is six rows in both orientations.

Solo is the layout most likely to overflow — a 96px logo plus three text sizes
plus a footer — and 172px-tall landscape is the tight case. `checkSolo()`
asserts each element against the panel *and* against the element below it, plus
that the logo is either above the text (portrait) or beside it (landscape).

## What the self-check caught

`selfCheck()` runs before the display initialises and asserts on failure, so a
layout bug is a boot loop with a line number rather than a subtly wrong screen.
Two real finds during this build:

- **A landscape overflow.** Rows ended at y=132 but the footer started at y=130.
  The two asserts together pin the only legal window, 132–140.
- **A bad assertion of mine, not bad code.** `formatPct(2.35f)` was expected to
  give `+2.4%`; it correctly gives `+2.3%`, because `2.35f` is really
  `2.3499999`. Don't assert what a half-way float literal rounds to — the test
  values now sit off the boundary, with a comment saying why.

`formatPrice()` has a 7-character box and shrinks precision as the number grows,
because `0.4215` and `79010` have to fit the same space. Percent change is
**always signed**, so green/red is never the only cue.

## Hard parts, still true

- Delayed data is likely (Yahoo runs ~15 min behind on some exchanges). The
  footer says `delayed, Nm ago` rather than implying live prices.
- Back off hard on 429 and never retry a rate-limit tightly. The request floor
  above is the main defence; the HTTP code is named in the serial log.
- A misspelled symbol returns 200 with no price field. That is logged as
  `no price in payload (bad symbol?)` so it isn't mistaken for a network fault —
  the row just shows `--` in the dim colour.
- `client.setInsecure()` is deliberate: these are public read-only quotes, and
  certificate pinning on a desk device whose flash can be dumped buys nothing.
  See [SECURITY.md](../../../SECURITY.md).

**Pieces:** `NetworkClientSecure`, `ArduinoJson` v7 with filters, `LittleFS`,
`<ui.h>` for schemes/rotation/gestures, `<netjoin.h>` for the BSSID-pinned join.
