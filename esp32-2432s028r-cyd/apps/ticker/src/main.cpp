// ticker on the CYD: the C6 watchlist with the width to draw each stock's
// intraday series beside its price, and a finger to open a stock's own page.
//
// Everything the C6 build measured still holds (README): Yahoo's v8 chart is
// one request per symbol and 429s without a User-Agent; CoinGecko is one
// request for every coin. New here: the same Yahoo request at interval=5m
// carries ~79 closes, so the sparkline and the detail page are free.
//
// Two cores: fetching runs in its own task pinned to core 0, so a TLS
// handshake never stalls touch or the clock. Fetches stay strictly sequential
// -- each TLS session is ~40KB of a 320KB heap with no PSRAM behind it.
#include <appcfg.h>
#include <board.h>
#include <netjoin.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── one colour scheme: black, white symbols, green and red numbers ───────
static const uint16_t C_BG = RGB565_BLACK, C_FG = RGB565_WHITE, C_GOOD = 0x07E0, C_BAD = 0xF800,
                      C_DIM = 0x630C, C_MUTED = 0xA534, C_RULE = 0x2104, C_WARN = RGB565_YELLOW;

// ── settings (defaults; data/config.json overrides) ──────────────────────
static char tzString[64] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static uint16_t mktOpenMin = 9 * 60 + 30, mktCloseMin = 16 * 60;
static uint32_t pageMs = 8000;
static uint16_t cascadeMs = 22;
static bool blAuto = true;
static uint8_t blMin = 20, blMax = 220, blClosedScale = 60;
static uint16_t ldrDark = 3000;  // raw LDR reading that counts as a dark room
static char sparkInterval[4] = "5m";
static uint32_t returnMs = 60000;
static bool refreshOnOpen = true;
static uint32_t stockOpenMs = 5UL * 60 * 1000, stockShutMs = 60UL * 60 * 1000, coinMs = 5UL * 60 * 1000;
static const uint32_t WIFI_RETRY_MS = 20UL * 1000, LOG_MS = 60UL * 1000;
static const char *UA = "Mozilla/5.0 (esp32-ticker)";

static const uint8_t ROWS = 6, MAX_SYMBOLS = 32, MAX_LABEL = 5, SPARK_N = 80;
// Two logo sizes, both pre-converted on the Mac (tools/make-logos.py --size N
// --out data/logo/N) and read raw from /logo/<N>/<LABEL>.565. Must match.
// ponytail: 96 on the detail page, not the README's 128 -- 26 x 32KB plus
// badges overflows the 896KB LittleFS partition. 128 when the SD card lands.
static const uint8_t LOGO_BADGE = 24, LOGO_BIG = 96;
#define GW(s) (6 * (s))
#define GH(s) (8 * (s))

static Arduino_GFX *gfx;

// ── the watchlist ────────────────────────────────────────────────────────
struct Row {
  char label[MAX_LABEL + 1];
  char id[28];
  char name[32];
  bool coin, valid;
  float price, pct, prev, dayLo, dayHi, wkLo, wkHi;
  uint8_t n;
  float close[SPARK_N];
};
static Row rows[MAX_SYMBOLS];
static uint8_t nRows = 0, nStocks = 0, nCoins = 0, page = 0, nPages = 1;
static char coinIds[MAX_SYMBOLS * 28];
static const char *cfgErr = nullptr;

// rows[] is written by the fetch task and read by the UI: every access to a
// row goes through a copy taken under this mutex.
static SemaphoreHandle_t mux;
static Row rowCopy(uint8_t i) {
  Row r;
  xSemaphoreTake(mux, portMAX_DELAY);
  r = rows[i];
  xSemaphoreGive(mux);
  return r;
}
static void rowStore(uint8_t i, const Row &r) {
  xSemaphoreTake(mux, portMAX_DELAY);
  rows[i] = r;
  xSemaphoreGive(mux);
}
static volatile int8_t priority = -1;  // symbol to fetch next, ahead of the sweep
static volatile uint32_t lastOk = 0, lastStock = 0, lastCoin = 0;
static volatile uint16_t failures = 0;

static bool addRow(const char *label, const char *id, bool coin) {
  if (nRows >= MAX_SYMBOLS) return false;
  if (!label || !*label || strlen(label) > MAX_LABEL) return false;
  if (!id || !*id) return false;
  Row &r = rows[nRows++];
  memset(&r, 0, sizeof r);
  snprintf(r.label, sizeof r.label, "%s", label);
  snprintf(r.id, sizeof r.id, "%s", id);
  r.coin = coin;
  return true;
}

