# ticker — idea

<img src="preview.svg" alt="ticker preview" width="172">


A vertical list of watchlist symbols: ticker, last price, percent change, green
or red. Scrolls slowly if the list is longer than the screen.

**Why this board:** a 172×320 portrait panel is *the* right shape for a ranked
list — roughly 10–12 readable rows with no wasted space. This is the app the
display was accidentally designed for.

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
app rather than a rejected request. Set one.

So the shape is fixed by the APIs: **crypto is one cheap request, stocks are one
request per symbol.** Eight stocks means eight TLS handshakes at roughly 40KB of
heap each, so they must run sequentially on a reused client, not concurrently.
At 1–2s each that is 10–15s for a full refresh — fine for a panel, but it blocks
the single core, so the clock in the corner will visibly jump.

**Poll on market hours.** The board already has NTP and a real TZ, so there is no
excuse for hitting a closed market every five minutes: refresh every 5 min
between 09:30 and 16:00 ET on weekdays, hourly otherwise, and treat crypto
separately since it never closes. That also keeps a long way clear of the 429.

## Readability comes first

The preview below shows eleven rows at size 1, and that is now known to be too
small to read on this panel — the same complaint that got desk-clock's forecast
enlarged. So the design is **six to eight symbols at size 2**, not a dense list:

- symbol at size 2 on the left (4 chars, 48px)
- price at size 2 right-aligned (72px), coloured green or red
- percent change at size 1 beneath, or the price colour alone

That fits 172px with margins and gives ~8 rows of 30px. Fewer symbols, legible.
Pick the eight that matter rather than paging — a list is the natural form for a
ticker, and paging was already rejected for desk-clock.

**Hard parts**

- Delayed data is likely (Yahoo is ~15 min behind on some exchanges). Show a
  "delayed" marker rather than implying live prices; the preview already does.
- Back off hard on 429, and never retry a rate-limit tightly.
- TLS on 512KB of SRAM: each HTTPS connection costs a chunk of heap. Use one
  reused `WiFiClientSecure`, fetch all symbols in a single batched request if the
  API allows, and skip certificate pinning unless it matters.
- Don't repaint the whole list per update — per-row dirty rects, or the scroll
  stutters.

**Pieces:** `WiFiClientSecure`, `ArduinoJson` (use a filter to skip fields —
whole-response parsing will exhaust heap), Arduino_GFX.

**Effort:** small once the data source is settled; picking the API is the
decision, not the code.
