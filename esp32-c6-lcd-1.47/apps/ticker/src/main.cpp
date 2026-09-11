// ticker: a watchlist panel -- a few stocks and a few crypto, price and daily
// change, green or red.
//
// The watchlist lives in data/watchlist.json on the device's LittleFS
// partition, NOT in this file. Editing symbols is an `uploadfs`, not a rebuild.
//
// The API shapes were measured before any of this was written, and they dictate
// the design (see README):
//   stocks  Yahoo v8 chart, no key, ~1.3KB, ONE SYMBOL PER REQUEST, and it
//           returns HTTP 429 unless a User-Agent is sent.
//   crypto  CoinGecko simple/price, no key, all coins in one ~500 byte request.
//
// Follows APP-CHECKLIST.md: ui.h for scheme/rotation/gestures, a Layout per
// orientation, exact-box redraws, a self-check over the pure formatters and both
// layouts, and failure screens that name the cause rather than going blank.
#include <board.h>
#include <netjoin.h>
#include <ui.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── config ───────────────────────────────────────────────────────────────
static const char *TZ_STRING = "EST5EDT,M3.2.0/2,M11.1.0/2";  // also US market time

// Six rows is the readable budget on a 172px panel at text size 2, and size 1
// is the density already rejected as too small to read. So six is a layout
// fact, asserted in checkLayout() -- but it caps rows ON SCREEN, not symbols:
// a longer list pages through six at a time.
static const uint8_t ROWS_PER_PAGE = 6;
static const uint8_t MAX_SYMBOLS = 24;
static const uint8_t MAX_LABEL = 5;  // glyphs; the symbol box is 5 wide at size 2

static const uint32_t PAGE_MS = 8000;   // how long each page stays up
static const uint16_t CASCADE_MS = 22;  // per-row stagger when a page flips

// Yahoo 429s an anonymous client, so this is required rather than polite.
static const char *UA = "Mozilla/5.0 (esp32-ticker)";

static const uint32_t STOCK_OPEN_MS = 5UL * 60 * 1000;   // market open: every 5 min
static const uint32_t STOCK_SHUT_MS = 60UL * 60 * 1000;  // closed: hourly is plenty
static const uint32_t COIN_MS = 5UL * 60 * 1000;         // crypto never closes
static const uint32_t WIFI_RETRY_MS = 20UL * 1000;
static const uint32_t LOG_MS = 60UL * 1000;

#define POWER_SAVE 1

#define GW(s) (6 * (s))
#define GH(s) (8 * (s))

static Arduino_GFX *gfx;

// ── the watchlist, loaded from LittleFS ──────────────────────────────────
struct Row {
  char label[MAX_LABEL + 1];  // what the screen shows
  char id[28];                // Yahoo symbol, or CoinGecko id
  bool coin;
  float price, pct;
  bool valid;
};
static Row rows[MAX_SYMBOLS];
static uint8_t nRows = 0, nStocks = 0, nCoins = 0;
static uint8_t page = 0, nPages = 1;
static char coinIds[MAX_SYMBOLS * 28];  // comma-joined, for the one coin request
static const char *cfgErr = nullptr;
static char cfgErrDetail[40] = "";

// Stocks are stored first so the divider between the two groups is a single
// index, and so the sequential stock fetches are one contiguous loop.
static bool addRow(const char *label, const char *id, bool coin) {
  if (nRows >= MAX_SYMBOLS) return false;
  if (!label || !*label || strlen(label) > MAX_LABEL) return false;
  if (!id || !*id) return false;
  Row &r = rows[nRows++];
  snprintf(r.label, sizeof r.label, "%s", label);
  snprintf(r.id, sizeof r.id, "%s", id);
  r.coin = coin;
  r.valid = false;
  return true;
}