// ticker's config is also its data: no watchlist, nothing to show.
static bool loadConfig() {
  if (!cfgLoad()) {
    cfgErr = cfgError();
    return false;
  }
  for (JsonVariant v : cfgArr("stocks")) {
    const char *s = v.as<const char *>();
    if (addRow(s, s, false)) nStocks++;
    else Serial.printf("skipped stock '%s' (bad label or list full)\n", s ? s : "?");
  }
  for (JsonObject o : cfgArr("coins")) {
    const char *id = o["id"], *label = o["label"];
    if (addRow(label ? label : id, id, true)) nCoins++;
    else Serial.printf("skipped coin '%s' (bad label or list full)\n", id ? id : "?");
  }
  if (nRows == 0) {
    cfgErr = "config.json has no symbols";
    return false;
  }

  blAuto = cfgBool("brightness.auto", blAuto);
  blMin = (uint8_t)cfgInt("brightness.min", blMin, 8, 255);
  blMax = (uint8_t)cfgInt("brightness.max", blMax, 8, 255);
  if (blMax < blMin) blMax = blMin;
  blClosedScale = (uint8_t)cfgInt("brightness.closedScale", blClosedScale, 10, 100);
  ldrDark = (uint16_t)cfgInt("brightness.ldrDark", ldrDark, 100, 4095);
  pageMs = (uint32_t)cfgInt("timing.pageSeconds", pageMs / 1000, 2, 600) * 1000UL;
  cascadeMs = (uint16_t)cfgInt("timing.cascadeMs", cascadeMs, 0, 400);
  char iv[4] = "";
  if (cfgStr("sparkline.interval", iv, sizeof iv)) {
    if (!strcmp(iv, "5m") || !strcmp(iv, "15m") || !strcmp(iv, "30m")) strcpy(sparkInterval, iv);
    else Serial.printf("config sparkline.interval: '%s' is not 5m/15m/30m, ignored\n", iv);
  }
  returnMs = (uint32_t)cfgInt("detail.returnSeconds", returnMs / 1000, 0, 3600) * 1000UL;
  refreshOnOpen = cfgBool("detail.refreshOnOpen", refreshOnOpen);
  stockOpenMs = (uint32_t)cfgInt("refresh.openMinutes", stockOpenMs / 60000, 1, 240) * 60000UL;
  stockShutMs = (uint32_t)cfgInt("refresh.closedMinutes", stockShutMs / 60000, 1, 1440) * 60000UL;
  coinMs = (uint32_t)cfgInt("refresh.coinMinutes", coinMs / 60000, 1, 240) * 60000UL;
  cfgStr("timezone", tzString, sizeof tzString);
  cfgHhMm("market.open", &mktOpenMin);
  cfgHhMm("market.close", &mktCloseMin);
  if (mktCloseMin <= mktOpenMin) {
    Serial.printf("config market: close %u <= open %u, restoring 09:30-16:00\n", mktCloseMin, mktOpenMin);
    mktOpenMin = 9 * 60 + 30;
    mktCloseMin = 16 * 60;
  }
  Serial.printf("settings: bl %s %u-%u closed %u%% ldrDark %u  page %lus  spark %s  return %lus\n",
                blAuto ? "auto" : "fixed", blMin, blMax, blClosedScale, ldrDark,
                (unsigned long)(pageMs / 1000), sparkInterval, (unsigned long)(returnMs / 1000));
  Serial.printf("settings: refresh %lu/%lu/%lu min  market %02u:%02u-%02u:%02u  tz %s\n",
                (unsigned long)(stockOpenMs / 60000), (unsigned long)(stockShutMs / 60000),
                (unsigned long)(coinMs / 60000), mktOpenMin / 60, mktOpenMin % 60, mktCloseMin / 60,
                mktCloseMin % 60, tzString);

  coinIds[0] = '\0';
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    if (coinIds[0]) strlcat(coinIds, ",", sizeof coinIds);
    strlcat(coinIds, rows[i].id, sizeof coinIds);
  }
  nPages = (nRows + ROWS - 1) / ROWS;
  Serial.printf("watchlist: %u stocks + %u coins over %u page(s)\n", nStocks, nCoins, nPages);
  return true;
}

// ── layout (portrait 240x320, fixed) ─────────────────────────────────────
// List: six 42px rows. Badge | symbol | sparkline | price over percent.
static const int16_t Y_HEAD = 4, Y_ROW0 = 22, ROW_H = 42, X_SYM = 6, Y_BADGE = 7, X_LBL = 32,
                     X_SPK = 96, SPK_W = 50, SPK_H = 28, X_RIGHT = 234, Y_FOOT = 282, FOOT_STEP = 12;
// Detail: 96px logo at the left with symbol, name, price, change beside it;
// then chart, two range bars, three buttons.
static const int16_t D_X_TXT = 108, D_Y_SYM = 6, D_Y_NAME = 34, D_Y_PRICE = 46, D_Y_CHG = 74, D_Y_PCT = 92,
                     D_Y_HI = 112, CH_X = 6, CH_Y = 122, CH_W = 228, CH_H = 78, D_Y_LO = 202, D_Y_DAY = 218,
                     D_Y_WK = 242, BAR_X = 40, BAR_W = 194, BAR_H = 6, D_Y_BTN = 282, BTN_H = 34,
                     BTN_W = 76;

