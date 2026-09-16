# ticker — **built** (list + detail + logos)

Built 2026-09-15, first version: the LIST layout with a sparkline on every
row, tap a row for the stock's own page (chart with previous-close line, day
and 52-week range bars, PREV / LIST / NEXT buttons), fetches on core 0,
brightness from the LDR. 2026-09-16: logos at two sizes from LittleFS; the
list became a **continuously scrolling ring** (hardware scroll, seven rows,
no pages, no footer); a range selector (1D / 5D / 1M / 6M / 1Y) under the
detail chart, prefetched so a tap is instant; **market sessions** (nothing is
fetched while the market is closed, the header says CLOSED and when it
opens, the detail page shows the after-hours price); an INFO page behind the
circled i in the header. **Not built yet:** the SD card, the HEATMAP layout,
and the NVS layout memory -- the design below is the roadmap for those.

## The list scrolls, it doesn't page

The ILI9341 scrolls in hardware: `VSCRDEF` (0x33) fences the seven row slots
between the fixed 22px header and the bottom edge, `VSCRSADD` (0x37) says
which line of that region shows at the top, and the region wraps. Moving the
whole list one pixel is one two-byte command, so the crawl costs nothing to
draw. `timing.pageSeconds` is now how long one screen (seven rows) takes to
pass; 10 reads well.

The catch is that the slot scrolling off the top **is** the slot the next row
enters from the bottom, a line at a time. So the incoming row is painted
off-screen into a one-row `Arduino_Canvas` (20KB) and its lines are fed into
the slot as they come into view. Rows fully in the ring update in place when
their price changes; the two rows in transit wait until the transit is over.
The scroll freezes while a finger is down, and `hitRow()` maps a tap through
the current offset, so a tap lands on the row it was aimed at. A list shorter
than seven simply repeats. The board's `boardBus()` exists so an app can send
the two panel commands Arduino_GFX has no API for.

## Market sessions

The trading day has four parts from the clock and `market.*`: pre-market
(04:00), regular (09:30-16:00), after hours (to 20:00), closed. Fetch policy
follows it: `refresh.openMinutes` in the regular session,
`refresh.extendedMinutes` in pre/post, and when closed **one sweep after the
close** to keep the final after-hours prints, then nothing until the next
pre-market. Crypto never closes and keeps its own interval. Tapping a row
while closed does not refetch it either. A holiday looks like a weekday and
costs a handful of unchanged fetches; not worth a calendar.

Quotes are requested with `includePrePost=true`, so the 1D series runs
04:00-20:00 and the row keeps its newest bar as `last`. The list always shows
the regular-session figures. The detail page, outside the regular session,
shows `last` as the price with its move measured from the regular close and
says "after hours" / "pre-market" where the company name goes. The header
says nothing while the market is open ("market open" is not news) and
`CLOSED til Mon 09:30` when it is.

```sh
python3 tools/make-logos.py --size 24 --out data/logo/24   # once, on the Mac
python3 tools/make-logos.py --size 96 --out data/logo/96
pio run -e ticker -t upload -t monitor
./push-config ticker                                        # config + logos
```

Verified 2026-09-16: `logos 24px: 26/26 present`, `logos 96px: 26/26 present`
on the boot log, ~162KB heap free after the first coin fetch.


<img src="preview.svg" alt="ticker list layout with logo badges and sparklines" width="240">
<img src="preview-detail.svg" alt="ticker detail page for one symbol" width="240">


The [C6 ticker](../../../esp32-c6-lcd-1.47/apps/ticker/), ported to a screen with
40% more width and a finger on it. Same watchlist, same keyless APIs, same
one-palette rule, **same logos** — but every row carries a logo badge and a
sparkline, there's a twelve-symbol heatmap the C6 can't hold, and **tapping a
stock opens that stock's own page** instead of cycling blind through the list
with one button.

The port is worth doing because four separate C6 compromises were all forced by
that board, and none of them apply here.

## The sparkline is already paid for

This is the headline, and it costs **nothing**.

The C6 calls Yahoo's v8 chart endpoint with `range=1d&interval=1d`, reads
`meta.regularMarketPrice` and `meta.chartPreviousClose`, and throws the rest
away. Ask the *same endpoint*, at the *same one request per symbol*, for
`range=1d&interval=5m` and the response carries **roughly 78 intraday closes**
in `indicators.quote[0].close`. Same rate-limit cost, same request count, same
TLS session — the C6 just had no width to draw a chart in and never asked for
the series.

240px does have the width: symbol, a 70px sparkline, price and change all fit one
42px row, six rows to a page.