static bool loadWatchlist() {
  if (!LittleFS.begin()) {
    cfgErr = "no filesystem";
    return false;
  }
  File f = LittleFS.open("/watchlist.json", "r");
  if (!f) {
    cfgErr = "watchlist.json missing";
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) {
    cfgErr = "watchlist.json unreadable";
    snprintf(cfgErrDetail, sizeof cfgErrDetail, "%s", e.c_str());
    return false;
  }

  // Stocks first, then coins -- addRow() enforces the cap, so an over-long list
  // is truncated rather than overflowing. Say which rows were dropped.
  for (JsonVariant v : doc["stocks"].as<JsonArray>()) {
    const char *s = v.as<const char *>();
    if (addRow(s, s, false)) nStocks++;
    else Serial.printf("skipped stock '%s' (bad label or list full)\n", s ? s : "?");
  }
  for (JsonObject o : doc["coins"].as<JsonArray>()) {
    const char *id = o["id"], *label = o["label"];
    if (addRow(label ? label : id, id, true)) nCoins++;
    else Serial.printf("skipped coin '%s' (bad label or list full)\n", id ? id : "?");
  }

  if (nRows == 0) {
    cfgErr = "watchlist is empty";
    return false;
  }

  coinIds[0] = '\0';
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    if (coinIds[0]) strlcat(coinIds, ",", sizeof coinIds);
    strlcat(coinIds, rows[i].id, sizeof coinIds);
  }
  nPages = (nRows + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
  Serial.printf("watchlist: %u stocks + %u coins over %u page(s) (%s)\n", nStocks, nCoins,
                nPages, coinIds[0] ? coinIds : "no coins");
  return true;
}

// ── layout ───────────────────────────────────────────────────────────────
struct Layout {
  bool landscape;
  int16_t w, h;
  int16_t yHead;
  int16_t xRow[2], rowW;  // one column in portrait, two in landscape
  int16_t yRow0, rowPitch;
  uint8_t perCol;
  int16_t yFoot0, footStep;
};

static const Layout PORTRAIT = {
    false, 172, 320,
    /*head  */ 6,
    /*cols  */ {8, 0}, 156,
    /*rows  */ 30, 38,
    /*perCol*/ 6,
    /*foot  */ 264, 15,
};

static const Layout LANDSCAPE = {
    true, 320, 172,
    /*head  */ 4,
    /*cols  */ {8, 166}, 146,
    /*rows  */ 26, 40,
    /*perCol*/ 3,
    // Only 132..140 is legal here: below it the footer lands on row 3, above it
    // the third footer line runs off the 172px bottom. Both ends are asserted.
    /*foot  */ 134, 12,
};

static const Layout *L = &PORTRAIT;
static void syncLayout() { L = uiLandscape() ? &LANDSCAPE : &PORTRAIT; }

static uint32_t lastStock = 0, lastCoin = 0, lastOk = 0;
static uint16_t failures = 0;

// ── pure formatters (what selfCheck covers) ──────────────────────────────
// Price must fit 7 characters: anything wider collides with the symbol on its
// left. So precision shrinks as the number grows -- 0.4215 and 79010 have to
// fit the same box.
static void formatPrice(float v, char *out, size_t n) {
  float a = fabsf(v);
  if (a < 1.0f) snprintf(out, n, "%.4f", v);
  else if (a < 100.0f) snprintf(out, n, "%.2f", v);
  else if (a < 10000.0f) snprintf(out, n, "%.1f", v);
  else if (a < 1000000.0f) snprintf(out, n, "%.0f", v);
  else snprintf(out, n, "%.0fk", v / 1000.0f);
}

// Always signed, so green/red is not the only cue -- colour alone fails for
// anyone who cannot distinguish it. Must fit 7 characters.
static void formatPct(float p, char *out, size_t n) {
  if (fabsf(p) >= 100.0f) snprintf(out, n, "%+.0f%%", p);
  else snprintf(out, n, "%+.1f%%", p);
}

// US market hours in local time. The board runs NTP with TZ set to Eastern, so
// this needs no conversion -- worth stating because it breaks silently if
// TZ_STRING is ever changed to a non-Eastern zone.
static bool marketOpen(const struct tm &t) {
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;  // weekend
  int mins = t.tm_hour * 60 + t.tm_min;
  return mins >= (9 * 60 + 30) && mins < (16 * 60);
}

// ── drawing ──────────────────────────────────────────────────────────────
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg,
                  const char *s) {
  gfx->fillRect(x, y, GW(size) * chars, GH(size), uiTheme()->bg);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size, uint16_t fg,
                       const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, w, GH(size), uiTheme()->bg);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(right - (int16_t)(GW(size) * strlen(s)), y);
  gfx->print(s);
}

// Cached per SLOT on screen, not per symbol: a page flip changes which symbol
// occupies a slot, so the cache has to be invalidated when the page turns.
static char cRow[ROWS_PER_PAGE][28], cHead[20], cFoot[3][40];