// ── pure helpers (what selfCheck covers) ─────────────────────────────────
static void formatPrice(float v, char *out, size_t n) {
  float a = fabsf(v);
  if (a < 1.0f) snprintf(out, n, "%.4f", v);
  else if (a < 100.0f) snprintf(out, n, "%.2f", v);
  else if (a < 10000.0f) snprintf(out, n, "%.1f", v);
  else if (a < 1000000.0f) snprintf(out, n, "%.0f", v);
  else snprintf(out, n, "%.0fk", v / 1000.0f);
}
static void formatPct(float p, char *out, size_t n) {
  if (fabsf(p) >= 100.0f) snprintf(out, n, "%+.0f%%", p);
  else snprintf(out, n, "%+.1f%%", p);
}
static bool marketOpen(const struct tm &t) {
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;
  int mins = t.tm_hour * 60 + t.tm_min;
  return mins >= mktOpenMin && mins < mktCloseMin;
}
// Which list slot a tap at y lands in, or -1.
static int8_t hitSlot(int16_t y) {
  if (y < Y_ROW0 || y >= Y_ROW0 + ROW_H * ROWS) return -1;
  return (y - Y_ROW0) / ROW_H;
}
// Which detail button, or -1. Hit zones are 80px, wider than the drawn 76.
static int8_t hitButton(int16_t x, int16_t y) {
  if (y < D_Y_BTN) return -1;
  int8_t b = x / 80;
  return b > 2 ? 2 : b;
}
// Value to pixel row inside a box; a flat series sits mid-box, not on the floor.
static int16_t sparkY(float v, float lo, float hi, int16_t y0, int16_t h) {
  if (hi <= lo) return y0 + h / 2;
  float f = (v - lo) / (hi - lo);
  return y0 + (int16_t)lroundf((1.0f - f) * (h - 1));
}

// ── drawing ──────────────────────────────────────────────────────────────
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, GW(size) * chars, GH(size), C_BG);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y);
  gfx->print(s);
}
static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, w, GH(size), C_BG);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(right - (int16_t)(GW(size) * strlen(s)), y);
  gfx->print(s);
}
static void fieldCentre(int16_t cx, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(cx - w / 2, y, w, GH(size), C_BG);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(cx - GW(size) * (int16_t)strlen(s) / 2, y);
  gfx->print(s);
}

// Logos are raw RGB565 files on LittleFS: open, check the length is exactly
// size*size*2, blit. No decoder, no fetch. False when the file is missing or
// the wrong size, and the caller draws nothing there -- the symbol text next
// to it carries the identity. One static buffer serves both sizes.
static uint16_t logoBuf[LOGO_BIG * LOGO_BIG];
static bool blitLogo(int16_t x, int16_t y, uint8_t size, const char *label) {
  char path[40];
  snprintf(path, sizeof path, "/logo/%u/%s.565", size, label);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t want = (size_t)size * size * 2;
  bool ok = f.size() == want && f.read((uint8_t *)logoBuf, want) == want;
  f.close();
  if (!ok) {
    Serial.printf("%s: bad size, rerun tools/make-logos.py --size %u\n", path, size);
    return false;
  }
  gfx->draw16bitRGBBitmap(x, y, logoBuf, size, size);
  return true;
}

// Inventory at boot so a missing set is named on the log, not discovered row
// by row. A missing logo is an ordinary case, not an error.
static void logoInventory() {
  for (uint8_t size : {LOGO_BADGE, LOGO_BIG}) {
    uint8_t have = 0;
    for (uint8_t i = 0; i < nRows; i++) {
      char path[40];
      snprintf(path, sizeof path, "/logo/%u/%s.565", size, rows[i].label);
      File f = LittleFS.open(path, "r");
      if (f && f.size() == (size_t)size * size * 2) have++;
    }
    Serial.printf("logos %upx: %u/%u present%s\n", size, have, nRows,
                  have ? "" : "  (python3 tools/make-logos.py --size N --out data/logo/N; ./push-config ticker)");
  }
}

// The series as a shape: scaled to its own min/max, no axis. "Not yet" is a
// dashed dim line so it cannot be mistaken for a flat day. Coins have no
// series (CoinGecko simple/price carries none) and leave the column empty.
static void drawSpark(int16_t x, int16_t y, int16_t w, int16_t h, const Row &r) {
  gfx->fillRect(x, y, w, h, C_BG);
  if (r.coin) return;
  if (!r.valid || r.n < 2) {
    for (int16_t i = 0; i < w; i += 6) gfx->drawFastHLine(x + i, y + h / 2, 3, C_DIM);
    return;
  }
  float lo = r.close[0], hi = r.close[0];
  for (uint8_t i = 1; i < r.n; i++) {
    lo = min(lo, r.close[i]);
    hi = max(hi, r.close[i]);
  }
  uint16_t c = r.pct >= 0 ? C_GOOD : C_BAD;
  int16_t px = x, py = sparkY(r.close[0], lo, hi, y, h);
  for (uint8_t i = 1; i < r.n; i++) {
    int16_t nx = x + (int32_t)i * (w - 1) / (r.n - 1);
    int16_t ny = sparkY(r.close[i], lo, hi, y, h);
    gfx->drawLine(px, py, nx, ny, c);
    px = nx;
    py = ny;
  }
}

// Cached per SLOT on screen, not per symbol: a page flip changes which symbol
// occupies a slot, so the cache is invalidated when the page turns.
static char cRow[ROWS][48], cHead[20], cFoot[3][40];
static void invalidateCache() {
  for (uint8_t i = 0; i < ROWS; i++) cRow[i][0] = '\0';
  cHead[0] = '\0';
  for (uint8_t i = 0; i < 3; i++) cFoot[i][0] = '\0';
}

static void drawChrome() {
  gfx->fillScreen(C_BG);
  field(X_SYM, Y_HEAD, 9, 1, C_MUTED, "WATCHLIST");
  if (nPages > 1) {
    char p[12];
    snprintf(p, sizeof p, "%u/%u", page + 1, nPages);
    field(72, Y_HEAD, 6, 1, C_DIM, p);
  }
  uint8_t first = page * ROWS;
  if (nStocks > first && nStocks < first + ROWS)
    gfx->drawFastHLine(X_SYM, Y_ROW0 + (nStocks - first) * ROW_H - 4, 240 - 2 * X_SYM, C_RULE);
}

