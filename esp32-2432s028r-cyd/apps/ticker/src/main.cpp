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
#include <Preferences.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── one colour scheme: black, white symbols, green and red numbers ───────
static const uint16_t C_BG = RGB565_BLACK, C_FG = RGB565_WHITE, C_GOOD = 0x07E0, C_BAD = 0xF800,
                      C_DIM = 0x630C, C_MUTED = 0xA534, C_RULE = 0x2104, C_WARN = RGB565_YELLOW;

// ── settings (defaults; data/config.json overrides) ──────────────────────
static char tzString[64] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static uint16_t mktOpenMin = 9 * 60 + 30, mktCloseMin = 16 * 60, mktPreMin = 4 * 60, mktPostMin = 20 * 60;
static uint32_t pageMs = 10000;  // six rows scroll past in this long
static bool blAuto = true, cfgBlAuto = true;
static uint8_t blFixed = 220;  // the level when not auto
static uint8_t blMin = 20, blMax = 220, blClosedScale = 60;
static uint16_t ldrDark = 3000;  // raw LDR reading that counts as a dark room
static char sparkInterval[4] = "5m";
static uint32_t returnMs = 60000;
static bool refreshOnOpen = true;
static uint32_t stockOpenMs = 5UL * 60 * 1000, stockExtMs = 15UL * 60 * 1000, coinMs = 5UL * 60 * 1000;
static const uint32_t WIFI_RETRY_MS = 20UL * 1000, LOG_MS = 60UL * 1000;
static const char *UA = "Mozilla/5.0 (esp32-ticker)";

static const uint8_t ROWS = 7, MAX_SYMBOLS = 32, MAX_LABEL = 5, SPARK_N = 200;  // 04:00-20:00 at 5m is 192
// Two logo sizes, both pre-converted on the Mac (tools/make-logos.py --size N
// --out data/logo/N) and read raw from /logo/<N>/<LABEL>.565. Must match.
// ponytail: 96 on the detail page, not the README's 128 -- 26 x 32KB plus
// badges overflows the 896KB LittleFS partition. 128 when the SD card lands.
static const uint8_t LOGO_BADGE = 24, LOGO_BIG = 96;
#define GW(s) (6 * (s))
#define GH(s) (8 * (s))

static Arduino_GFX *gfx;
static bool touchHeld = false;  // set by pollGesture; the list freezes while a finger is down

// ── settings the finger can change ───────────────────────────────────────
// Four things worth a tap on the device itself, each a short cycle. They
// live in NVS and beat config.json, the way the C6's layout choice does --
// otherwise a config push would undo a tap on every boot. Everything else
// stays in config.json, where a keyboard is.
static const char *const SPEED_NAMES[] = {"slow", "normal", "fast"};
static const uint32_t SPEED_MS[] = {20000, 10000, 5000};
static const char *const BL_NAMES[] = {"auto", "bright", "dim"};
static const char *const RET_NAMES[] = {"15s", "60s", "never"};
static const uint32_t RET_MS[] = {15000, 60000, 0};
static uint8_t sSpeed = 1, sBl = 0, sRet = 1;
static bool sSound = true;
static Preferences prefs;
static int blApplied = -1, ldrRaw = 0;  // brightnessTick's last reading and level, for the info page

// ── the watchlist ────────────────────────────────────────────────────────
struct Row {
  char label[MAX_LABEL + 1];
  char id[28];
  char name[32];
  bool coin, valid;
  float price, pct, prev, dayLo, dayHi, wkLo, wkHi;
  float last;  // the newest bar, extended hours included; == price in the regular session
  uint8_t n;
  float close[SPARK_N];
};
static Row rows[MAX_SYMBOLS];
static uint8_t nRows = 0, nStocks = 0, nCoins = 0;
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

  cfgBlAuto = cfgBool("brightness.auto", cfgBlAuto);
  blMin = (uint8_t)cfgInt("brightness.min", blMin, 8, 255);
  blMax = (uint8_t)cfgInt("brightness.max", blMax, 8, 255);
  if (blMax < blMin) blMax = blMin;
  blClosedScale = (uint8_t)cfgInt("brightness.closedScale", blClosedScale, 10, 100);
  ldrDark = (uint16_t)cfgInt("brightness.ldrDark", ldrDark, 100, 4095);
  pageMs = (uint32_t)cfgInt("timing.pageSeconds", pageMs / 1000, 2, 600) * 1000UL;
  char iv[4] = "";
  if (cfgStr("sparkline.interval", iv, sizeof iv)) {
    if (!strcmp(iv, "5m") || !strcmp(iv, "15m") || !strcmp(iv, "30m")) strcpy(sparkInterval, iv);
    else Serial.printf("config sparkline.interval: '%s' is not 5m/15m/30m, ignored\n", iv);
  }
  returnMs = (uint32_t)cfgInt("detail.returnSeconds", returnMs / 1000, 0, 3600) * 1000UL;
  refreshOnOpen = cfgBool("detail.refreshOnOpen", refreshOnOpen);
  stockOpenMs = (uint32_t)cfgInt("refresh.openMinutes", stockOpenMs / 60000, 1, 240) * 60000UL;
  stockExtMs = (uint32_t)cfgInt("refresh.extendedMinutes", stockExtMs / 60000, 1, 1440) * 60000UL;
  coinMs = (uint32_t)cfgInt("refresh.coinMinutes", coinMs / 60000, 1, 240) * 60000UL;
  cfgStr("timezone", tzString, sizeof tzString);
  cfgHhMm("market.pre", &mktPreMin);
  cfgHhMm("market.open", &mktOpenMin);
  cfgHhMm("market.close", &mktCloseMin);
  cfgHhMm("market.post", &mktPostMin);
  if (!(mktPreMin <= mktOpenMin && mktOpenMin < mktCloseMin && mktCloseMin <= mktPostMin)) {
    Serial.printf("config market: %u <= %u < %u <= %u does not hold, restoring 04:00 09:30-16:00 20:00\n", mktPreMin,
                  mktOpenMin, mktCloseMin, mktPostMin);
    mktPreMin = 4 * 60;
    mktOpenMin = 9 * 60 + 30;
    mktCloseMin = 16 * 60;
    mktPostMin = 20 * 60;
  }
  Serial.printf("settings: bl %s %u-%u closed %u%% ldrDark %u  page %lus  spark %s  return %lus\n",
                blAuto ? "auto" : "fixed", blMin, blMax, blClosedScale, ldrDark,
                (unsigned long)(pageMs / 1000), sparkInterval, (unsigned long)(returnMs / 1000));
  Serial.printf("settings: refresh %lu/%lu/%lu min  market %02u:%02u %02u:%02u-%02u:%02u %02u:%02u  tz %s\n",
                (unsigned long)(stockOpenMs / 60000), (unsigned long)(stockExtMs / 60000),
                (unsigned long)(coinMs / 60000), mktPreMin / 60, mktPreMin % 60, mktOpenMin / 60, mktOpenMin % 60,
                mktCloseMin / 60, mktCloseMin % 60, mktPostMin / 60, mktPostMin % 60, tzString);

  coinIds[0] = '\0';
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    if (coinIds[0]) strlcat(coinIds, ",", sizeof coinIds);
    strlcat(coinIds, rows[i].id, sizeof coinIds);
  }
  Serial.printf("watchlist: %u stocks + %u coins\n", nStocks, nCoins);
  return true;
}