static void invalidateCache() {
  for (uint8_t i = 0; i < ROWS_PER_PAGE; i++) cRow[i][0] = '\0';
  cHead[0] = '\0';
  for (uint8_t i = 0; i < 3; i++) cFoot[i][0] = '\0';
}

// slot is the position on screen (0..ROWS_PER_PAGE-1), not the symbol index.
static void slotPos(uint8_t slot, int16_t *x, int16_t *y, int16_t *right) {
  uint8_t col = L->landscape ? slot / L->perCol : 0;
  uint8_t idx = L->landscape ? slot % L->perCol : slot;
  *x = L->xRow[col];
  *y = L->yRow0 + idx * L->rowPitch;
  *right = *x + L->rowW;
}

static void drawChrome() {
  gfx->fillScreen(uiTheme()->bg);
  field(8, L->yHead, 9, 1, uiTheme()->muted, "WATCHLIST");
  if (nPages > 1) {  // say where you are, or a flip looks like a data change
    char p[12];
    snprintf(p, sizeof p, "%u/%u", page + 1, nPages);
    field(76, L->yHead, 6, 1, uiTheme()->dim, p);
  }
  // The divider marks where stocks end and coins begin -- they behave
  // differently (one closes, one doesn't). It only exists if that boundary
  // actually falls inside the page being shown.
  uint8_t first = page * ROWS_PER_PAGE;
  if (!L->landscape && nStocks > first && nStocks < first + ROWS_PER_PAGE)
    gfx->drawFastHLine(8, L->yRow0 + (nStocks - first) * L->rowPitch - 8, L->w - 16,
                       uiTheme()->rule);
}

// cascade: stagger the slots so a page flip reads as movement rather than a
// blink. It costs nothing extra -- the same redraws, just spaced out -- which
// is why it beats a pixel-wise slide, where every frame repaints the whole
// row region and the single core has to push all of it over SPI.
static void drawRows(bool cascade = false) {
  char price[12], pct[12], key[28];
  for (uint8_t slot = 0; slot < ROWS_PER_PAGE; slot++) {
    int16_t x, y, right;
    slotPos(slot, &x, &y, &right);
    uint8_t i = page * ROWS_PER_PAGE + slot;

    // A short last page leaves empty slots; they must be cleared, or the
    // previous page's symbols stay on screen looking current.
    if (i >= nRows) {
      if (cRow[slot][0] == '\0') continue;
      cRow[slot][0] = '\0';
      gfx->fillRect(x, y, L->rowW, 18 + GH(1), uiTheme()->bg);
      continue;
    }

    if (rows[i].valid) {
      formatPrice(rows[i].price, price, sizeof price);
      formatPct(rows[i].pct, pct, sizeof pct);
    } else {
      snprintf(price, sizeof price, "--");
      pct[0] = '\0';
    }
    snprintf(key, sizeof key, "%s|%s|%s", rows[i].label, price, pct);
    if (strcmp(key, cRow[slot]) == 0) continue;
    strcpy(cRow[slot], key);

    uint16_t fg = !rows[i].valid     ? uiTheme()->dim
                  : rows[i].pct >= 0 ? uiTheme()->good
                                     : uiTheme()->bad;
    field(x, y, MAX_LABEL, 2, uiTheme()->fg, rows[i].label);
    fieldRight(right, y, 7, 2, fg, price);
    fieldRight(right, y + 18, 7, 1, fg, pct);
    if (cascade) delay(CASCADE_MS);
  }
}

static void turnPage(uint8_t to) {
  page = to;
  invalidateCache();
  if (!uiScreenOn()) return;
  drawChrome();
  drawRows(true);
}

static void drawHead(const struct tm *t, bool haveTime) {
  char buf[20];
  if (haveTime) strftime(buf, sizeof buf, "%H:%M", t);
  else snprintf(buf, sizeof buf, "--:--");
  if (strcmp(buf, cHead) == 0) return;
  strcpy(cHead, buf);
  fieldRight(L->w - 8, L->yHead, 5, 1, uiTheme()->muted, buf);
}