static void drawRows(bool cascade = false) {
  char price[12], pct[12], key[48];
  for (uint8_t slot = 0; slot < ROWS; slot++) {
    int16_t y = Y_ROW0 + slot * ROW_H;
    uint8_t i = page * ROWS + slot;
    if (i >= nRows) {  // short last page: clear, or the old page's rows look current
      if (cRow[slot][0] == '\0') continue;
      cRow[slot][0] = '\0';
      gfx->fillRect(0, y, 240, ROW_H - 4, C_BG);
      continue;
    }
    Row r = rowCopy(i);
    if (r.valid) {
      formatPrice(r.price, price, sizeof price);
      formatPct(r.pct, pct, sizeof pct);
    } else {
      snprintf(price, sizeof price, "--");
      pct[0] = '\0';
    }
    snprintf(key, sizeof key, "%s|%s|%s|%u|%.2f", r.label, price, pct, r.n, r.n ? r.close[r.n - 1] : 0.0f);
    if (strcmp(key, cRow[slot]) == 0) continue;
    strcpy(cRow[slot], key);

    uint16_t fg = !r.valid ? C_DIM : r.pct >= 0 ? C_GOOD : C_BAD;
    if (!blitLogo(X_SYM, y + Y_BADGE, LOGO_BADGE, r.label))
      gfx->fillRect(X_SYM, y + Y_BADGE, LOGO_BADGE, LOGO_BADGE, C_BG);  // the previous page's badge
    field(X_LBL, y + 4, MAX_LABEL, 2, C_FG, r.label);
    drawSpark(X_SPK, y + 4, SPK_W, SPK_H, r);
    fieldRight(X_RIGHT, y + 2, 7, 2, fg, price);
    fieldRight(X_RIGHT, y + 22, 7, 1, fg, pct);
    if (cascade) delay(cascadeMs);
  }
}

static void turnPage(uint8_t to) {
  page = to;
  invalidateCache();
  drawChrome();
  drawRows(true);
}

static void drawHead(const struct tm *t, bool haveTime) {
  char buf[20];
  if (haveTime) strftime(buf, sizeof buf, "%H:%M", t);
  else snprintf(buf, sizeof buf, "--:--");
  if (strcmp(buf, cHead) == 0) return;
  strcpy(cHead, buf);
  fieldRight(X_RIGHT, Y_HEAD, 5, 1, C_MUTED, buf);
}

static void drawFooter(const struct tm *t, bool haveTime) {
  char buf[40];
  bool open = haveTime && marketOpen(*t);
  snprintf(buf, sizeof buf, "market %s", !haveTime ? "?" : open ? "open" : "closed");
  if (strcmp(buf, cFoot[0]) != 0) {
    strcpy(cFoot[0], buf);
    field(X_SYM, Y_FOOT, 20, 1, open ? C_GOOD : C_MUTED, buf);
  }
  snprintf(buf, sizeof buf, "delayed, %lum ago", lastOk ? (unsigned long)((millis() - lastOk) / 60000) : 0UL);
  if (strcmp(buf, cFoot[1]) != 0) {
    strcpy(cFoot[1], buf);
    field(X_SYM, Y_FOOT + FOOT_STEP, 20, 1, C_DIM, buf);
  }
  snprintf(buf, sizeof buf, "%.9s %ddBm  heap %uk", WiFi.SSID().c_str(), WiFi.RSSI(), ESP.getFreeHeap() / 1024);
  if (strcmp(buf, cFoot[2]) != 0) {
    strcpy(cFoot[2], buf);
    field(X_SYM, Y_FOOT + FOOT_STEP * 2, 30, 1, C_MUTED, buf);
  }
}

static void drawPanel(const char *title, uint16_t tc, const char *const *lines, uint8_t n) {
  gfx->fillScreen(C_BG);
  field(8, 24, 12, 3, tc, title);
  gfx->drawFastHLine(8, 54, 224, C_RULE);
  for (uint8_t i = 0; i < n && i < 20; i++) field(8, 62 + i * 11, 38, 1, C_MUTED, lines[i]);
}

// ── detail page ──────────────────────────────────────────────────────────
static uint8_t detailIdx = 0;
static uint32_t detailOpenedAt = 0;
static char cDetail[48];

static void drawButton(uint8_t i, const char *s) {
  int16_t x = 2 + i * 80;
  gfx->drawRect(x, D_Y_BTN, BTN_W, BTN_H, C_MUTED);
  gfx->setTextSize(2);
  gfx->setTextColor(C_FG);
  gfx->setCursor(x + (BTN_W - GW(2) * (int16_t)strlen(s)) / 2, D_Y_BTN + (BTN_H - GH(2)) / 2);
  gfx->print(s);
}

static void drawRange(int16_t y, const char *label, float lo, float hi, float v) {
  char b[12];
  field(X_SYM, y, 5, 1, C_MUTED, label);
  gfx->fillRect(BAR_X, y + 2, BAR_W, BAR_H, C_RULE);
  if (hi > lo) {
    float f = constrain((v - lo) / (hi - lo), 0.0f, 1.0f);
    gfx->fillRect(BAR_X + (int16_t)(f * (BAR_W - 3)), y + 1, 3, BAR_H + 2, C_FG);
  }
  formatPrice(lo, b, sizeof b);
  field(BAR_X, y + 10, 7, 1, C_DIM, b);
  formatPrice(hi, b, sizeof b);
  fieldRight(BAR_X + BAR_W, y + 10, 7, 1, C_DIM, b);
}