**The payload grows about tenfold**, from ~1.3KB to ~10–15KB, and that is the one
change that can take the heap down. Parse it with an **ArduinoJson streaming
filter** over `indicators.quote[0].close` and `meta` only, straight into a
`float[78]` — 312 bytes retained per symbol, and the document never lives in RAM
whole. Don't `deserializeJson()` the response and then pick fields out of it;
that's the version that runs fine on twelve symbols and dies on twenty-six.

## Four C6 compromises that don't apply here

| C6 limit | What it forced | On this board |
|---|---|---|
| 172px wide | Six rows of symbol/price/change, no room for anything else | A sparkline column, and a 3×4 heatmap |
| One button, no touch | Blind cycling: hold 2s for the next layout, no way to pick a symbol | **Tap the row you want** |
| Single core | 22 blocking requests froze `loop()` for ~26s; fixed by fetching one per pass | Fetch task on core 0, UI on core 1 |
| 896KB data partition | 26 logos at 96px = 479KB, **52% of the partition** | SD card on its own bus: 128px logos, 32 of them, ~1MB, nobody cares |

**Dual-core is a real fix, not a bigger workaround.** The C6's one-request-per-pass
design exists so the clock keeps ticking and a button press doesn't land half a
minute late. Put the fetch loop in its own task pinned to core 0 and the UI never
stalls at all. But **do not let two cores tempt you into parallel TLS** — each
`NetworkClientSecure` session is ~40KB of a 520KB heap and this board has no
PSRAM either, so fetches stay strictly sequential. The rate floor stays too:
`stockEvery = max(interval, nStocks × 60s)`, because Yahoo's 429 doesn't care how
many cores you have.

**Touch earns its keep here specifically.** Resistive is bad at flicks and
useless for pinch, which is why `touch-dashboard` and `sd-file-browser` both
design around it — but "tap one of six 240×42 rows" is exactly what it's good at.
Tap a row for the detail view, tap the header to change layout, tap anywhere to
come back. That replaces a one-button gesture vocabulary that had to spend its
colour-scheme gesture on layout switching just to fit.

## Three layouts

| Layout | What it shows |
|---|---|
| **LIST** | Six rows: symbol, intraday sparkline, price, signed change. Longer lists page every 8s. |
| **HEATMAP** | 3×4 grid of 76×64 tiles, each tinted by the size of its move. Twelve symbols at once — **the layout the C6 physically cannot hold.** |
| **DETAIL** | One stock's own page — logo, chart, ranges. Reached by **tapping it**, not by waiting for it to come round. |

Heatmap tint is **intensity, not just sign** — a 0.2% drift and a 9% drop must
not look the same. Bucket the magnitude and keep the signed percentage printed on
every tile, because colour is never allowed to be the only cue (the C6 build
settled that already).

## Logos, at two sizes

Two sizes, **both prepared on the host** and shipped raw on LittleFS:

| File | Size | Used by |
|---|---|---|
| `/logo/96/NVDA.565` | 18KB | The stock's own page |
| `/logo/24/NVDA.565` | 1.1KB | The badge on each list row |

**96 on the detail page, not the 128 this section first planned.** 128px is
32KB a logo; 26 of them plus the badges overflows the 896KB LittleFS partition
once LittleFS's 4KB blocks are counted. 96 is the C6's size, the whole set at
both sizes is 26 × (18KB + 1.1KB) ≈ 510KB (628KB on disk with block rounding),
and it needed no SD card driver. `// ponytail:` in `main.cpp` marks it: when
the SD card lands, the ceiling goes and 128 is one constant and one rerun.

`tools/make-logos.py` is the C6's converter reached by symlink, with an `--out`
so one script writes both sizes into this app's `data/`. It now **crops each
logo to its visible mark** before scaling, so every badge fills 88% of its box
and the detail logo sits flush with the text beside it -- sources arrive with
anything from no padding to half the image, and at 24px that difference was
the whole picture. The crop follows what will be visible on black, not the
alpha channel: many sources carry an opaque black backing square that is
"glyph" by alpha and invisible on screen. The firmware reads
`/logo/<size>/<LABEL>.565` into one static 18KB buffer and blits it; the boot
log inventories both sizes (`logos 24px: 26/26 present`).

**Two files rather than scaling on the device.** Nearest-neighbour from 128 to 24
turns a logo into a smear, and anything better costs code plus a buffer on a
board with no PSRAM. `tools/make-logos.py` already takes a size — run it twice.
The decision stays on the Mac, exactly as the colour handling does.

**The row badge is recognition, not identification.** The symbol text stays right
next to it. That matters because plenty of logos are wordmarks, and a wordmark at
24px is an unreadable smudge — but you aren't reading it, you're recognising it,
and the text carries the actual identity. It also makes a missing small logo a
non-event: the row just has no badge.