static void drawFooter(const struct tm *t, bool haveTime) {
  char buf[40];
  bool open = haveTime && marketOpen(*t);

  snprintf(buf, sizeof buf, "market %s", !haveTime ? "?" : open ? "open" : "closed");
  if (strcmp(buf, cFoot[0]) != 0) {
    strcpy(cFoot[0], buf);
    field(8, L->yFoot0, 20, 1, open ? uiTheme()->good : uiTheme()->muted, buf);
  }
  // Say it is delayed rather than implying live prices.
  snprintf(buf, sizeof buf, "delayed, %lum ago",
           lastOk ? (unsigned long)((millis() - lastOk) / 60000) : 0UL);
  if (strcmp(buf, cFoot[1]) != 0) {
    strcpy(cFoot[1], buf);
    field(8, L->yFoot0 + L->footStep, 20, 1, uiTheme()->dim, buf);
  }
  snprintf(buf, sizeof buf, "%.9s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  if (strcmp(buf, cFoot[2]) != 0) {
    strcpy(cFoot[2], buf);
    field(8, L->yFoot0 + L->footStep * 2, 20, 1, uiTheme()->muted, buf);
  }
}

static void drawPanel(const char *title, uint16_t tc, const char *const *lines, uint8_t n) {
  gfx->fillScreen(uiTheme()->bg);
  uint8_t cols = (L->w / GW(1)) - 2;
  field(8, 24, cols, 3, tc, title);
  gfx->drawFastHLine(8, 54, L->w - 16, uiTheme()->rule);
  uint8_t maxRows = (L->h - 62 - 16) / 11;
  for (uint8_t i = 0; i < n && i < maxRows; i++)
    field(8, 62 + i * 11, cols, 1, uiTheme()->muted, lines[i]);
}

// ── fetching ─────────────────────────────────────────────────────────────
// One reused TLS client for the sequential stock requests: each session costs
// ~40KB of a 512KB heap, so they cannot overlap.
static bool fetchStock(NetworkClientSecure &client, Row &r) {
  char url[180];
  snprintf(url, sizeof url,
           "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=1d",
           r.id);

  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", UA);  // without this Yahoo answers 429

  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d%s\n", r.id, code,
                  code == 429 ? " (rate limited -- backing off)" : "");
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  JsonVariant m = doc["chart"]["result"][0]["meta"];
  if (!m["regularMarketPrice"].is<float>()) {
    // A misspelled symbol lands here, so name it -- otherwise the row just sits
    // at "--" and looks like a network fault.
    Serial.printf("%s: no price in payload (bad symbol?)\n", r.id);
    return false;
  }
  float price = m["regularMarketPrice"] | 0.0f;
  float prev = m["chartPreviousClose"] | 0.0f;
  r.price = price;
  r.pct = prev > 0 ? (price - prev) / prev * 100.0f : 0.0f;
  r.valid = true;
  return true;
}

static bool fetchCoins(NetworkClientSecure &client) {
  if (!coinIds[0]) return false;
  char url[sizeof coinIds + 128];
  snprintf(url, sizeof url,
           "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd"
           "&include_24hr_change=true",
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
    rows[i].price = v["usd"] | 0.0f;
    rows[i].pct = v["usd_24h_change"] | 0.0f;
    rows[i].valid = true;
    any = true;
  }
  return any;
}

// ── self-check ───────────────────────────────────────────────────────────
static void checkLayout(const Layout *l) {
  uint8_t cols = l->landscape ? 2 : 1;
  // Both orientations must hold exactly one page, or a slot would have no
  // position and the page arithmetic would silently drop symbols.
  assert(cols * l->perCol == ROWS_PER_PAGE);
  for (uint8_t c = 0; c < cols; c++) assert(l->xRow[c] + l->rowW <= l->w);
  assert(GW(2) * MAX_LABEL <= l->rowW - GW(2) * 7);  // symbol cannot reach price
  // last row, including the percent line beneath it
  int16_t rowsEnd = l->yRow0 + (l->perCol - 1) * l->rowPitch + 18 + GH(1);
  assert(rowsEnd <= l->h);
  assert(l->yFoot0 >= rowsEnd);  // footer must not land on the last row
  assert(l->yFoot0 + l->footStep * 2 + GH(1) <= l->h);
  assert((l->w / GW(1)) - 2 >= 22);    // panel text width
  assert((l->h - 62 - 16) / 11 >= 7);  // panel line count
}