static void drawDetail(bool full) {
  Row r = rowCopy(detailIdx);
  if (full) {
    gfx->fillScreen(C_BG);
    cDetail[0] = '\0';
    blitLogo(X_SYM, D_Y_SYM, LOGO_BIG, r.label);  // missing: the square stays black
    drawButton(0, "<PREV");
    drawButton(1, "LIST");
    drawButton(2, "NEXT>");
  }
  char price[12], pct[12], key[48];
  if (r.valid) {
    formatPrice(r.price, price, sizeof price);
    formatPct(r.pct, pct, sizeof pct);
  } else {
    snprintf(price, sizeof price, "--");
    pct[0] = '\0';
  }
  snprintf(key, sizeof key, "%s|%s|%s|%u", r.label, price, pct, r.n);
  if (strcmp(key, cDetail) == 0) return;
  strcpy(cDetail, key);

  uint16_t fg = !r.valid ? C_DIM : r.pct >= 0 ? C_GOOD : C_BAD;
  char name[22];  // what fits beside the logo; a long name is cut, not wrapped
  snprintf(name, sizeof name, "%s", r.coin ? "crypto, 24h change" : r.name);
  field(D_X_TXT, D_Y_SYM, MAX_LABEL, 3, C_FG, r.label);
  field(D_X_TXT, D_Y_NAME, 21, 1, C_MUTED, name);
  field(D_X_TXT, D_Y_PRICE, 7, 3, fg, price);
  char chg[12] = "";
  if (r.valid && r.prev > 0) snprintf(chg, sizeof chg, "%+.2f", r.price - r.prev);
  field(D_X_TXT, D_Y_CHG, 10, 2, fg, chg);
  field(D_X_TXT, D_Y_PCT, 7, 2, fg, pct);

  // Chart: the sparkline's data with room to be a chart. The previous close
  // is a dashed reference line -- the percent is measured from it, so
  // without it the shape means nothing.
  gfx->fillRect(0, D_Y_HI, 240, D_Y_LO + GH(1) - D_Y_HI, C_BG);
  gfx->fillRect(0, D_Y_DAY, 240, D_Y_BTN - 2 - D_Y_DAY, C_BG);
  if (r.coin) {
    field(X_SYM, CH_Y + CH_H / 2 - 4, 30, 1, C_DIM, "no intraday series for coins");
    return;
  }
  if (!r.valid || r.n < 2) {
    field(X_SYM, CH_Y + CH_H / 2 - 4, 20, 1, C_DIM, "no series yet");
    return;
  }
  float lo = r.prev, hi = r.prev;
  for (uint8_t i = 0; i < r.n; i++) {
    lo = min(lo, r.close[i]);
    hi = max(hi, r.close[i]);
  }
  gfx->drawRect(CH_X - 1, CH_Y - 1, CH_W + 2, CH_H + 2, C_RULE);
  int16_t yp = sparkY(r.prev, lo, hi, CH_Y, CH_H);
  for (int16_t x = CH_X; x < CH_X + CH_W; x += 6) gfx->drawFastHLine(x, yp, 3, C_MUTED);
  int16_t px = CH_X, py = sparkY(r.close[0], lo, hi, CH_Y, CH_H);
  for (uint8_t i = 1; i < r.n; i++) {
    int16_t nx = CH_X + (int32_t)i * (CH_W - 1) / (r.n - 1);
    int16_t ny = sparkY(r.close[i], lo, hi, CH_Y, CH_H);
    gfx->drawLine(px, py, nx, ny, fg);
    px = nx;
    py = ny;
  }
  char b[12];
  formatPrice(hi, b, sizeof b);
  fieldRight(X_RIGHT, D_Y_HI, 7, 1, C_DIM, b);
  formatPrice(lo, b, sizeof b);
  fieldRight(X_RIGHT, D_Y_LO, 7, 1, C_DIM, b);
  field(X_SYM, D_Y_HI, 4, 1, C_DIM, "prev");
  drawRange(D_Y_DAY, "day", r.dayLo, r.dayHi, r.price);
  drawRange(D_Y_WK, "52w", r.wkLo, r.wkHi, r.price);
}

