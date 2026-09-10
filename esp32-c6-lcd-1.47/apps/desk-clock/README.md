# desk-clock — idea

<img src="preview.svg" alt="desk-clock preview" width="172">


Big clock on top, today's weather stacked beneath it, date at the bottom. The
default thing to build on a small always-on display, and the 172×320 portrait
shape suits a vertical stack of three blocks.

**Why this board:** always USB-powered, so no battery/sleep design needed. Wi-Fi
for NTP is already there. The RGB LED gives a free ambient channel — tint it by
temperature or turn it red when rain starts.

**Hard parts**

- Redrawing a large font every second flickers. Draw digits into small dirty
  rects and only repaint the ones that changed; don't `fillScreen()` in `loop()`.
- Single core: a blocking `HTTPClient` weather fetch freezes the clock. Fetch on
  a timer (every 10–15 min) from a task, or accept a visible hitch.
- Timezone and DST: use `configTzTime()` with a real TZ string, not a raw UTC
  offset, or you'll be re-flashing it twice a year.

**Pieces:** `WiFi`, `configTzTime`, [open-meteo](https://open-meteo.com) (no API
key), `ArduinoJson`, Arduino_GFX free fonts.

**Effort:** small. Good first real app — mostly layout work.