// ── layout (portrait 240x320, fixed) ─────────────────────────────────────
// List: six 42px rows. Badge | symbol | sparkline | price over percent.
// Everything in a row is centred on y+16: badge 4..28, symbol 8..24, the
// sparkline 2..30, price 2..18 over percent 22..30.
static const int16_t Y_HEAD = 4, Y_ROW0 = 22, ROW_H = 42, X_SYM = 6, Y_BADGE = 4, X_LBL = 34, Y_LBL = 8,
                     X_SPK = 98, Y_SPK = 2, SPK_W = 48, SPK_H = 28, X_RIGHT = 234;
// Settings: title, five 40px rows, the LIST button.
static const int16_t S_Y0 = 64, S_H = 40, S_N = 5;
// Detail: 96px logo at the left with symbol, name, price, change beside it;
// then the chart (high and low printed inside it), a row of five range
// chips sized for a finger, two range bars, and a one-line gesture hint.
// No buttons: swipe left/right for the next/previous stock, down for the
// list, like any phone app.
static const int16_t D_X_TXT = 108, D_Y_SYM = 6, D_Y_NAME = 34, D_Y_PRICE = 46, D_Y_CHG = 74, D_Y_PCT = 92,
                     CH_X = 6, CH_Y = 112, CH_W = 228, CH_H = 96, R_Y = 212, R_W = 44, R_STEP = 46, R_H = 24,
                     D_Y_DAY = 244, D_Y_WK = 268, BAR_X = 40, BAR_W = 194, BAR_H = 6, Y_HINT = 306;

// ── the detail chart's range ─────────────────────────────────────────────
// 1D is the row's own sparkline series. The other four are fetched the
// moment a page opens, selected one first, so a chip tap is instant once
// they have landed -- a TLS round trip per tap felt slow.
struct RangeDef { const char *label, *range, *interval; };
static const RangeDef RANGES[] = {{"1D", "1d", nullptr}, {"5D", "5d", "30m"}, {"1M", "1mo", "1d"},
                                  {"6M", "6mo", "1d"}, {"1Y", "1y", "1wk"}};