static void selfCheck() {
  char b[16];
  // Price has a 7-character box, and these are the shapes that stress it.
  formatPrice(0.4215f, b, sizeof b);  assert(strcmp(b, "0.4215") == 0);
  formatPrice(12.5f, b, sizeof b);    assert(strcmp(b, "12.50") == 0);
  formatPrice(334.35f, b, sizeof b);  assert(strcmp(b, "334.4") == 0);
  formatPrice(79010.0f, b, sizeof b); assert(strcmp(b, "79010") == 0);
  for (float v : {0.0f, 0.0001f, 1.0f, 99.99f, 999.9f, 9999.0f, 79010.0f, 1.5e6f, 9.9e7f}) {
    formatPrice(v, b, sizeof b);
    assert(strlen(b) <= 7);
  }

  // Values chosen away from the rounding boundary on purpose: 2.35f is really
  // 2.3499999, so it formats as "+2.3%" and an assert expecting "+2.4%" fails
  // on the float, not on the code. Don't test what a half-way literal rounds to.
  formatPct(2.44f, b, sizeof b);   assert(strcmp(b, "+2.4%") == 0);
  formatPct(-12.34f, b, sizeof b); assert(strcmp(b, "-12.3%") == 0);
  formatPct(0.0f, b, sizeof b);    assert(strcmp(b, "+0.0%") == 0);
  for (float p : {0.0f, 0.04f, -9.99f, 99.9f, -100.0f, 1234.0f}) {
    formatPct(p, b, sizeof b);
    assert(strlen(b) <= 7);
  }

  // Market hours. tm_wday: 0 = Sunday.
  struct tm t = {};
  t.tm_wday = 3; t.tm_hour = 9; t.tm_min = 29;  assert(!marketOpen(t));
  t.tm_min = 30;                                assert(marketOpen(t));
  t.tm_hour = 15; t.tm_min = 59;                assert(marketOpen(t));
  t.tm_hour = 16; t.tm_min = 0;                 assert(!marketOpen(t));
  t.tm_hour = 12; t.tm_wday = 6;                assert(!marketOpen(t));
  t.tm_wday = 0;                                assert(!marketOpen(t));

  // The watchlist file is user-editable, so addRow() is now a trust boundary:
  // an over-long label would overflow the symbol box, and the row cap is a
  // layout guarantee. Both have to hold against whatever the JSON says.
  assert(nRows == 0);  // runs before loadWatchlist()
  assert(!addRow("TOOLONG", "TOOLONG", false));
  assert(!addRow("", "X", false));
  assert(!addRow("X", "", false));
  assert(!addRow("X", nullptr, false));
  for (uint8_t i = 0; i < MAX_SYMBOLS; i++) assert(addRow("AAA", "aaa", false));
  assert(!addRow("BBB", "bbb", false));  // cap holds

  // Paging arithmetic: every symbol must land on exactly one page, and the
  // last page is the one that is allowed to be short.
  for (uint8_t n = 1; n <= MAX_SYMBOLS; n++) {
    uint8_t pages = (n + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    assert(pages >= 1 && pages * ROWS_PER_PAGE >= n);
    assert((pages - 1) * ROWS_PER_PAGE < n);  // no trailing all-empty page
  }
  nRows = 0;

  checkLayout(&PORTRAIT);
  checkLayout(&LANDSCAPE);
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoConfig, NoWifi, NoData, Running };
static State state = State::Boot;

void setup() {
  Serial.begin(115200);
  delay(300);
  selfCheck();

  gfx = boardDisplay();
  gfx->begin();
  uiBegin(gfx, "ticker");
  syncLayout();

  if (!loadWatchlist()) {
    Serial.printf("config error: %s %s\n", cfgErr, cfgErrDetail);
    return;  // loop() draws the panel; nothing else can usefully run
  }

  const char *boot[] = {"connecting to wifi", WIFI_SSID};
  drawPanel("STARTING", uiTheme()->muted, boot, 2);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinBest(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
    configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    for (int i = 0; i < 40 && !getLocalTime(&t, 250); i++) {}
  }
#if POWER_SAVE
  setCpuFrequencyMhz(80);
#endif
}

static void drawFailPanel() {
  static char key[32];
  char k[32];
  snprintf(k, sizeof k, "%d-%u-%u", (int)state, failures, uiScheme());
  if (strcmp(k, key) == 0) return;
  strcpy(key, k);

  if (state == State::NoConfig) {
    const char *l[] = {cfgErr,
                       cfgErrDetail,
                       "",
                       "upload the watchlist:",
                       "PLATFORMIO_DATA_DIR=",
                       " apps/ticker/data",
                       " pio run -e ticker",
                       " -t uploadfs"};
    drawPanel("NO LIST", uiTheme()->warn, l, 8);
  } else if (state == State::NoWifi) {
    static char ssid[40];
    snprintf(ssid, sizeof ssid, "SSID %s", WIFI_SSID);
    const char *l[] = {ssid, "", "not associated.", "this board is",
                       "2.4GHz only.", "", "retrying..."};
    drawPanel("NO WIFI", uiTheme()->bad, l, 7);
  } else {
    static char f[34];
    snprintf(f, sizeof f, "%u failed fetches", failures);
    const char *l[] = {"no quotes yet", f,          "",
                       "check the serial", "log for the http", "code -- yahoo",
                       "rate-limits hard.", "",     "retrying..."};
    drawPanel("NO DATA", uiTheme()->warn, l, 9);
  }
}

void loop() {
  uiTick();
  if (uiHandle(uiPoll())) {
    syncLayout();
    invalidateCache();
    state = State::Boot;  // forces a full repaint below
  }

  struct tm t;
  bool haveTime = getLocalTime(&t, 0);

  bool anyValid = false;
  for (uint8_t i = 0; i < nRows; i++) anyValid |= rows[i].valid;
  State want = cfgErr                          ? State::NoConfig
               : WiFi.status() != WL_CONNECTED ? State::NoWifi
               : !anyValid                     ? State::NoData
                                               : State::Running;

  if (want != state) {
    state = want;
    invalidateCache();
    if (uiScreenOn() && state == State::Running) drawChrome();
  }

  if (state == State::NoConfig) {
    if (uiScreenOn()) drawFailPanel();
    delay(50);
    return;  // no watchlist means nothing to fetch and nothing to show
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

  if (uiScreenOn()) {
    if (state == State::Running) {
      drawHead(&t, haveTime);
      drawRows();
      drawFooter(&t, haveTime);
    } else {
      drawFailPanel();
    }
  }

  static uint32_t lastPage = 0;
  if (state == State::Running && nPages > 1 && millis() - lastPage > PAGE_MS) {
    lastPage = millis();
    turnPage((page + 1) % nPages);
  }

  // Crypto and stocks on separate clocks: one market closes, the other never
  // does, and polling a shut exchange every five minutes only risks the 429.
  if (state != State::NoWifi) {
    uint32_t stockEvery = (haveTime && marketOpen(t)) ? STOCK_OPEN_MS : STOCK_SHUT_MS;
    // One HTTPS request per stock, so a long list must not become a burst --
    // 20 symbols every 5 min is 240 requests/hour and Yahoo will start
    // answering 429. Hold the floor at roughly one request a minute.
    if (stockEvery < nStocks * 60000UL) stockEvery = nStocks * 60000UL;
    bool doStocks = nStocks && (lastStock == 0 || millis() - lastStock > stockEvery);
    bool doCoins = nCoins && (lastCoin == 0 || millis() - lastCoin > COIN_MS);

    if (doStocks || doCoins) {
      NetworkClientSecure client;
      client.setInsecure();  // public read-only quotes; pinning buys nothing here
      bool ok = false;

      if (doCoins) {
        lastCoin = millis();
        ok |= fetchCoins(client);
      }
      if (doStocks) {
        lastStock = millis();
        for (uint8_t i = 0; i < nRows; i++) {
          if (rows[i].coin) continue;
          ok |= fetchStock(client, rows[i]);
          delay(250);  // sequential and unhurried: 429 is the enemy, not latency
        }
      }
      if (ok) {
        lastOk = millis();
        failures = 0;
        if (uiScreenOn() && state == State::Running) drawRows();
      } else {
        failures++;
      }
      Serial.printf("fetch %s (failures %u, heap %u)\n", ok ? "ok" : "FAILED", failures,
                    ESP.getFreeHeap());
    }
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > LOG_MS) {
    lastLog = millis();
    Serial.printf("die %.1fC  heap %u  wifi %s %ddBm  rows", temperatureRead(),
                  ESP.getFreeHeap(), WiFi.status() == WL_CONNECTED ? "up" : "DOWN",
                  WiFi.RSSI());
    for (uint8_t i = 0; i < nRows; i++)
      Serial.printf(" %s=%s", rows[i].label, rows[i].valid ? "ok" : "-");
    Serial.println();
  }

  delay(20);
}