// ── fetching (core 0 task) ───────────────────────────────────────────────
static bool fetchStock(NetworkClientSecure &client, Row &r) {
  char url[180];
  snprintf(url, sizeof url, "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=%s", r.id,
           sparkInterval);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", UA);  // without this Yahoo answers 429
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d%s\n", r.id, code, code == 429 ? " (rate limited)" : "");
    http.end();
    return false;
  }
  // ponytail: the body (8-15KB) is held as a String, then parsed through a
  // filter so the document keeps only ~80 floats and a few meta fields.
  // Parse straight from http.getStream() if the heap ever gets tight.
  String body = http.getString();
  http.end();

  JsonDocument filter;
  JsonObject fm = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  for (const char *k : {"regularMarketPrice", "chartPreviousClose", "longName", "shortName", "regularMarketDayHigh",
                        "regularMarketDayLow", "fiftyTwoWeekHigh", "fiftyTwoWeekLow"})
    fm[k] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonVariant res = doc["chart"]["result"][0];
  JsonVariant m = res["meta"];
  if (!m["regularMarketPrice"].is<float>()) {
    Serial.printf("%s: no price in payload (bad symbol?)\n", r.id);
    return false;
  }
  r.price = m["regularMarketPrice"] | 0.0f;
  r.prev = m["chartPreviousClose"] | 0.0f;
  r.pct = r.prev > 0 ? (r.price - r.prev) / r.prev * 100.0f : 0.0f;
  r.dayLo = m["regularMarketDayLow"] | 0.0f;
  r.dayHi = m["regularMarketDayHigh"] | 0.0f;
  r.wkLo = m["fiftyTwoWeekLow"] | 0.0f;
  r.wkHi = m["fiftyTwoWeekHigh"] | 0.0f;
  const char *nm = m["longName"] | (m["shortName"] | "");
  snprintf(r.name, sizeof r.name, "%s", nm);
  // The close array contains null for gaps and halts. Skip them rather than
  // parsing to 0.0, which would drop the line to the floor.
  // ponytail: the line is drawn through a gap; break it there if it matters.
  r.n = 0;
  for (JsonVariant v : res["indicators"]["quote"][0]["close"].as<JsonArray>()) {
    if (!v.is<float>()) continue;
    if (r.n == SPARK_N) {
      memmove(r.close, r.close + 1, (SPARK_N - 1) * sizeof(float));
      r.n--;
    }
    r.close[r.n++] = v.as<float>();
  }
  r.valid = true;
  return true;
}

static bool fetchCoins(NetworkClientSecure &client) {
  if (!coinIds[0]) return false;
  char url[sizeof coinIds + 128];
  snprintf(url, sizeof url,
           "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true",
           coinIds);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", UA);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("coins http %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  bool any = false;
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    JsonVariant v = doc[rows[i].id];
    if (!v["usd"].is<float>()) {
      Serial.printf("coin '%s' not in response (bad id?)\n", rows[i].id);
      continue;
    }
    Row r = rowCopy(i);
    r.price = v["usd"] | 0.0f;
    r.pct = v["usd_24h_change"] | 0.0f;
    r.valid = true;
    rowStore(i, r);
    any = true;
  }
  return any;
}

static void fetchOne(uint8_t i) {
  Row r = rowCopy(i);
  NetworkClientSecure client;
  client.setInsecure();  // public read-only quotes; pinning buys nothing
  bool ok = fetchStock(client, r);
  if (ok) {
    rowStore(i, r);
    lastOk = millis();
    failures = 0;
  } else {
    failures = failures + 1;
  }
}

static void doCoins() {
  NetworkClientSecure client;
  client.setInsecure();
  bool ok = fetchCoins(client);
  if (ok) {
    lastOk = millis();
    failures = 0;
  } else {
    failures = failures + 1;
  }
  Serial.printf("coins %s (failures %u, heap %u)\n", ok ? "ok" : "FAILED", failures, ESP.getFreeHeap());
}

// Sequential, one request at a time, with a tapped symbol jumping the queue.
static void fetchTask(void *) {
  bool sweeping = false;
  uint8_t sweepIdx = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() != WL_CONNECTED) continue;
    int8_t pri = priority;
    if (pri >= 0) {
      priority = -1;
      if (rows[pri].coin) doCoins();
      else fetchOne(pri);
      continue;
    }
    if (nCoins && (lastCoin == 0 || millis() - lastCoin > coinMs)) {
      lastCoin = millis();
      doCoins();
      continue;
    }
    struct tm t;
    bool haveTime = getLocalTime(&t, 0);
    uint32_t stockEvery = (haveTime && marketOpen(t)) ? stockOpenMs : stockShutMs;
    if (stockEvery < nStocks * 5000UL) stockEvery = nStocks * 5000UL;  // rate floor
    if (!sweeping && nStocks && (lastStock == 0 || millis() - lastStock > stockEvery)) {
      sweeping = true;
      sweepIdx = 0;
    }
    if (!sweeping) continue;
    if (sweepIdx >= nStocks) {
      sweeping = false;
      lastStock = millis();
      Serial.printf("sweep done (%u stocks, heap %u)\n", nStocks, ESP.getFreeHeap());
    } else {
      fetchOne(sweepIdx++);
    }
  }
}

// ── touch: taps resolved at touch-down ───────────────────────────────────
static bool touchHeld = false;
static bool pollTap(int16_t *x, int16_t *y) {
  static int16_t dx = 0, dy = 0;
  static uint32_t t0 = 0, lastTap = 0;
  int16_t cx, cy;
  bool now = touchRead(&cx, &cy);
  if (now && !touchHeld) {
    dx = cx;
    dy = cy;
    t0 = millis();
  }
  bool tap = false;
  if (!now && touchHeld && millis() - t0 < 800 && millis() - lastTap > 150) {
    *x = dx;
    *y = dy;
    lastTap = millis();
    tap = true;
  }
  touchHeld = now;
  return tap;
}

// ── brightness from the LDR ──────────────────────────────────────────────
// Calibration knob: ldrDark is the raw reading treated as fully dark. The
// sensor read ~0 in room light on this unit; if the panel dims in a bright
// room instead, the sense is inverted and this mapping needs flipping.
static void brightnessTick(bool open) {
  static uint32_t last = 0;
  static float ema = -1;
  static int applied = -1;
  if (millis() - last < 1000) return;
  last = millis();
  int ldr = analogRead(LDR);
  if (ema < 0) ema = ldr;
  ema += (ldr - ema) * 0.3f;
  int lvl = blMax;
  if (blAuto) {
    float dark = constrain(ema / ldrDark, 0.0f, 1.0f);
    lvl = blMax - (int)(dark * (blMax - blMin));
  }
  if (!open) lvl = max((int)blMin, lvl * blClosedScale / 100);
  if (abs(lvl - applied) >= 4) {
    applied = lvl;
    backlight((uint8_t)lvl);
  }
}