Everything else about logos is inherited unchanged, and the
[C6 logo section](../../../esp32-c6-lcd-1.47/apps/ticker/#logos) is required
reading before touching the converter — the three background cases, and the fact
that "is this pixel background?" and "does this mark need inverting?" are
*separate questions* that got conflated three times. The firmware still carries
no PNG decoder: it opens the file, checks the length is exactly `w × h × 2`,
blits it, and falls back to large text otherwise.

One extension to the converter, and it's the reason the two sizes are worth
verifying separately: **run the percent-visible check at both sizes.** A mark
that passes comfortably at 128px can fall under the 6% threshold at 24px — and
that is precisely the signal that this symbol should be text-only in the row.

## Tap a stock, get its page

**Range selector, built 2026-09-16.** Five 44×24 chips under the chart: 1D is
the row's own sparkline series; 5D (30m), 1M (1d), 6M (1d) and 1Y (1wk) are
fetched by the core-0 task **the moment the page opens**, selected range
first, into one buffer per range (130 closes each), so a chip tap is instant
once they have landed and the chart says "loading 1M..." until then. The
previous-close line follows the range -- Yahoo's `chartPreviousClose` is the
close before the range start. The selection sticks across PREV / NEXT. Coins
have no series and show no chips. High and low are printed inside the chart
box so the chips get the full width. Taps fire at touch-down, not release --
release made every button feel a beat late.

**INFO page.** The circled i in the list header (or anywhere on the header)
opens a page with what the footer used to carry: session and next open,
how stale the prices are, refresh intervals, wifi and RSSI, heap, uptime,
LDR reading and backlight level, symbol and logo counts, build date. Any
tap returns; so does `detail.returnSeconds`.

Tap a LIST row or a HEATMAP tile — same gesture, same result in both layouts, no
modes to learn. **The page is free**, for the same reason the sparkline is: the
response already being fetched carries far more in `meta` than the C6 ever reads
out of it —

`longName`, `regularMarketDayHigh` / `Low`, `fiftyTwoWeekHigh` / `Low`,
`regularMarketVolume`, `exchangeName`, `currency`.

The C6 takes two fields from that object and discards the rest. **Confirm the
exact set with one `curl` before designing around it**, the same way the C6
settled its API questions by measurement rather than assumption.

What goes on the page:

- **128px logo**, company name, large price, and the change as *both* absolute
  and percent.
- **The full intraday chart** at 240×110 — the sparkline's data, given room to
  be a chart, with axis labels.
- **The previous close as a dashed reference line.** Not decoration: the percent
  change is measured from it, so without that line the chart's shape doesn't
  actually mean anything.
- **Day range and 52-week range as bars**, each with a marker showing where the
  current price sits. That's the use of the extra width that a phone app would
  spend on a table.

Three behaviours that make it feel like a device rather than a screen:

- **Navigate with explicit buttons** — `‹ PREV | LIST | NEXT ›` across the
  bottom. Not tap-zones. Resistive touch wants an obvious chunky target, and
  this is a desk device where precision is affordable — the opposite call from
  [bike-buddy](../bike-buddy/), where the whole screen is one target because you
  are on rough ground.
- **Bump the tapped symbol to the front of the fetch queue.** If it's five
  minutes stale, you want it fresh *now*. That's a priority in the queue that
  already exists, not a new request path, and it still obeys the rate floor.
- **Auto-return to the list** after ~60s, configurable, 0 to disable. A panel
  parked on one stock is showing you less than it could.

## Settings

Everything tunable lives in [`data/config.json`](data/config.json) on the
device's filesystem, read through the shared
[`appcfg.h`](../../../esp32-c6-lcd-1.47/lib/board/appcfg.h) loader — the same
convention as every app on the C6, so a change applies without a rebuild and the
file documents itself in `_`-prefixed blocks.

| Key | What | Range |
|---|---|---|
| `brightness.auto` | Track the LDR instead of the clock | bool |
| `brightness.min` / `.max` | Bounds the LDR maps between | 8–255 |
| `brightness.closedScale` | Dim further while the market is shut | 10–100 % |
| `layout` | Which layout to start in | `list` / `heatmap` |
| `timing.pageSeconds` | How long one screen of rows takes to scroll past | 2–600 |
| `sparkline.interval` | Yahoo granularity, and so the payload size | `5m` / `15m` / `30m` |
| `detail.returnSeconds` | Auto-return to the list; 0 = never | 0–3600 |
| `detail.refreshOnOpen` | Jump the tapped symbol up the fetch queue | bool |
| `logos.rowBadges` | 24px badges on list rows | bool |
| `refresh.openMinutes` / `.extendedMinutes` / `.coinMinutes` | Price refresh in the regular session / pre and post / crypto | 1–240 / 1–1440 / 1–240 |
| `timezone` | POSIX TZ, for the clock *and* the market window | — |
| `market.pre` / `.open` / `.close` / `.post` | The trading day, `HH:MM`, Mon–Fri | pre ≤ open < close ≤ post |
| `stocks` / `coins` | The watchlist itself | ≤32 symbols |

**The C6's `night.from` / `night.to` are gone, deliberately.** That pair exists
only because the C6 has no light sensor and has to infer darkness from the clock
— which is also how it ended up with a bug where night dimming only applied if
someone happened to press a button after 23:00. This board has an LDR, so
`brightness.auto` measures the room directly and two settings disappear. Market
state still scales the result, because "nothing is changing" is a different
reason to dim than "the room is dark".

Two rules carried over verbatim, both load-bearing:

- **Wi-Fi credentials are not in this file.** They stay in `secrets.h`, because
  `config.json` is plain text on a filesystem anyone holding the board can dump.
  See [SECURITY.md](../../../SECURITY.md).
- **Bad values are rejected and named on the serial log, never clamped.**
  `appcfg.h` does this for you — `cfgInt()` keeps the caller's existing value and
  prints what it threw out. Silently clamping a typo produces a setting that
  "doesn't work" with no explanation.

Logo size is **not** a setting, at either size: the firmware and
`tools/make-logos.py` have to agree on it.

## Inherit these, don't rediscover them

The C6 build paid for all of this. The port should copy the conclusions:

- **Set a User-Agent.** Yahoo returns 429 to a request without one, and an MCU
  sends none by default. This presents as a broken app rather than a rejected
  request, and it cost real debugging time once already.
- **Coins are one cheap CoinGecko request**, stocks are one Yahoo request each.
  That asymmetry drives the whole polling design.
- **Poll on market hours**, and not at all while the screen is off.
- **`config.json` is a trust boundary**: range-check every numeric, keep the
  previous value, and *name what was rejected* on the serial log. Clamping a typo
  silently produces a setting that "doesn't work" with no explanation.
- **NVS layout choice beats the file default**, or a config default undoes the
  user's own tap on every boot. Say which is in force in the boot log.
- **Logos are converted on the host** and the firmware carries no PNG decoder —
  see [Logos](#logos-at-two-sizes-because-the-ceiling-is-gone) above, and the C6's
  own section before touching the converter.
- **`setInsecure()` is deliberate** for public read-only quotes. See
  [SECURITY.md](../../../SECURITY.md).
- **Assert the geometry in `selfCheck()` before the display initialises**, so a
  layout bug is a boot loop with a line number instead of a subtly wrong screen.
  That caught a real landscape overflow on the C6.

**Hard parts**

- **The close array contains `null`.** Gaps, halts and the pre-open padding all
  come back as nulls, and parsing those into `0.0` drops your sparkline to the
  axis — a chart that looks plausible and is wrong, which is the exact failure
  class this repo has been bitten by before. Skip them, carry the last good
  value, and break the line across a gap rather than drawing through it.
- **Scale each sparkline to its own min/max.** A shared vertical scale flattens a
  $12 stock into a straight line next to a $600 one. Per-row autoscale, no axis
  labels — it's a shape, not a chart. The detail view is where numbers go.
- **A flat line and a missing line must look different.** A symbol that hasn't
  fetched yet, and one that genuinely didn't move, both draw as a horizontal
  line. Draw "not yet" as the dim dashed placeholder the preview shows.
- **Touch shares SPI with the display**; the card does not. Don't sample the
  XPT2046 mid-blit — the community repo covers the sequencing.
- **Resistive touch needs calibration** stored in NVS (four-corner tap), same as
  `touch-dashboard`. Raw values drift per unit and with temperature.
- **Cache rows per screen slot, not per symbol.** Inherited from the C6 and still
  true, and now it applies to heatmap tiles as well — a slot holding an unchanged
  string would otherwise keep the previous page's symbol.
- **A page flip between touch-down and touch-up opens the wrong stock.** The list
  pages every 8s and the slot-to-symbol map turns over with it, so a tap that
  lands either side of a flip resolves to a symbol the user never touched.
  Resolve the symbol **at touch-down**, and freeze the page timer while a touch
  is held. This is the same bug as the row cache wearing a different hat: **the
  slot is not the symbol.**
- **The pinout is unverified.** This board is still wishlist status, so
  `lib/board/board.h` comes first and the app second.

**Pieces:** `NetworkClientSecure`, `ArduinoJson` v7 **with a streaming filter**
(not optional here), `SdFat` for logos, `XPT2046_Touchscreen`, TFT_eSPI, a
board-local `ui.h` in the C6's shape, and the C6's `tools/make-logos.py` with a
size argument.

**Effort:** medium. The API questions are all answered, the polling design is
proven, and the logo pipeline exists — this is a port plus a board layer plus
touch, not a new problem. The genuinely new code is the series parse, the
sparkline, and the heatmap. Build LIST first; it's the layout that has to be
right, and the other two are additive.