static const uint8_t N_RANGES = 5, SERIES_N = 130;  // 6mo of daily closes is ~126
struct Series {
  uint8_t idx, range, n;
  bool valid;
  float prev;
  float close[SERIES_N];
};
static Series series[N_RANGES];  // [0] unused; written by the fetch task, read by the UI, under mux
static volatile uint8_t seriesWant = 0;  // bitmask of ranges still to fetch for seriesIdx
static volatile uint8_t seriesIdx = 0;
static uint8_t rangeSel = 0;

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
// The trading day in four parts. Closed is a weekend or the night between
// the post session and the next pre session: nothing prints then, so
// nothing is fetched. A holiday looks like a weekday here and costs a few
// unchanged fetches; not worth a calendar.
enum class Session : uint8_t { Closed, Pre, Regular, Post };
static Session session(const struct tm &t) {
  if (t.tm_wday == 0 || t.tm_wday == 6) return Session::Closed;
  int mins = t.tm_hour * 60 + t.tm_min;
  if (mins < mktPreMin || mins >= mktPostMin) return Session::Closed;
  if (mins < mktOpenMin) return Session::Pre;
  if (mins < mktCloseMin) return Session::Regular;
  return Session::Post;
}
static bool marketOpen(const struct tm &t) { return session(t) == Session::Regular; }
static const char *sessionWord(Session s) {
  switch (s) {
    case Session::Pre: return "pre-market";
    case Session::Regular: return "market open";
    case Session::Post: return "after hours";
    default: return "market closed";
  }
}
// "opens Mon 09:30": the next regular open from t. Pure.
static void nextOpen(const struct tm &t, char *out, size_t n) {
  static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int wday = t.tm_wday;
  bool today = wday >= 1 && wday <= 5 && t.tm_hour * 60 + t.tm_min < mktOpenMin;
  if (!today)
    do wday = (wday + 1) % 7;
    while (wday == 0 || wday == 6);
  if (today) snprintf(out, n, "%02u:%02u", mktOpenMin / 60, mktOpenMin % 60);
  else snprintf(out, n, "%s %02u:%02u", days[wday], mktOpenMin / 60, mktOpenMin % 60);
}
// The list is an endless strip of "virtual rows" v (any integer): v maps to
// watchlist row v mod n and to ring slot v mod ROWS, and the strip is
// scrolled by pos pixels, either sign. Floor-mod so a drag back past the
// start behaves. Which row is under screen y, or -1. Pure, so selfCheck
// can drive it.
static const int16_t RING = ROW_H * ROWS;
static int32_t modp(int32_t a, int32_t m) {
  int32_t r = a % m;
  return r < 0 ? r + m : r;
}
static int32_t floordiv(int32_t a, int32_t m) { return (a - modp(a, m)) / m; }
static int16_t hitRow(int16_t y, int32_t pos, uint8_t n) {
  if (n == 0 || y < Y_ROW0 || y >= Y_ROW0 + RING) return -1;
  return (int16_t)modp(floordiv(pos + y - Y_ROW0, ROW_H), n);
}
// Which settings row, or -1.
static int8_t hitSetting(int16_t y) {
  if (y < S_Y0 || y >= S_Y0 + S_H * S_N) return -1;
  return (y - S_Y0) / S_H;
}
// Which range chip, or -1. The zone is the whole strip between the chart
// and the day bar, taller than the drawn chip, because the chip is small.
static int8_t hitRange(int16_t x, int16_t y) {
  if (y < CH_Y + CH_H || y >= D_Y_DAY || x < X_SYM) return -1;
  int8_t i = (x - X_SYM) / R_STEP;
  return i >= N_RANGES ? -1 : i;
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
static uint8_t haveLogo[2];  // per size, for the info page
static void logoInventory() {
  for (uint8_t size : {LOGO_BADGE, LOGO_BIG}) {
    uint8_t have = 0;
    for (uint8_t i = 0; i < nRows; i++) {
      char path[40];
      snprintf(path, sizeof path, "/logo/%u/%s.565", size, rows[i].label);
      File f = LittleFS.open(path, "r");
      if (f && f.size() == (size_t)size * size * 2) have++;
    }
    haveLogo[size == LOGO_BIG] = have;
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

// ── the list: a scrolling ring ───────────────────────────────────────────
// The panel scrolls in hardware. VSCRDEF (0x33) fences the seven row slots
// under the fixed header, VSCRSADD (0x37) says which line of that region
// shows at the top, and the region wraps. Moving the whole list one pixel
// is one two-byte command. The catch: when pos is not on a row boundary,
// the top row and the row entering at the bottom share ONE slot (they are
// seven virtual rows apart, and the ring has seven slots): the top row owns
// lines [off, ROW_H), the entering row owns [0, off). So whichever row is
// entering is painted off-screen into a one-row canvas, and as pos moves
// its lines are fed into the shared slot -- forward that is row v0+7 from
// the top of the slot down, backward it is row v0 from the bottom up. The
// canvas holds whichever one the direction needs and is repainted on a
// reversal.
static Arduino_Canvas *rowCanvas;
static int32_t pos = 0;                 // pixels scrolled; row 0 sat at the top at 0
static int32_t canvasV = INT32_MIN;     // the virtual row in the canvas
static uint32_t scrollLast = 0, scrollAcc = 0, holdUntil = 0;
static char cRow[ROWS][48], cCanvas[48], cHead[24];

static uint8_t slotOf(int32_t v) { return (uint8_t)modp(v, ROWS); }
static uint16_t rowOf(int32_t v) { return (uint16_t)modp(v, nRows); }
static int32_t topV() { return floordiv(pos, ROW_H); }
static uint8_t offPx() { return (uint8_t)modp(pos, ROW_H); }

static void panelScroll(int32_t p) {
  Arduino_DataBus *bus = boardBus();
  bus->beginWrite();
  bus->writeCommand(0x37);  // VSCRSADD
  bus->write16(Y_ROW0 + (uint16_t)modp(p, RING));
  bus->endWrite();
}
static void panelScrollArea() {
  Arduino_DataBus *bus = boardBus();
  bus->beginWrite();
  bus->writeCommand(0x33);  // VSCRDEF: top fixed, scrolling, bottom fixed
  bus->write16(Y_ROW0);
  bus->write16(RING);
  bus->write16(LCD_H - Y_ROW0 - RING);
  bus->endWrite();
}

static void invalidateCache() {
  for (uint8_t i = 0; i < ROWS; i++) cRow[i][0] = '\0';
  cHead[0] = '\0';
}

static void rowKey(const Row &r, char *key, size_t n, char *price, char *pct) {
  if (r.valid) {
    formatPrice(r.price, price, 12);
    formatPct(r.pct, pct, 12);
  } else {
    snprintf(price, 12, "--");
    pct[0] = '\0';
  }
  snprintf(key, n, "%s|%s|%s|%u|%.2f", r.label, price, pct, r.n, r.n ? r.close[r.n - 1] : 0.0f);
}

// Paint one row with its top at y on whatever gfx points at: the panel (a
// ring slot) or the row canvas (y = 0). A rule marks the stock/coin seam.
static void paintRow(int16_t y, const Row &r, bool rule) {
  char price[12], pct[12], key[48];
  rowKey(r, key, sizeof key, price, pct);
  uint16_t fg = !r.valid ? C_DIM : r.pct >= 0 ? C_GOOD : C_BAD;
  gfx->drawFastHLine(0, y, LCD_W, rule ? C_RULE : C_BG);
  if (!blitLogo(X_SYM, y + Y_BADGE, LOGO_BADGE, r.label))
    gfx->fillRect(X_SYM, y + Y_BADGE, LOGO_BADGE, LOGO_BADGE, C_BG);  // the previous occupant's badge
  field(X_LBL, y + Y_LBL, MAX_LABEL, 2, C_FG, r.label);
  drawSpark(X_SPK, y + Y_SPK, SPK_W, SPK_H, r);
  fieldRight(X_RIGHT, y + 2, 7, 2, fg, price);
  fieldRight(X_RIGHT, y + 22, 7, 1, fg, pct);
}
static bool seam(uint16_t rowIdx) { return nStocks && nCoins && (rowIdx == 0 || rowIdx == nStocks); }

// A slot fully owned by virtual row v: repaint only if the row's key changed.
static void paintSlot(int32_t v) {
  Row r = rowCopy(rowOf(v));
  char key[48], price[12], pct[12];
  rowKey(r, key, sizeof key, price, pct);
  if (strcmp(key, cRow[slotOf(v)]) == 0) return;
  strcpy(cRow[slotOf(v)], key);
  paintRow(Y_ROW0 + slotOf(v) * ROW_H, r, seam(rowOf(v)));
}

// The entering row, painted off-screen. ponytail: the drawing helpers all
// go through the global gfx, so point it at the canvas for the duration
// rather than thread a target through every one of them.
static void paintCanvas(int32_t v) {
  if (v == canvasV) return;
  canvasV = v;
  Row r = rowCopy(rowOf(v));
  char price[12], pct[12];
  rowKey(r, cCanvas, sizeof cCanvas, price, pct);
  Arduino_GFX *panel = gfx;
  gfx = rowCanvas;
  gfx->fillScreen(C_BG);
  paintRow(0, r, seam(rowOf(v)));
  gfx = panel;
}
// Canvas lines [from, to) into the slot of virtual row v.
static void feed(int32_t v, uint8_t from, uint8_t to) {
  gfx->draw16bitRGBBitmap(0, Y_ROW0 + slotOf(v) * ROW_H + from, rowCanvas->getFramebuffer() + from * LCD_W, LCD_W,
                          to - from);
}

// Move the strip by delta pixels, either sign, one row-boundary chunk at a
// time: scroll the panel, then feed the lines that just came into view.
static void scrollBy(int32_t delta) {
  while (delta) {
    int32_t v0 = topV();
    uint8_t off = offPx();
    if (delta > 0) {
      uint8_t k = (uint8_t)min<int32_t>(delta, ROW_H - off);
      paintCanvas(v0 + ROWS);
      pos += k;
      panelScroll(pos);
      feed(v0, off, off + k);
      if (off + k == ROW_H) strcpy(cRow[slotOf(v0)], cCanvas);  // v0+7 owns the slot now
      delta -= k;
    } else {
      if (off == 0) {  // step back across the boundary: v0-1 starts entering at the top
        v0 -= 1;
        off = ROW_H;
      }
      uint8_t k = (uint8_t)min<int32_t>(-delta, off);
      paintCanvas(v0);
      pos -= k;
      panelScroll(pos);
      feed(v0, off - k, off);
      if (off == k) strcpy(cRow[slotOf(v0)], cCanvas);  // v0 owns the slot now
      delta += k;
    }
  }
}

static void listStart() {
  gfx->fillScreen(C_BG);
  panelScrollArea();
  panelScroll(pos);
  invalidateCache();
  canvasV = INT32_MIN;
  field(X_SYM, Y_HEAD, 9, 1, C_MUTED, "WATCHLIST");
  int32_t v0 = topV();
  uint8_t off = offPx();
  for (uint8_t p = 0; p < ROWS; p++) paintSlot(v0 + p);
  if (off) {  // the shared slot: the entering row's lines over the top row's
    paintCanvas(v0 + ROWS);
    feed(v0, 0, off);
  }
  scrollLast = millis();
  scrollAcc = 0;
}

// Every pass: refresh the slots fully in view, then advance the crawl by
// however many pixels the clock owes. Frozen while a finger is down and
// for a moment after, so a drag or a press is not fought.
static void listTick() {
  uint32_t now = millis(), dt = now - scrollLast;
  scrollLast = now;
  int32_t v0 = topV();
  for (uint8_t p = offPx() ? 1 : 0; p < ROWS; p++) paintSlot(v0 + p);
  if (touchHeld || now < holdUntil) {
    scrollAcc = 0;
    return;
  }
  scrollAcc += dt * RING;  // pixels, scaled by pageMs
  int32_t step = scrollAcc / pageMs;
  if (!step) return;
  scrollAcc -= (uint32_t)step * pageMs;
  scrollBy(step);
}

static void drawHead(const struct tm *t, bool haveTime) {
  char buf[20], clk[8];
  if (haveTime) strftime(clk, sizeof clk, "%H:%M", t);
  else snprintf(clk, sizeof clk, "--:--");
  Session ses = haveTime ? session(*t) : Session::Regular;
  snprintf(buf, sizeof buf, "%s|%u", clk, (unsigned)ses);
  if (strcmp(buf, cHead) == 0) return;
  strcpy(cHead, buf);
  fieldRight(X_RIGHT, Y_HEAD, 5, 1, C_MUTED, clk);
  char tag[24] = "";
  if (ses == Session::Closed) {
    char nx[16];
    nextOpen(*t, nx, sizeof nx);
    snprintf(tag, sizeof tag, "CLOSED til %s", nx);
  }
  field(72, Y_HEAD, 18, 1, C_WARN, tag);  // "market open" says nothing; only closed is news
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

// One dim line at the foot of a page saying which swipes it takes. Not a
// control: the gestures work anywhere on the page.
static void drawHint(const char *s) { fieldCentre(LCD_W / 2, Y_HINT, 38, 1, C_DIM, s); }

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

static void drawRangeChips() {
  for (uint8_t i = 0; i < N_RANGES; i++) {
    int16_t x = X_SYM + i * R_STEP;
    bool on = i == rangeSel;
    gfx->fillRoundRect(x, R_Y, R_W, R_H, 4, on ? C_DIM : C_BG);
    gfx->drawRoundRect(x, R_Y, R_W, R_H, 4, on ? C_MUTED : C_RULE);
    gfx->setTextSize(2);
    gfx->setTextColor(on ? C_FG : C_MUTED);
    gfx->setCursor(x + (R_W - GW(2) * 2) / 2, R_Y + (R_H - GH(2)) / 2);
    gfx->print(RANGES[i].label);
  }
}

static void drawChart(const float *cl, uint8_t n, float prev, uint16_t fg) {
  float lo = prev, hi = prev;
  for (uint8_t i = 0; i < n; i++) {
    lo = min(lo, cl[i]);
    hi = max(hi, cl[i]);
  }
  gfx->drawRect(CH_X - 1, CH_Y - 1, CH_W + 2, CH_H + 2, C_RULE);
  int16_t yp = sparkY(prev, lo, hi, CH_Y, CH_H);
  for (int16_t x = CH_X; x < CH_X + CH_W; x += 6) gfx->drawFastHLine(x, yp, 3, C_MUTED);
  int16_t px = CH_X, py = sparkY(cl[0], lo, hi, CH_Y, CH_H);
  for (uint8_t i = 1; i < n; i++) {
    int16_t nx = CH_X + (int32_t)i * (CH_W - 1) / (n - 1);
    int16_t ny = sparkY(cl[i], lo, hi, CH_Y, CH_H);
    gfx->drawLine(px, py, nx, ny, fg);
    px = nx;
    py = ny;
  }
  // High and low printed inside the box, top-left and bottom-left, over
  // whatever the line does there: the row of chips below wanted the space.
  char b[12];
  formatPrice(hi, b, sizeof b);
  field(CH_X + 2, CH_Y + 2, 7, 1, C_DIM, b);
  formatPrice(lo, b, sizeof b);
  field(CH_X + 2, CH_Y + CH_H - GH(1) - 2, 7, 1, C_DIM, b);
}

static void drawDetail(bool full) {
  Row r = rowCopy(detailIdx);
  if (full) {
    gfx->fillScreen(C_BG);
    cDetail[0] = '\0';
    blitLogo(X_SYM, D_Y_SYM, LOGO_BIG, r.label);  // missing: the square stays black
    drawHint("< next     v list     prev >");
    if (!r.coin) drawRangeChips();
  }
  // The selected range's series: the row's own for 1D, else that range's
  // buffer if it holds this symbol.
  Series sr;
  xSemaphoreTake(mux, portMAX_DELAY);
  sr = series[rangeSel];
  xSemaphoreGive(mux);
  bool mine = rangeSel && sr.idx == detailIdx && sr.range == rangeSel;
  const float *cl = rangeSel ? sr.close : r.close;
  uint8_t n = rangeSel ? (mine ? sr.n : 0) : r.n;
  float prev = rangeSel ? sr.prev : r.prev;

  // Outside the regular session the page shows the newest extended-hours
  // bar as the price, with its move measured from the regular close, and
  // says so where the company name goes. The list keeps the regular figures.
  struct tm t;
  Session ses = getLocalTime(&t, 0) ? session(t) : Session::Regular;
  bool ext = !r.coin && r.valid && ses != Session::Regular && r.last > 0 && r.price > 0 && r.last != r.price;
  float shown = ext ? r.last : r.price, base = ext ? r.price : r.prev;
  float pctv = ext ? (r.last - r.price) / r.price * 100.0f : r.pct;

  char price[12], pct[12], key[48];
  if (r.valid) {
    formatPrice(shown, price, sizeof price);
    formatPct(pctv, pct, sizeof pct);
  } else {
    snprintf(price, sizeof price, "--");
    pct[0] = '\0';
  }
  snprintf(key, sizeof key, "%s|%s|%s|%u|%u|%u|%d|%d", r.label, price, pct, r.n, rangeSel, n, mine && !sr.valid, ext);
  if (strcmp(key, cDetail) == 0) return;
  strcpy(cDetail, key);

  uint16_t fg = !r.valid ? C_DIM : pctv >= 0 ? C_GOOD : C_BAD;
  char name[22];  // what fits beside the logo; a long name is cut, not wrapped
  if (ext) snprintf(name, sizeof name, "%s", ses == Session::Pre ? "pre-market" : "after hours");
  else snprintf(name, sizeof name, "%s", r.coin ? "crypto, 24h change" : r.name);
  field(D_X_TXT, D_Y_SYM, MAX_LABEL, 3, C_FG, r.label);
  field(D_X_TXT, D_Y_NAME, 21, 1, ext ? C_WARN : C_MUTED, name);
  field(D_X_TXT, D_Y_PRICE, 7, 3, fg, price);
  char chg[12] = "";
  if (r.valid && base > 0) snprintf(chg, sizeof chg, "%+.2f", shown - base);
  field(D_X_TXT, D_Y_CHG, 10, 2, fg, chg);
  field(D_X_TXT, D_Y_PCT, 7, 2, fg, pct);

  // Chart: the sparkline's data with room to be a chart. The previous close
  // is a dashed reference line -- the percent is measured from it, so
  // without it the shape means nothing.
  gfx->fillRect(0, CH_Y - 1, 240, CH_H + 2, C_BG);  // chips below are left alone
  gfx->fillRect(0, D_Y_DAY, 240, Y_HINT - 2 - D_Y_DAY, C_BG);
  if (r.coin) {
    field(X_SYM, CH_Y + CH_H / 2 - 4, 30, 1, C_DIM, "no intraday series for coins");
    return;
  }
  if (rangeSel && mine && !sr.valid) {
    char m[24];
    snprintf(m, sizeof m, "no %s series", RANGES[rangeSel].label);
    field(X_SYM, CH_Y + CH_H / 2 - 4, 20, 1, C_DIM, m);
  } else if (rangeSel && n < 2) {
    char m[24];
    snprintf(m, sizeof m, "loading %s...", RANGES[rangeSel].label);
    field(X_SYM, CH_Y + CH_H / 2 - 4, 20, 1, C_DIM, m);
  } else if (!r.valid || n < 2) {
    field(X_SYM, CH_Y + CH_H / 2 - 4, 20, 1, C_DIM, "no series yet");
  } else {
    drawChart(cl, n, prev, fg);
  }
  if (!r.valid) return;
  drawRange(D_Y_DAY, "day", r.dayLo, r.dayHi, r.price);
  drawRange(D_Y_WK, "52w", r.wkLo, r.wkHi, r.price);
}

// ── settings page: swipe right from the list ─────────────────────────────
static uint32_t pageOpenedAt = 0;  // settings and info share the auto-return

static void applySettings() {
  pageMs = SPEED_MS[sSpeed];
  blAuto = sBl == 0 ? cfgBlAuto : false;
  blFixed = sBl == 2 ? blMin : blMax;
  returnMs = RET_MS[sRet];
}
static void loadSettings() {
  prefs.begin("ticker", true);
  bool any = prefs.isKey("speed");
  if (any) {
    sSpeed = prefs.getUChar("speed", sSpeed) % 3;
    sBl = prefs.getUChar("bl", sBl) % 3;
    sRet = prefs.getUChar("ret", sRet) % 3;
    sSound = prefs.getBool("snd", sSound);
    applySettings();
  } else {  // nothing saved yet: the config's own values stand
    for (uint8_t i = 0; i < 3; i++) if (SPEED_MS[i] == pageMs) sSpeed = i;
    for (uint8_t i = 0; i < 3; i++) if (RET_MS[i] == returnMs) sRet = i;
    blAuto = cfgBlAuto;
    blFixed = blMax;
  }
  prefs.end();
  Serial.printf("settings from %s: speed %s, backlight %s, sound %s, return %s\n", any ? "NVS (beats config.json)" : "config.json",
                SPEED_NAMES[sSpeed], BL_NAMES[sBl], sSound ? "on" : "off", RET_NAMES[sRet]);
}
static void saveSettings() {
  prefs.begin("ticker", false);
  prefs.putUChar("speed", sSpeed);
  prefs.putUChar("bl", sBl);
  prefs.putUChar("ret", sRet);
  prefs.putBool("snd", sSound);
  prefs.end();
  applySettings();
}

static void drawSettingRow(uint8_t i) {
  static const char *const labels[] = {"Scroll", "Backlight", "Tap sound", "Auto return", "Info"};
  const char *v = i == 0 ? SPEED_NAMES[sSpeed] : i == 1 ? BL_NAMES[sBl] : i == 2 ? (sSound ? "on" : "off")
                : i == 3 ? RET_NAMES[sRet] : ">";
  int16_t y = S_Y0 + i * S_H;
  field(8, y + 12, 11, 2, C_MUTED, labels[i]);
  fieldRight(X_RIGHT, y + 12, 7, 2, C_FG, v);
  gfx->drawFastHLine(8, y + S_H - 1, 224, C_RULE);
}
static void drawSettings() {
  drawPanel("SETTINGS", C_MUTED, nullptr, 0);
  for (uint8_t i = 0; i < S_N; i++) drawSettingRow(i);
  drawHint("< list");
}
// A tap on row i: cycle it, or open info. Returns true if info was opened.
static bool tapSetting(uint8_t i) {
  if (i == 4) return true;
  if (i == 0) sSpeed = (sSpeed + 1) % 3;
  else if (i == 1) sBl = (sBl + 1) % 3;
  else if (i == 2) sSound = !sSound;
  else sRet = (sRet + 1) % 3;
  saveSettings();
  drawSettingRow(i);
  return false;
}

// ── info page: what the footer used to say, one row of settings ──────────
static void drawInfo(bool full) {
  static uint32_t last = 0;
  if (full) {
    drawPanel("INFO", C_MUTED, nullptr, 0);
    drawHint("< list     v settings");
    last = 0;
  }
  if (millis() - last < 1000) return;
  last = millis();
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  char l[12][40];
  uint8_t n = 0;
  if (haveTime) {
    Session ses = session(t);
    char nx[16];
    nextOpen(t, nx, sizeof nx);
    if (ses == Session::Regular || ses == Session::Post) snprintf(l[n++], 40, "%s", sessionWord(ses));
    else snprintf(l[n++], 40, "%s, opens %s", sessionWord(ses), nx);
  } else {
    snprintf(l[n++], 40, "clock not set yet");
  }
  if (lastOk) snprintf(l[n++], 40, "prices delayed, %lum ago", (unsigned long)((millis() - lastOk) / 60000));
  else snprintf(l[n++], 40, "no prices yet");
  snprintf(l[n++], 40, "refresh %lu / %lu / %lu min", (unsigned long)(stockOpenMs / 60000),
           (unsigned long)(stockExtMs / 60000), (unsigned long)(coinMs / 60000));
  l[n++][0] = '\0';
  snprintf(l[n++], 40, "wifi %.14s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  snprintf(l[n++], 40, "heap %uk free", ESP.getFreeHeap() / 1024);
  uint32_t up = millis() / 1000;
  snprintf(l[n++], 40, "up %luh %02lum", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
  snprintf(l[n++], 40, "ldr %d  backlight %d", ldrRaw, blApplied);
  l[n++][0] = '\0';
  snprintf(l[n++], 40, "%u symbols: %u stocks, %u coins", nRows, nStocks, nCoins);
  snprintf(l[n++], 40, "logos %u/%u badges, %u/%u large", haveLogo[0], nRows, haveLogo[1], nRows);
  snprintf(l[n++], 40, "built " __DATE__ " " __TIME__);
  for (uint8_t i = 0; i < n; i++) field(8, 62 + i * 11, 38, 1, i == 0 ? C_FG : C_MUTED, l[i]);
}

// ── fetching (core 0 task) ───────────────────────────────────────────────
// Yahoo's v8 chart for one symbol at one range/interval, filtered down to the
// meta fields used and the close array. False on any HTTP or parse failure.
static bool fetchChart(NetworkClientSecure &client, const char *id, const char *range, const char *interval,
                       JsonDocument &doc) {
  char url[180];
  snprintf(url, sizeof url, "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=%s&interval=%s", id, range,
           interval);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", UA);  // without this Yahoo answers 429
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d%s\n", id, code, code == 429 ? " (rate limited)" : "");
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
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  if (!doc["chart"]["result"][0]["meta"]["regularMarketPrice"].is<float>()) {
    Serial.printf("%s: no price in payload (bad symbol?)\n", id);
    return false;
  }
  return true;
}

// The close array contains null for gaps and halts. Skip them rather than
// parsing to 0.0, which would drop the line to the floor; keep the LAST cap
// values if there are more.
// ponytail: the line is drawn through a gap; break it there if it matters.
static uint8_t pullCloses(JsonVariant res, float *out, uint8_t cap) {
  uint8_t n = 0;
  for (JsonVariant v : res["indicators"]["quote"][0]["close"].as<JsonArray>()) {
    if (!v.is<float>()) continue;
    if (n == cap) {
      memmove(out, out + 1, (cap - 1) * sizeof(float));
      n--;
    }
    out[n++] = v.as<float>();
  }
  return n;
}

static bool fetchStock(NetworkClientSecure &client, Row &r) {
  JsonDocument doc;
  if (!fetchChart(client, r.id, "1d&includePrePost=true", sparkInterval, doc)) return false;
  JsonVariant res = doc["chart"]["result"][0];
  JsonVariant m = res["meta"];
  r.price = m["regularMarketPrice"] | 0.0f;
  r.prev = m["chartPreviousClose"] | 0.0f;
  r.pct = r.prev > 0 ? (r.price - r.prev) / r.prev * 100.0f : 0.0f;
  r.dayLo = m["regularMarketDayLow"] | 0.0f;
  r.dayHi = m["regularMarketDayHigh"] | 0.0f;
  r.wkLo = m["fiftyTwoWeekLow"] | 0.0f;
  r.wkHi = m["fiftyTwoWeekHigh"] | 0.0f;
  const char *nm = m["longName"] | (m["shortName"] | "");
  snprintf(r.name, sizeof r.name, "%s", nm);
  r.n = pullCloses(res, r.close, SPARK_N);
  r.last = r.n ? r.close[r.n - 1] : r.price;
  r.valid = true;
  return true;
}

// The detail page's range series. A failed fetch still lands (valid=false)
// so the page says "no 1M series" instead of "loading" forever.
static void doSeries(uint8_t idx, uint8_t range) {
  Row r = rowCopy(idx);
  Series s = {};
  s.idx = idx;
  s.range = range;
  if (!r.coin) {
    NetworkClientSecure client;
    client.setInsecure();
    JsonDocument doc;
    if (fetchChart(client, r.id, RANGES[range].range, RANGES[range].interval, doc)) {
      JsonVariant res = doc["chart"]["result"][0];
      s.prev = res["meta"]["chartPreviousClose"] | 0.0f;
      s.n = pullCloses(res, s.close, SERIES_N);
      s.valid = s.n >= 2;
    }
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  series[range] = s;
  xSemaphoreGive(mux);
  Serial.printf("series %s %s: %u points%s (heap %u)\n", r.label, RANGES[range].label, s.n, s.valid ? "" : " FAILED",
                ESP.getFreeHeap());
}
static bool seriesHeld(uint8_t idx, uint8_t range) {  // a byte read; no lock needed
  return series[range].idx == idx && series[range].range == range;
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
    uint8_t want = seriesWant;
    if (want) {  // the page is waiting on these; they jump even the tapped symbol
      uint8_t rg = (want >> rangeSel) & 1 ? rangeSel : (uint8_t)__builtin_ctz(want);
      seriesWant = want & ~(1 << rg);
      doSeries(seriesIdx, rg);
      continue;
    }
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
    // Sweep on the session: every openMinutes in the regular session, every
    // extendedMinutes in pre/post, and when closed exactly ONCE after the
    // close (to keep the final after-hours prints), then not at all until
    // the next pre-market. A boot while closed still gets its first sweep.
    struct tm t;
    bool haveTime = getLocalTime(&t, 0);
    Session ses = haveTime ? session(t) : Session::Regular;
    static Session prevSes = Session::Regular;
    static uint32_t closedAt = 0;
    if (ses != prevSes) {
      prevSes = ses;
      if (ses == Session::Closed) closedAt = millis();
      Serial.printf("session: %s\n", sessionWord(ses));
    }
    uint32_t stockEvery = ses == Session::Regular ? stockOpenMs : stockExtMs;
    if (stockEvery < nStocks * 5000UL) stockEvery = nStocks * 5000UL;  // rate floor
    bool due = lastStock == 0 || millis() - lastStock > stockEvery;
    if (ses == Session::Closed) due = lastStock == 0 || (closedAt && lastStock < closedAt);
    if (!sweeping && nStocks && due) {
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

// ── touch: tap, long press, vertical drag, horizontal swipe ──────────────
// A finger that holds still for 100ms is a tap, fired then and there -- on
// release it felt a beat late, and 100ms is below notice. Still at 450ms
// it is a long press (the list opens a stock on that, so a drag can start
// on a row without opening it). A finger that moves 20px first locks to an
// axis: vertical is a drag, reported as the delta since the last poll;
// horizontal is a swipe if it goes 60px by release. Resistive touch
// jitters a few px, hence 20.
enum class Gesture : uint8_t { None, Tap, LongPress, Drag, SwipeRight, SwipeLeft, SwipeUp, SwipeDown };
static const uint32_t TAP_MS = 100, LONG_MS = 450;
static Gesture pollGesture(int16_t *x, int16_t *y, int16_t *dy) {
  static int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
  static uint32_t t0 = 0, lastTap = 0;
  static uint8_t axis = 0;  // 0 undecided, 1 horizontal, 2 vertical
  static bool tapped = false, longed = false;
  int16_t cx, cy;
  bool now = touchRead(&cx, &cy);
  Gesture g = Gesture::None;
  if (now && !touchHeld) {
    x0 = lx = cx;
    y0 = ly = cy;
    t0 = millis();
    axis = 0;
    tapped = longed = false;
  } else if (now) {
    int16_t ddx = cx - x0, ddy = cy - y0;
    if (!axis && (abs(ddx) > 20 || abs(ddy) > 20)) axis = abs(ddx) > abs(ddy) ? 1 : 2;
    if (axis == 2) {
      *dy = cy - ly;
      g = Gesture::Drag;
    } else if (!axis) {
      *x = x0;
      *y = y0;
      if (!tapped && millis() - t0 >= TAP_MS && millis() - lastTap > 150) {
        tapped = true;
        lastTap = millis();
        g = Gesture::Tap;
      } else if (tapped && !longed && millis() - t0 >= LONG_MS) {
        longed = true;
        g = Gesture::LongPress;
      }
    }
    lx = cx;
    ly = cy;
  } else if (touchHeld) {  // release
    int16_t ddx = lx - x0, ddy = ly - y0;
    if (axis == 1) {
      if (ddx > 60 && abs(ddy) < 60) g = Gesture::SwipeRight;
      else if (ddx < -60 && abs(ddy) < 60) g = Gesture::SwipeLeft;
    } else if (axis == 2) {  // a vertical drag that went far enough is also a swipe; the list ignores it
      if (ddy > 60 && abs(ddx) < 60) g = Gesture::SwipeDown;
      else if (ddy < -60 && abs(ddx) < 60) g = Gesture::SwipeUp;
    } else if (!axis && !tapped && millis() - lastTap > 150) {  // a tap quicker than TAP_MS
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::Tap;
    }
  }
  touchHeld = now;
  return g;
}

// ── brightness from the LDR ──────────────────────────────────────────────
// Calibration knob: ldrDark is the raw reading treated as fully dark. The
// sensor read ~0 in room light on this unit; if the panel dims in a bright
// room instead, the sense is inverted and this mapping needs flipping.
static void brightnessTick(bool open) {
  static uint32_t last = 0;
  static float ema = -1;
  int &applied = blApplied;
  if (millis() - last < 1000) return;
  last = millis();
  int ldr = analogRead(LDR);
  ldrRaw = ldr;
  if (ema < 0) ema = ldr;
  ema += (ldr - ema) * 0.3f;
  int lvl = blFixed;
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
  t.tm_wday = 3; t.tm_hour = 3; t.tm_min = 59;  assert(session(t) == Session::Closed);
  t.tm_hour = 4; t.tm_min = 0;                  assert(session(t) == Session::Pre);
  t.tm_hour = 9; t.tm_min = 29;                 assert(!marketOpen(t) && session(t) == Session::Pre);
  t.tm_min = 30;                                assert(marketOpen(t));
  t.tm_hour = 16; t.tm_min = 0;                 assert(!marketOpen(t) && session(t) == Session::Post);
  t.tm_hour = 20;                               assert(session(t) == Session::Closed);
  t.tm_hour = 12; t.tm_wday = 6;                assert(session(t) == Session::Closed);
  char nx[24];
  t.tm_wday = 6; t.tm_hour = 12;  nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "Mon 09:30") == 0);
  t.tm_wday = 5; t.tm_hour = 17;  nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "Mon 09:30") == 0);
  t.tm_wday = 2; t.tm_hour = 5;   nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "09:30") == 0);

  assert(nRows == 0);
  assert(!addRow("TOOLONG", "TOOLONG", false));
  assert(!addRow("", "X", false));
  assert(!addRow("X", nullptr, false));
  for (uint8_t i = 0; i < MAX_SYMBOLS; i++) assert(addRow("AAA", "aaa", false));
  assert(!addRow("BBB", "bbb", false));
  nRows = 0;

  // Hit tests: the head and footer are not rows, every row maps to itself.
  // Ring hit tests on a 26-row list: row 0 at the top at pos 0, the entering
  // row at the bottom edge once scrolled, the wrap, and a drag back past 0.
  assert(modp(-1, 26) == 25 && floordiv(-1, ROW_H) == -1 && floordiv(ROW_H, ROW_H) == 1);
  assert(hitRow(Y_ROW0 - 1, 0, 26) == -1 && hitRow(Y_ROW0, 0, 26) == 0 && hitRow(Y_ROW0, 0, 0) == -1);
  assert(hitRow(Y_ROW0 + RING - 1, 0, 26) == ROWS - 1 && hitRow(Y_ROW0 + RING, 0, 26) == -1);
  assert(hitRow(Y_ROW0, ROW_H - 1, 26) == 0 && hitRow(Y_ROW0 + 1, ROW_H - 1, 26) == 1);
  assert(hitRow(Y_ROW0 + RING - 1, ROW_H - 1, 26) == ROWS);
  assert(hitRow(Y_ROW0 + RING - 1, 24 * ROW_H + ROW_H - 1, 26) == (24 + ROWS) % 26);
  assert(hitRow(Y_ROW0, -1, 26) == 25 && hitRow(Y_ROW0, 26 * ROW_H, 26) == 0);
  assert(hitRange(X_SYM, CH_Y + CH_H) == 0 && hitRange(X_SYM + R_STEP * 4, D_Y_DAY - 1) == 4);
  assert(hitRange(X_SYM, CH_Y + CH_H - 1) == -1 && hitRange(X_SYM, D_Y_DAY) == -1);
  assert(hitRange(X_SYM - 1, R_Y) == -1 && hitRange(X_SYM + R_STEP * 5, R_Y) == -1);
  // Sparkline scaling: extremes hit the box edges, a flat series sits mid-box.
  assert(sparkY(10, 10, 20, 100, 28) == 127 && sparkY(20, 10, 20, 100, 28) == 100);
  assert(sparkY(5, 5, 5, 100, 28) == 114);

  // Geometry: badge, symbol, sparkline and price never overlap; rows clear the footer.
  assert(X_SYM + LOGO_BADGE <= X_LBL);
  assert(Y_BADGE + LOGO_BADGE <= ROW_H - 4 && Y_LBL + GH(2) <= ROW_H - 4);
  assert(X_LBL + GW(2) * MAX_LABEL <= X_SPK);
  assert(X_SPK + SPK_W <= X_RIGHT - GW(2) * 7);
  assert(Y_SPK + SPK_H <= ROW_H - 4);
  assert(Y_ROW0 + RING <= LCD_H);  // the ring plus header fills the panel; nothing below it
  assert(72 + GW(1) * 18 <= X_RIGHT - GW(1) * 5);
  assert(S_Y0 + S_H * S_N <= Y_HINT && hitSetting(S_Y0 - 1) == -1 && hitSetting(S_Y0) == 0);
  assert(hitSetting(S_Y0 + S_H * S_N - 1) == S_N - 1 && hitSetting(S_Y0 + S_H * S_N) == -1);
  // Detail: the logo and the text column beside it, then the chart labels
  // and the buttons, all fit.
  assert(X_SYM + LOGO_BIG <= D_X_TXT && D_Y_SYM + LOGO_BIG <= CH_Y);
  assert(D_X_TXT + GW(3) * 7 <= 240 && D_X_TXT + GW(1) * 21 <= 240 && D_X_TXT + GW(2) * 10 <= 240);
  assert(D_Y_SYM + GH(3) <= D_Y_NAME && D_Y_NAME + GH(1) <= D_Y_PRICE);
  assert(D_Y_PRICE + GH(3) <= D_Y_CHG && D_Y_CHG + GH(2) <= D_Y_PCT && D_Y_PCT + GH(2) <= CH_Y);
  // Range chips fill the strip between the chart and the day bar.
  assert(CH_Y + CH_H <= R_Y && R_Y + R_H <= D_Y_DAY && R_W <= R_STEP && GW(2) * 2 <= R_W && GH(2) <= R_H);
  assert(X_SYM + R_STEP * (N_RANGES - 1) + R_W <= LCD_W);
  assert(D_Y_DAY + 10 + GH(1) <= D_Y_WK && D_Y_WK + 10 + GH(1) <= Y_HINT && Y_HINT + GH(1) <= LCD_H);
  assert(sizeof logoBuf >= (size_t)LOGO_BADGE * LOGO_BADGE * 2);
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoConfig, NoWifi, NoData, Running };
enum class View { List, Detail, Settings, Info };
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
  rowCanvas = new Arduino_Canvas(LCD_W, ROW_H, nullptr);
  if (!rowCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) Serial.println("row canvas: alloc failed");  // 20KB
  backlight(blMax);

  if (!loadConfig()) {
    Serial.printf("config error: %s\n", cfgErr);
    return;  // loop() draws the panel
  }
  cfgRelease();
  logoInventory();
  loadSettings();

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
  struct tm t;
  if (refreshOnOpen && !(getLocalTime(&t, 0) && session(t) == Session::Closed)) priority = idx;  // nothing new when closed
  seriesIdx = idx;
  uint8_t want = 0;
  for (uint8_t rg = 1; rg < N_RANGES; rg++)
    if (!rows[idx].coin && !seriesHeld(idx, rg)) want |= 1 << rg;
  seriesWant = want;  // all four, selected first; the range sticks across PREV/NEXT
  panelScroll(0);     // the list's scroll offset must not shift this page
  drawDetail(true);
}

static void backToList() {
  view = View::List;
  listStart();
}

void loop() {
  int16_t tx, ty;
  int16_t ddy = 0;
  Gesture g = pollGesture(&tx, &ty, &ddy);
  bool tap = g == Gesture::Tap;
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
    panelScroll(0);  // panels draw the screen 1:1
    if (state == State::Running) {
      if (view == View::List) listStart();
      else if (view == View::Settings) drawSettings();
      else if (view == View::Info) drawInfo(true);
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
    listTick();
  } else if (view == View::Info) {
    drawInfo(false);
  } else if (view == View::Detail) {
    drawDetail(false);
  }

  // On the list: drag scrolls it, any touch holds the crawl for 3s after,
  // and a long press opens the stock. Swipe right opens settings; swipe
  // left there goes back.
  if (state == State::Running && view == View::List) {
    if (touchHeld) holdUntil = millis() + 3000;
    if (g == Gesture::Drag && ddy) scrollBy(-ddy);
    if (g == Gesture::LongPress) {
      int16_t r = hitRow(ty, pos, nRows);
      if (r >= 0) {
        if (sSound) tone(SPK, 1200, 15);
        openDetail((uint8_t)r);
      }
    }
  }
  // Swipes are the navigation, phone style. List: right opens settings.
  // Detail: left is the next stock, right the previous, down the list.
  // Settings: left is the list. Info: left the list, down settings.
  bool swipe = g == Gesture::SwipeLeft || g == Gesture::SwipeRight || g == Gesture::SwipeDown || g == Gesture::SwipeUp;
  if (swipe && state == State::Running) {
    bool acts = (view == View::List && g == Gesture::SwipeRight) ||
                (view == View::Detail && g != Gesture::SwipeUp) ||
                (view == View::Settings && g == Gesture::SwipeLeft) ||
                (view == View::Info && (g == Gesture::SwipeLeft || g == Gesture::SwipeDown));
    if (acts && sSound) tone(SPK, 1200, 15);
    if (view == View::List && g == Gesture::SwipeRight) {
      view = View::Settings;
      pageOpenedAt = millis();
      panelScroll(0);
      drawSettings();
    } else if (view == View::Detail) {
      if (g == Gesture::SwipeLeft) openDetail((detailIdx + 1) % nRows);
      else if (g == Gesture::SwipeRight) openDetail((detailIdx + nRows - 1) % nRows);
      else if (g == Gesture::SwipeDown) backToList();
    } else if (view == View::Settings && g == Gesture::SwipeLeft) {
      backToList();
    } else if (view == View::Info) {
      if (g == Gesture::SwipeLeft) backToList();
      else if (g == Gesture::SwipeDown) {
        view = View::Settings;
        pageOpenedAt = millis();
        drawSettings();
      }
    }
  }

  if (tap && state == State::Running && view != View::List) {
    if (sSound) tone(SPK, 1200, 15);
    if (view == View::Settings) {
      int8_t i = hitSetting(ty);
      pageOpenedAt = millis();
      if (i >= 0 && tapSetting((uint8_t)i)) {
        view = View::Info;
        drawInfo(true);
      }
    } else if (view == View::Info) {
      pageOpenedAt = millis();  // a tap only keeps it open
    } else {
      int8_t rg = hitRange(tx, ty);
      if (rg >= 0 && !rows[detailIdx].coin && rg != rangeSel) {
        rangeSel = (uint8_t)rg;
        if (rg && !seriesHeld(detailIdx, rg)) seriesWant = seriesWant | (1 << rg);  // failed earlier: retry
        drawRangeChips();
        cDetail[0] = '\0';  // the chart must redraw for the new range
      }
    }
    Serial.printf("tap %d,%d -> view %u %s\n", tx, ty, (unsigned)view, view == View::Detail ? rows[detailIdx].label : "");
  }

  if (view == View::Detail && returnMs && !touchHeld && millis() - detailOpenedAt > returnMs) backToList();
  if ((view == View::Info || view == View::Settings) && returnMs && !touchHeld && millis() - pageOpenedAt > returnMs)
    backToList();

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