// ── self-check ───────────────────────────────────────────────────────────
static void selfCheck() {
  cfgSelfCheck();
  touchSelfCheck();
  char b[16];
  formatPrice(0.4215f, b, sizeof b);  assert(strcmp(b, "0.4215") == 0);
  formatPrice(334.35f, b, sizeof b);  assert(strcmp(b, "334.4") == 0);
  formatPrice(79010.0f, b, sizeof b); assert(strcmp(b, "79010") == 0);
  for (float v : {0.0f, 1.0f, 99.99f, 9999.0f, 1.5e6f, 9.9e7f}) {
    formatPrice(v, b, sizeof b);
    assert(strlen(b) <= 7);
  }
  formatPct(2.44f, b, sizeof b);   assert(strcmp(b, "+2.4%") == 0);
  formatPct(-12.34f, b, sizeof b); assert(strcmp(b, "-12.3%") == 0);
  for (float p : {0.0f, -9.99f, 99.9f, -100.0f, 1234.0f}) {
    formatPct(p, b, sizeof b);
    assert(strlen(b) <= 7);
  }
  assert(mktOpenMin == 9 * 60 + 30 && mktCloseMin == 16 * 60);  // runs before loadConfig()
  struct tm t = {};
  t.tm_wday = 3; t.tm_hour = 9; t.tm_min = 29;  assert(!marketOpen(t));
  t.tm_min = 30;                                assert(marketOpen(t));
  t.tm_hour = 16; t.tm_min = 0;                 assert(!marketOpen(t));
  t.tm_hour = 12; t.tm_wday = 6;                assert(!marketOpen(t));

  assert(nRows == 0);
  assert(!addRow("TOOLONG", "TOOLONG", false));
  assert(!addRow("", "X", false));
  assert(!addRow("X", nullptr, false));
  for (uint8_t i = 0; i < MAX_SYMBOLS; i++) assert(addRow("AAA", "aaa", false));
  assert(!addRow("BBB", "bbb", false));
  nRows = 0;

  // Hit tests: the head and footer are not rows, every row maps to itself.
  assert(hitSlot(Y_ROW0 - 1) == -1 && hitSlot(Y_ROW0) == 0);
  assert(hitSlot(Y_ROW0 + ROW_H * ROWS - 1) == ROWS - 1 && hitSlot(Y_ROW0 + ROW_H * ROWS) == -1);
  assert(hitButton(0, D_Y_BTN - 1) == -1 && hitButton(0, D_Y_BTN) == 0);
  assert(hitButton(120, 319) == 1 && hitButton(239, 319) == 2);
  // Sparkline scaling: extremes hit the box edges, a flat series sits mid-box.
  assert(sparkY(10, 10, 20, 100, 28) == 127 && sparkY(20, 10, 20, 100, 28) == 100);
  assert(sparkY(5, 5, 5, 100, 28) == 114);

  // Geometry: badge, symbol, sparkline and price never overlap; rows clear the footer.
  assert(X_SYM + LOGO_BADGE <= X_LBL);
  assert(Y_BADGE + LOGO_BADGE <= ROW_H - 4);
  assert(X_LBL + GW(2) * MAX_LABEL <= X_SPK);
  assert(X_SPK + SPK_W <= X_RIGHT - GW(2) * 7);
  assert(4 + SPK_H <= ROW_H - 4);
  assert(Y_ROW0 + ROW_H * ROWS <= Y_FOOT);
  assert(Y_FOOT + FOOT_STEP * 2 + GH(1) <= 320);
  // Detail: the logo and the text column beside it, then the chart labels
  // and the buttons, all fit.
  assert(X_SYM + LOGO_BIG <= D_X_TXT && D_Y_SYM + LOGO_BIG <= D_Y_HI);
  assert(D_X_TXT + GW(3) * 7 <= 240 && D_X_TXT + GW(1) * 21 <= 240 && D_X_TXT + GW(2) * 10 <= 240);
  assert(D_Y_SYM + GH(3) <= D_Y_NAME && D_Y_NAME + GH(1) <= D_Y_PRICE);
  assert(D_Y_PRICE + GH(3) <= D_Y_CHG && D_Y_CHG + GH(2) <= D_Y_PCT && D_Y_PCT + GH(2) <= D_Y_HI);
  assert(D_Y_HI + GH(1) <= CH_Y && CH_Y + CH_H <= D_Y_LO && D_Y_LO + GH(1) <= D_Y_DAY);
  assert(D_Y_DAY + 10 + GH(1) <= D_Y_WK && D_Y_WK + 10 + GH(1) <= D_Y_BTN);
  assert(D_Y_BTN + BTN_H <= 320 && 2 + 2 * 80 + BTN_W <= 240);
  assert(sizeof logoBuf >= (size_t)LOGO_BADGE * LOGO_BADGE * 2);
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoConfig, NoWifi, NoData, Running };
enum class View { List, Detail };
static State state = State::Boot;
static View view = View::List;

