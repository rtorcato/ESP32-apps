# ticker — idea

<img src="preview.svg" alt="ticker preview" width="172">


A vertical list of watchlist symbols: ticker, last price, percent change, green
or red. Scrolls slowly if the list is longer than the screen.

**Why this board:** a 172×320 portrait panel is *the* right shape for a ranked
list — roughly 10–12 readable rows with no wasted space. This is the app the
display was accidentally designed for.

**Hard parts**

- Free quote APIs are the whole problem. Most need a key, rate-limit hard, or
  quietly serve delayed data. Decide up front whether delayed is fine (it
  usually is for a desk display) and cache aggressively.
- TLS on 512KB of SRAM: each HTTPS connection costs a chunk of heap. Use one
  reused `WiFiClientSecure`, fetch all symbols in a single batched request if the
  API allows, and skip certificate pinning unless it matters.
- Don't repaint the whole list per update — per-row dirty rects, or the scroll
  stutters.

**Pieces:** `WiFiClientSecure`, `ArduinoJson` (use a filter to skip fields —
whole-response parsing will exhaust heap), Arduino_GFX.

**Effort:** small once the data source is settled; picking the API is the
decision, not the code.