void setup() {
  Serial.begin(115200);
  delay(300);
  boardBegin();
  selfCheck();

  gfx = boardDisplay();
  gfx->begin();
  gfx->fillScreen(C_BG);
  backlight(blMax);

  if (!loadConfig()) {
    Serial.printf("config error: %s\n", cfgErr);
    return;  // loop() draws the panel
  }
  cfgRelease();
  logoInventory();

  const char *boot[] = {"connecting to wifi", WIFI_SSID};
  drawPanel("STARTING", C_MUTED, boot, 2);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinBest(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
    configTzTime(tzString, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    for (int i = 0; i < 40 && !getLocalTime(&t, 250); i++) {}
  }

  mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
}

static void drawFailPanel() {
  static char key[32];
  char k[32];
  snprintf(k, sizeof k, "%d-%u", (int)state, failures);
  if (strcmp(k, key) == 0) return;
  strcpy(key, k);
  if (state == State::NoConfig) {
    const char *l[] = {cfgErr, "", "upload the watchlist:", "  ./push-config ticker"};
    drawPanel("NO LIST", C_WARN, l, 4);
  } else if (state == State::NoWifi) {
    static char ssid[40];
    snprintf(ssid, sizeof ssid, "SSID %s", WIFI_SSID);
    const char *l[] = {ssid, "", "not associated. 2.4GHz only.", "", "retrying..."};
    drawPanel("NO WIFI", C_BAD, l, 5);
  } else {
    static char f[34];
    snprintf(f, sizeof f, "%u failed fetches", failures);
    const char *l[] = {"no quotes yet", f, "", "check the serial log for the", "http code -- yahoo rate-limits.",
                       "", "retrying..."};
    drawPanel("NO DATA", C_WARN, l, 7);
  }
}

static void openDetail(uint8_t idx) {
  view = View::Detail;
  detailIdx = idx;
  detailOpenedAt = millis();
  if (refreshOnOpen) priority = idx;
  drawDetail(true);
}

static void backToList() {
  view = View::List;
  invalidateCache();
  drawChrome();
  drawRows();
}

void loop() {
  int16_t tx, ty;
  bool tap = pollTap(&tx, &ty);
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  bool open = haveTime && marketOpen(t);

  bool anyValid = false;
  for (uint8_t i = 0; i < nRows; i++) anyValid |= rows[i].valid;  // a bool read; no lock needed
  State want = cfgErr                          ? State::NoConfig
               : WiFi.status() != WL_CONNECTED ? State::NoWifi
               : !anyValid                     ? State::NoData
                                               : State::Running;
  if (want != state) {
    state = want;
    invalidateCache();
    if (state == State::Running) {
      if (view == View::List) drawChrome();
      else drawDetail(true);
    }
  }
  if (state == State::NoConfig) {
    drawFailPanel();
    delay(50);
    return;
  }

  static uint32_t lastRetry = 0;
  static uint16_t retries = 0;
  if (state == State::NoWifi) {
    if (lastRetry == 0 || millis() - lastRetry > netRetryDelay(retries, WIFI_RETRY_MS)) {
      lastRetry = millis();
      Serial.printf("wifi retry #%u\n", ++retries);
      WiFi.disconnect();
      netJoinBest(WIFI_SSID, WIFI_PASS);
    }
  } else {
    retries = 0;
    lastRetry = 0;
  }

  if (state != State::Running) {
    drawFailPanel();
  } else if (view == View::List) {
    drawHead(&t, haveTime);
    drawRows();
    drawFooter(&t, haveTime);
  } else {
    drawDetail(false);
  }

  if (tap && state == State::Running) {
    tone(SPK, 1200, 15);
    if (view == View::List) {
      int8_t slot = hitSlot(ty);
      uint8_t idx = page * ROWS + (slot < 0 ? 0 : slot);
      if (slot >= 0 && idx < nRows) openDetail(idx);
    } else {
      int8_t b = hitButton(tx, ty);
      if (b == 0) openDetail((detailIdx + nRows - 1) % nRows);
      else if (b == 2) openDetail((detailIdx + 1) % nRows);
      else backToList();
    }
    Serial.printf("tap %d,%d -> %s %s\n", tx, ty, view == View::List ? "list" : "detail",
                  view == View::Detail ? rows[detailIdx].label : "");
  }

  // The page timer is frozen while a finger is down, so a tap can never land
  // on one symbol and resolve to the one that replaced it.
  static uint32_t lastAdvance = 0;
  if (touchHeld) lastAdvance = millis() - min(millis() - lastAdvance, pageMs - 1);
  if (state == State::Running && view == View::List && nPages > 1 && millis() - lastAdvance > pageMs) {
    lastAdvance = millis();
    turnPage((page + 1) % nPages);
  }
  if (view == View::Detail && returnMs && !touchHeld && millis() - detailOpenedAt > returnMs) backToList();

  brightnessTick(open);

  static uint32_t lastLog = 0;
  if (millis() - lastLog > LOG_MS) {
    lastLog = millis();
    Serial.printf("heap %u  wifi %s %ddBm  ldr %d  rows", ESP.getFreeHeap(),
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN", WiFi.RSSI(), analogRead(LDR));
    for (uint8_t i = 0; i < nRows; i++) Serial.printf(" %s=%s", rows[i].label, rows[i].valid ? "ok" : "-");
    Serial.println();
  }
  delay(20);
}
