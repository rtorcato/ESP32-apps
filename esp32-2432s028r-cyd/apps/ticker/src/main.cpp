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
#include <helv.h>
#include <netjoin.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
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
// Text comes in four logical sizes, each a Helvetica bitmap face (the X11
// Adobe set, via U8g2 -- the closest thing to a phone's type that fits in
// 18KB). GW is the width reserved per character in the layout: Helvetica is
// narrower than that in every size, so nothing overflows its box. GH is the
// real box height, cap plus descender.
struct Face {
  const uint8_t *font;
  uint8_t cap, desc;
};
static const Face FACES[] = {{u8g2_font_helvR08_tr, 8, 2},
                             {u8g2_font_helvB14_tr, 14, 4},
                             {u8g2_font_helvB18_tr, 19, 5},
                             {u8g2_font_helvB24_tr, 25, 7}};
#define GW(s) (6 * (s))
#define GH(s) (FACES[(s) - 1].cap + FACES[(s) - 1].desc)

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
static const char *const SLEEP_NAMES[] = {"never", "night", "closed"};
static uint8_t sSpeed = 1, sBl = 0, sRet = 1, sSleep = 0;
// Sound and LED are each two bits: bit 0 the everyday use (tap clicks /
// the day's glow), bit 1 the alerts (chime / white blinks).
static const char *const TWO_NAMES[][4] = {{"off", "taps", "alerts", "both"}, {"off", "glow", "alerts", "both"}};
static uint8_t sLed = 3;

// ── alerts: a chime (and a white blink) when a price crosses a line ──────
// alerts.movePct: any stock moving that far in a day, once, re-armed when
// it comes back inside half of it. alerts.levels: a price line per symbol,
// above or below, once per crossing, re-armed 1% back across it.
struct Alert {
  char sym[MAX_LABEL + 1];
  float above, below;
  bool fired;
};
static const uint8_t MAX_ALERTS = 16;
static Alert alerts[MAX_ALERTS];
static uint8_t nAlerts = 0;
static float movePct = 5.0f;
static uint32_t moveFired = 0;  // bit per row
static uint8_t sleepFrom = 23, sleepTo = 7;  // the night window, config.json
static uint32_t awakeUntil = 60000;          // no sleeping before this; a touch pushes it out a minute
static uint8_t sSound = 3;
static Preferences prefs;
static int blApplied = -1, ldrRaw = 0;  // brightnessTick's last reading and level, for the info page

// ── the watchlist ────────────────────────────────────────────────────────
struct Row {
  char label[MAX_LABEL + 1];
  char id[28];
  char name[32];
  bool coin, valid;
  float price, pct, prev, dayLo, dayHi, wkLo, wkHi;
  float last;     // the newest bar, extended hours included; == price in the regular session
  time_t traded;  // Yahoo's regularMarketTime: the last regular-session trade
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

// ── on-device edits to the watchlist ─────────────────────────────────────
// config.json stays the source; what the finger adds or removes is an
// overlay in NVS (two comma lists, "add" and "del") applied after the file
// loads, so a config push never undoes a tap. Adding a symbol the file
// lists takes it off "del"; removing an added one takes it off "add".
// Stocks stay ahead of coins in rows[] because the sweep counts on it, so
// an insert goes at nStocks. listVersion lets an in-flight fetch notice
// the rows moved under it and drop its result.
static char ovAdd[200] = "", ovDel[200] = "";
static uint32_t listVersion = 0;
static bool listHas(const char *list, const char *sym) {
  char pat[MAX_LABEL + 3];
  snprintf(pat, sizeof pat, ",%s,", sym);
  char wrapped[204];
  snprintf(wrapped, sizeof wrapped, ",%s,", list);
  return strstr(wrapped, pat) != nullptr;
}
static void listAppend(char *list, size_t n, const char *sym) {
  if (listHas(list, sym)) return;
  if (list[0]) strlcat(list, ",", n);
  strlcat(list, sym, n);
}
static void listRemove(char *list, const char *sym) {
  char out[200] = "";
  char copy[200];
  strlcpy(copy, list, sizeof copy);
  for (char *tok = strtok(copy, ","); tok; tok = strtok(nullptr, ","))
    if (strcmp(tok, sym) != 0) listAppend(out, sizeof out, tok);
  strcpy(list, out);
}
static int8_t findRow(const char *label) {
  for (uint8_t i = 0; i < nRows; i++)
    if (strcmp(rows[i].label, label) == 0) return (int8_t)i;
  return -1;
}
static void rebuildCoinIds() {
  coinIds[0] = '\0';
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    if (coinIds[0]) strlcat(coinIds, ",", sizeof coinIds);
    strlcat(coinIds, rows[i].id, sizeof coinIds);
  }
}
// The label a Yahoo symbol gets: "-USD" dropped (BTC-USD -> BTC), then cut
// to the five the row can show.
static void labelFor(const char *sym, char *out, size_t n) {
  char full[32];
  snprintf(full, sizeof full, "%s", sym);
  size_t l = strlen(full);
  if (l > 4 && strcmp(full + l - 4, "-USD") == 0) full[l - 4] = '\0';  // strip first, THEN cut
  snprintf(out, n, "%.*s", MAX_LABEL, full);
}
static int8_t insertStock(const char *label, const char *id) {
  if (nRows >= MAX_SYMBOLS || !*label || strlen(label) > MAX_LABEL || findRow(label) >= 0) return -1;
  xSemaphoreTake(mux, portMAX_DELAY);
  memmove(rows + nStocks + 1, rows + nStocks, (nRows - nStocks) * sizeof(Row));
  Row &r = rows[nStocks];
  memset(&r, 0, sizeof r);
  snprintf(r.label, sizeof r.label, "%s", label);
  snprintf(r.id, sizeof r.id, "%s", id);
  uint8_t at = nStocks++;
  nRows++;
  listVersion++;
  xSemaphoreGive(mux);
  return (int8_t)at;
}
static void removeRow(uint8_t i) {
  xSemaphoreTake(mux, portMAX_DELAY);
  bool coin = rows[i].coin;
  memmove(rows + i, rows + i + 1, (nRows - i - 1) * sizeof(Row));
  nRows--;
  if (coin) nCoins--;
  else nStocks--;
  rebuildCoinIds();
  listVersion++;
  xSemaphoreGive(mux);
}
static void saveOverlay() {
  prefs.begin("ticker", false);
  prefs.putString("add", ovAdd);
  prefs.putString("del", ovDel);
  prefs.end();
}
static void applyOverlay() {
  prefs.begin("ticker", true);
  prefs.getString("add", ovAdd, sizeof ovAdd);
  prefs.getString("del", ovDel, sizeof ovDel);
  prefs.end();
  char copy[200];
  strlcpy(copy, ovDel, sizeof copy);
  for (char *tok = strtok(copy, ","); tok; tok = strtok(nullptr, ",")) {
    int8_t i = findRow(tok);
    if (i >= 0) removeRow((uint8_t)i);
  }
  strlcpy(copy, ovAdd, sizeof copy);
  for (char *tok = strtok(copy, ","); tok; tok = strtok(nullptr, ",")) {
    char label[MAX_LABEL + 1];
    labelFor(tok, label, sizeof label);
    insertStock(label, tok);
  }
  rebuildCoinIds();
  if (ovAdd[0] || ovDel[0]) Serial.printf("watchlist edits from NVS: added [%s] removed [%s]\n", ovAdd, ovDel);
  if (nRows == 0) cfgErr = "every symbol removed; swipe left to add one";
}
// The finger adds symbol `sym` (a Yahoo symbol): row index, or -1.
static int8_t userAdd(const char *sym) {
  char label[MAX_LABEL + 1];
  labelFor(sym, label, sizeof label);
  int8_t i = findRow(label);
  if (i >= 0) return i;
  i = insertStock(label, sym);
  if (i < 0) return -1;
  if (listHas(ovDel, label)) listRemove(ovDel, label);
  else listAppend(ovAdd, sizeof ovAdd, sym);
  saveOverlay();
  Serial.printf("added %s as %s\n", sym, label);
  return i;
}
static void userRemove(uint8_t i) {
  char label[MAX_LABEL + 1], id[28];
  strcpy(label, rows[i].label);
  strcpy(id, rows[i].id);
  removeRow(i);
  if (listHas(ovAdd, id)) listRemove(ovAdd, id);
  else listAppend(ovDel, sizeof ovDel, label);
  saveOverlay();
  Serial.printf("removed %s\n", label);
}

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
  sleepFrom = (uint8_t)cfgInt("sleep.from", sleepFrom, 0, 23);
  sleepTo = (uint8_t)cfgInt("sleep.to", sleepTo, 0, 23);
  movePct = (float)cfgInt("alerts.movePct", (long)movePct, 0, 100);  // 0 = off
  for (JsonObject o : cfgArr("alerts.levels")) {
    const char *sym = o["symbol"];
    if (!sym || strlen(sym) > MAX_LABEL || nAlerts >= MAX_ALERTS) {
      Serial.printf("skipped alert '%s' (bad symbol or list full)\n", sym ? sym : "?");
      continue;
    }
    Alert &a = alerts[nAlerts++];
    snprintf(a.sym, sizeof a.sym, "%s", sym);
    a.above = o["above"] | 0.0f;
    a.below = o["below"] | 0.0f;
    a.fired = false;
  }
  if (nAlerts || movePct > 0) Serial.printf("alerts: %u price lines, move %.0f%%\n", nAlerts, movePct);
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

  rebuildCoinIds();
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
static const int16_t S_Y0 = 64, S_H = 30, S_N = 8;
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

// ── search: Yahoo's symbol lookup, keyless ───────────────────────────────
struct Hit {
  char sym[12], name[30], exch[6];
};
static const uint8_t MAX_HITS = 6;
static Hit hits[MAX_HITS];  // fetch task writes, UI reads, under mux
static uint8_t nHits = 0;
static bool searchDone = false, searchFailed = false;
static volatile bool searchWant = false;
static char query[9] = "", searchQ[9] = "";
static uint8_t searchMode = 0;  // 0 the keyboard, 1 the results

// ── news: Yahoo's per-symbol headline RSS, keyless ───────────────────────
struct NewsItem {
  char title[96];
  char age[8];
};
static const uint8_t NEWS_N = 6;
static NewsItem news[NEWS_N];  // fetch task writes, UI reads, under mux
static uint8_t newsN = 0, newsIdx = 255;
static uint32_t newsAt = 0;
static bool newsFailed = false;
static volatile int16_t newsWant = -1;  // symbol index to fetch headlines for, or -1

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
// A holiday looks like a weekday to the clock. The quotes know better: on
// a weekday, a quarter hour past the open, if every stock's last regular
// trade is from an earlier day, nothing is trading today. Pre-market on a
// holiday cannot be told apart from a normal one (yesterday's trade is
// normal then) and costs a few extended-hours fetches; fine.
static volatile bool holiday = false;  // set in loop, read by the fetch task
static bool holidayFrom(const struct tm &t) {
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;
  if (t.tm_hour * 60 + t.tm_min < mktOpenMin + 15) return false;
  bool seen = false;
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].valid || rows[i].coin || !rows[i].traded) continue;  // plain reads; a torn one costs nothing
    struct tm tt;
    time_t when = rows[i].traded;
    localtime_r(&when, &tt);
    if (tt.tm_year == t.tm_year && tt.tm_yday == t.tm_yday) return false;
    seen = true;
  }
  return seen;
}
static Session sessionNow(const struct tm &t) { return holiday ? Session::Closed : session(t); }
static bool marketOpen(const struct tm &t) { return sessionNow(t) == Session::Regular; }
// Inside the sleep window, which may wrap midnight (23 -> 7). Pure.
static bool inNight(uint8_t h, uint8_t from, uint8_t to) { return from <= to ? (h >= from && h < to) : (h >= from || h < to); }
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
// y is the TOP of the text box; the face's cap height turns it into the
// baseline the font wants. Widths are measured, not counted.
static int16_t textWidth(uint8_t size, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(FACES[size - 1].font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}
static void textAt(int16_t x, int16_t y, uint8_t size, uint16_t fg, const char *s) {
  gfx->setFont(FACES[size - 1].font);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y + FACES[size - 1].cap);
  gfx->print(s);
}
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, GW(size) * chars, GH(size), C_BG);
  textAt(x, y, size, fg, s);
}
static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, w, GH(size), C_BG);
  textAt(right - textWidth(size, s), y, size, fg, s);
}
static void fieldCentre(int16_t cx, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(cx - w / 2, y, w, GH(size), C_BG);
  textAt(cx - textWidth(size, s) / 2, y, size, fg, s);
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

// ── deep sleep ───────────────────────────────────────────────────────────
// Sleep is real sleep: the panel to SLPIN, Wi-Fi off, the chip in deep
// sleep, woken by the touch pen-down line (GPIO36 is RTC-capable) or a
// timer set for the end of the sleep window. Deep sleep is a reboot, so
// the watchlist's prices go into RTC slow memory first: a touch at 3am
// shows last night's numbers the instant the panel lights, before Wi-Fi
// has even started joining. The system clock survives deep sleep on its
// own. ponytail: the CYD's CH340 and regulator keep drawing regardless;
// this cuts the ESP32 and the backlight, which is where the current went.
struct RtcRow {
  float price, pct, prev, last;
  float close[16];
  uint8_t n;
  bool valid;
};
static const uint32_t RTC_MAGIC = 0x7469636B;  // "tick"
RTC_DATA_ATTR static uint32_t rtcMagic = 0, rtcLabelHash = 0;
RTC_DATA_ATTR static time_t rtcLastOk = 0;
RTC_DATA_ATTR static int32_t rtcPos = 0;
RTC_DATA_ATTR static RtcRow rtcRows[MAX_SYMBOLS];
static int32_t pos;  // defined with the list below; the snapshot needs it here

static uint32_t labelHash() {
  uint32_t h = nRows;
  for (uint8_t i = 0; i < nRows; i++)
    for (const char *c = rows[i].label; *c; c++) h = h * 31 + (uint8_t)*c;
  return h;
}
static void snapshotToRtc() {
  for (uint8_t i = 0; i < nRows; i++) {
    const Row &r = rows[i];  // the fetch task is not running any more
    RtcRow &o = rtcRows[i];
    o.price = r.price;
    o.pct = r.pct;
    o.prev = r.prev;
    o.last = r.last;
    o.valid = r.valid;
    o.n = r.n < 16 ? r.n : 16;
    for (uint8_t j = 0; j < o.n; j++) o.close[j] = r.close[r.n < 16 ? j : (uint32_t)j * (r.n - 1) / 15];
  }
  rtcLabelHash = labelHash();
  rtcLastOk = lastOk ? time(nullptr) - (millis() - lastOk) / 1000 : 0;
  rtcPos = pos;
  rtcMagic = RTC_MAGIC;
}
// True if the snapshot matched this watchlist and the rows were restored.
static bool restoreFromRtc() {
  if (rtcMagic != RTC_MAGIC || rtcLabelHash != labelHash()) return false;
  for (uint8_t i = 0; i < nRows; i++) {
    Row &r = rows[i];
    const RtcRow &o = rtcRows[i];
    r.price = o.price;
    r.pct = o.pct;
    r.prev = o.prev;
    r.last = o.last;
    r.valid = o.valid;
    r.n = o.n;
    memcpy(r.close, o.close, o.n * sizeof(float));
  }
  pos = rtcPos;
  if (rtcLastOk) {
    time_t age = time(nullptr) - rtcLastOk;
    lastOk = age > 0 && (uint32_t)age * 1000 < millis() ? millis() - age * 1000 : 1;
  }
  return true;
}
// The sleep condition, pure in t. Closed follows the market; night the window.
static bool sleepDue(const struct tm &t) {
  return (sSleep == 1 && inNight(t.tm_hour, sleepFrom, sleepTo)) || (sSleep == 2 && sessionNow(t) == Session::Closed);
}
// Seconds until the sleep window ends: the night's `to` hour, or the next
// weekday's pre-market. Capped at six hours so a long weekend still gets a
// look at the clock now and then.
static uint32_t secondsUntilWake(const struct tm &t) {
  int32_t now = t.tm_hour * 60 + t.tm_min;
  int32_t mins;
  if (sSleep == 1) {
    mins = (sleepTo * 60 - now + 1440) % 1440;
    if (mins == 0) mins = 1440;
  } else {
    int wday = t.tm_wday, days = 0;
    if (!(wday >= 1 && wday <= 5 && now < mktPreMin)) {
      do {
        wday = (wday + 1) % 7;
        days++;
      } while (wday == 0 || wday == 6);
    }
    mins = days * 1440 + mktPreMin - now;
  }
  uint32_t secs = (uint32_t)mins * 60 - t.tm_sec + 5;
  return secs > 6UL * 3600 ? 6UL * 3600 : secs;
}
static void goToSleep(uint32_t secs) {
  Serial.printf("deep sleep for up to %lus, or a touch\n", (unsigned long)secs);
  snapshotToRtc();
  backlight(0);
  ledGlow(0, 0, 0);
  if (gfx) {  // a timer wake goes back to sleep before the bus exists; the panel is still asleep then
    Arduino_DataBus *bus = boardBus();
    bus->beginWrite();
    bus->writeCommand(0x28);  // DISPOFF
    bus->writeCommand(0x10);  // SLPIN
    bus->endWrite();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);  // TP_IRQ: low while a finger is down
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
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
// pos: pixels scrolled; row 0 sat at the top at 0. Declared with the sleep code above.
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
  if (!blitLogo(X_SYM, y + Y_BADGE, LOGO_BADGE, r.label)) {  // no file: a tile with the initial
    gfx->fillRect(X_SYM, y + Y_BADGE, LOGO_BADGE, LOGO_BADGE, C_BG);
    gfx->fillRoundRect(X_SYM, y + Y_BADGE, LOGO_BADGE, LOGO_BADGE, 5, C_RULE);
    char c[2] = {r.label[0], 0};
    textAt(X_SYM + (LOGO_BADGE - textWidth(2, c)) / 2, y + Y_BADGE + (LOGO_BADGE - FACES[1].cap) / 2, 2, C_MUTED, c);
  }
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
  Session ses = haveTime ? sessionNow(*t) : Session::Regular;
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

// ── boot splash: drawn, not shipped ──────────────────────────────────────
// A navy-to-black sky, nine candles on the way up with a gold average
// through them, the wordmark, and one status line that follows the Wi-Fi
// join. Primitives only, so there is no asset to generate or push.
static const uint16_t C_GOLD = 0xFD40;
static void splashStatus(const char *s) { fieldCentre(LCD_W / 2, 286, 36, 1, C_DIM, s); }
static void drawSplash(const char *status) {
  for (int16_t y = 0; y < LCD_H; y++) {  // (0,10,30) at the top fading to black
    uint8_t g = 10 - (uint16_t)y * 10 / LCD_H, b = 30 - (uint16_t)y * 30 / LCD_H;
    gfx->drawFastHLine(0, y, LCD_W, ((g & 0xFC) << 3) | (b >> 3));
  }
  struct Candle { int16_t o, c, l, h; };  // screen y: smaller is higher
  static const Candle k[9] = {{206, 196, 211, 192}, {196, 202, 205, 190}, {202, 184, 204, 180},
                              {184, 172, 188, 166}, {172, 178, 182, 168}, {178, 158, 180, 152},
                              {158, 148, 162, 144}, {148, 154, 156, 142}, {154, 132, 156, 126}};
  for (uint8_t i = 0; i < 9; i++) {
    int16_t x = 30 + i * 22;
    uint16_t col = k[i].c < k[i].o ? C_GOOD : C_BAD;
    gfx->drawFastVLine(x + 5, k[i].h, k[i].l - k[i].h + 1, col);
    gfx->fillRect(x, min(k[i].o, k[i].c), 11, abs(k[i].o - k[i].c) + 1, col);
  }
  for (uint8_t i = 1; i < 9; i++) {  // the average, a little under the bodies
    int16_t x0 = 30 + (i - 1) * 22 + 5, x1 = x0 + 22;
    int16_t y0 = (k[i - 1].o + k[i - 1].c) / 2 + 8, y1 = (k[i].o + k[i].c) / 2 + 8;
    gfx->drawLine(x0, y0, x1, y1, C_GOLD);
    gfx->drawLine(x0, y0 + 1, x1, y1 + 1, C_GOLD);
  }
  gfx->drawFastHLine(24, 218, 192, C_RULE);
  const char *name = "TICKER";
  textAt((LCD_W - textWidth(4, name)) / 2, 48, 4, C_FG, name);
  gfx->fillRect(LCD_W / 2 - 24, 86, 48, 2, C_GOLD);
  const char *tag = "STOCKS   CRYPTO   HEADLINES";
  textAt((LCD_W - textWidth(1, tag)) / 2, 98, 1, C_MUTED, tag);
  const char *credit = "made by Richard Torcato";
  textAt((LCD_W - textWidth(1, credit)) / 2, 248, 1, C_MUTED, credit);
  splashStatus(status);
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
static void drawHint(const char *s, uint16_t c = C_DIM) { fieldCentre(LCD_W / 2, Y_HINT, 38, 1, c, s); }

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
    textAt(x + (R_W - textWidth(2, RANGES[i].label)) / 2, R_Y + (R_H - FACES[1].cap) / 2, 2, on ? C_FG : C_MUTED,
           RANGES[i].label);
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
    if (!blitLogo(X_SYM, D_Y_SYM, LOGO_BIG, r.label)) {  // no file: a tile with the symbol
      gfx->fillRoundRect(X_SYM, D_Y_SYM, LOGO_BIG, LOGO_BIG, 14, C_RULE);
      textAt(X_SYM + (LOGO_BIG - textWidth(3, r.label)) / 2, D_Y_SYM + (LOGO_BIG - FACES[2].cap) / 2, 3, C_MUTED, r.label);
    }
    drawHint("< next    ^ news    v list    prev >");
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
  Session ses = getLocalTime(&t, 0) ? sessionNow(t) : Session::Regular;
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
    sSound = prefs.getUChar("snd2", sSound) & 3;  // snd2: the old snd was a plain on/off
    sSleep = prefs.getUChar("slp", sSleep) % 3;
    sLed = prefs.getUChar("led2", sLed) & 3;
  }
  if (prefs.isKey("tx0")) {
    touchCal.swap = prefs.getBool("tsw", false);
    touchCal.xMin = prefs.getShort("tx0", touchCal.xMin);
    touchCal.xMax = prefs.getShort("tx1", touchCal.xMax);
    touchCal.yMin = prefs.getShort("ty0", touchCal.yMin);
    touchCal.yMax = prefs.getShort("ty1", touchCal.yMax);
    Serial.printf("touch cal from NVS: swap %d  x %d..%d  y %d..%d\n", touchCal.swap, touchCal.xMin, touchCal.xMax,
                  touchCal.yMin, touchCal.yMax);
  }
  if (any) {
    applySettings();
  } else {  // nothing saved yet: the config's own values stand
    for (uint8_t i = 0; i < 3; i++) if (SPEED_MS[i] == pageMs) sSpeed = i;
    for (uint8_t i = 0; i < 3; i++) if (RET_MS[i] == returnMs) sRet = i;
    blAuto = cfgBlAuto;
    blFixed = blMax;
  }
  prefs.end();
  Serial.printf("settings from %s: speed %s, backlight %s, sound %s, return %s, sleep %s\n",
                any ? "NVS (beats config.json)" : "config.json", SPEED_NAMES[sSpeed], BL_NAMES[sBl], TWO_NAMES[0][sSound],
                RET_NAMES[sRet], SLEEP_NAMES[sSleep]);
}
static void saveSettings() {
  prefs.begin("ticker", false);
  prefs.putUChar("speed", sSpeed);
  prefs.putUChar("bl", sBl);
  prefs.putUChar("ret", sRet);
  prefs.putUChar("snd2", sSound);
  prefs.putUChar("slp", sSleep);
  prefs.putUChar("led2", sLed);
  prefs.end();
  applySettings();
}

static void drawSettingRow(uint8_t i) {
  static const char *const labels[] = {"Scroll", "Backlight", "Sound", "Auto return", "Sleep", "LED", "Touch", "Info"};
  const char *v = i == 0 ? SPEED_NAMES[sSpeed] : i == 1 ? BL_NAMES[sBl] : i == 2 ? TWO_NAMES[0][sSound]
                : i == 3 ? RET_NAMES[sRet] : i == 4 ? SLEEP_NAMES[sSleep] : i == 5 ? TWO_NAMES[1][sLed]
                : i == 6 ? "calibrate" : ">";
  int16_t y = S_Y0 + i * S_H;
  field(8, y + 6, 11, 2, C_MUTED, labels[i]);
  fieldRight(X_RIGHT, y + 6, 9, 2, C_FG, v);
  gfx->drawFastHLine(8, y + S_H - 1, 224, C_RULE);
}
static void drawSettings() {
  drawPanel("SETTINGS", C_MUTED, nullptr, 0);
  for (uint8_t i = 0; i < S_N; i++) drawSettingRow(i);
  drawHint("< list");
}
// A tap on row i: cycle it, or open a page. Returns 0 (cycled), 1 (info), 2 (touch calibration).
static uint8_t tapSetting(uint8_t i) {
  if (i == 7) return 1;
  if (i == 6) return 2;
  if (i == 0) sSpeed = (sSpeed + 1) % 3;
  else if (i == 1) sBl = (sBl + 1) % 3;
  else if (i == 2) sSound = (sSound + 1) & 3;
  else if (i == 3) sRet = (sRet + 1) % 3;
  else if (i == 4) sSleep = (sSleep + 1) % 3;
  else sLed = (sLed + 1) & 3;
  saveSettings();
  drawSettingRow(i);
  return 0;
}

// ── touch calibration: three targets, the panel's own numbers ────────────
// Top-left, top-right, bottom-left. Which chip axis moved between the first
// two says whether the axes are swapped; the sign of the move says whether
// one is mirrored; extrapolating to the screen edges gives the ranges. It
// replaces guessing at a wiring fact, and it survives in NVS.
static const int16_t CAL_PT[3][2] = {{20, 20}, {LCD_W - 21, 20}, {20, LCD_H - 21}};
static uint8_t calStep = 0;
static uint16_t calRaw[3][2];
static void drawCalTarget() {
  gfx->fillScreen(C_BG);
  field(8, 24, 12, 3, C_MUTED, "TOUCH");
  char l[40];
  snprintf(l, sizeof l, "tap the target, %u of 3", calStep + 1);
  fieldCentre(LCD_W / 2, 150, 30, 1, C_MUTED, l);
  int16_t x = CAL_PT[calStep][0], y = CAL_PT[calStep][1];
  gfx->drawCircle(x, y, 10, C_WARN);
  gfx->drawFastHLine(x - 14, y, 29, C_WARN);
  gfx->drawFastVLine(x, y - 14, 29, C_WARN);
}
static bool touchCalFrom(const uint16_t raw[3][2], TouchCal *c) {
  int32_t dx = (int32_t)raw[1][0] - raw[0][0], dy = (int32_t)raw[1][1] - raw[0][1];  // TL -> TR
  bool swap = abs(dy) > abs(dx);
  int32_t ax0 = swap ? raw[0][1] : raw[0][0], ax1 = swap ? raw[1][1] : raw[1][0];  // along screen X
  int32_t ay0 = swap ? raw[0][0] : raw[0][1], ay2 = swap ? raw[2][0] : raw[2][1];  // along screen Y
  if (abs(ax1 - ax0) < 500 || abs(ay2 - ay0) < 500) return false;  // two taps in one place
  float sx = (float)(ax1 - ax0) / (CAL_PT[1][0] - CAL_PT[0][0]);
  float sy = (float)(ay2 - ay0) / (CAL_PT[2][1] - CAL_PT[0][1]);
  c->swap = swap;
  c->xMin = (int16_t)lroundf(ax0 - CAL_PT[0][0] * sx);
  c->xMax = (int16_t)lroundf(c->xMin + (LCD_W - 1) * sx);
  c->yMin = (int16_t)lroundf(ay0 - CAL_PT[0][1] * sy);
  c->yMax = (int16_t)lroundf(c->yMin + (LCD_H - 1) * sy);
  return true;
}
static void saveTouchCal() {
  prefs.begin("ticker", false);
  prefs.putBool("tsw", touchCal.swap);
  prefs.putShort("tx0", touchCal.xMin);
  prefs.putShort("tx1", touchCal.xMax);
  prefs.putShort("ty0", touchCal.yMin);
  prefs.putShort("ty1", touchCal.yMax);
  prefs.end();
}
// One pass while the calibration page is up: average the raw samples of a
// press, take the average on release. True when all three are in.
static bool calibTick() {
  static uint32_t sx = 0, sy = 0, n = 0, lastUp = 0;
  static bool down = false;
  uint16_t rx, ry;
  bool contact = touchRaw(&rx, &ry);
  if (contact) {
    if (millis() - lastUp < 300) return false;  // the tail of the previous press
    sx += rx;
    sy += ry;
    n++;
    down = true;
    return false;
  }
  if (!down) return false;
  down = false;
  lastUp = millis();
  if (n < 3) return false;
  calRaw[calStep][0] = sx / n;
  calRaw[calStep][1] = sy / n;
  Serial.printf("touch cal %u: raw %u,%u (%lu samples)\n", calStep, calRaw[calStep][0], calRaw[calStep][1],
                (unsigned long)n);
  sx = sy = n = 0;
  if (++calStep < 3) {
    drawCalTarget();
    return false;
  }
  calStep = 0;
  TouchCal c;
  if (touchCalFrom(calRaw, &c)) {
    touchCal = c;
    saveTouchCal();
    Serial.printf("touch cal: swap %d  x %d..%d  y %d..%d  (saved)\n", c.swap, c.xMin, c.xMax, c.yMin, c.yMax);
  } else {
    Serial.println("touch cal: taps too close together, not saved");
  }
  return true;
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
    Session ses = sessionNow(t);
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
  l[n++][0] = '\0';
  snprintf(l[n++], 40, "tap anywhere for the splash screen");
  for (uint8_t i = 0; i < n; i++) field(8, 62 + i * 11, 38, 1, i == 0 ? C_FG : C_MUTED, l[i]);
}

// ── search page: swipe left from the list ────────────────────────────────
// Symbols are short and upper-case, so the keyboard is a 6x5 grid of 40px
// keys, finger-sized on a resistive panel: A-X, then Y Z . - backspace GO.
static const char *const KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // + backspace + GO
static const int16_t K_Y0 = 120, K_W = 40, K_H = 40, Q_Y = 44, RES_Y0 = 48, RES_H = 40;
static int8_t hitKey(int16_t x, int16_t y) {
  if (y < K_Y0 || y >= K_Y0 + 5 * K_H) return -1;
  int8_t i = (y - K_Y0) / K_H * 6 + x / K_W;
  return i < 30 ? i : -1;
}
static int8_t hitResult(int16_t y) {
  if (y < RES_Y0 || y >= RES_Y0 + RES_H * nHits) return -1;
  return (y - RES_Y0) / RES_H;
}
static void drawQuery() {
  gfx->fillRoundRect(8, Q_Y, 224, 40, 8, C_RULE);
  if (query[0]) textAt(20, Q_Y + 10, 3, C_FG, query);
  else textAt(20, Q_Y + 16, 1, C_DIM, "symbol or company name");
}
static void drawKeyboard() {
  for (uint8_t i = 0; i < 30; i++) {
    int16_t x = (i % 6) * K_W, y = K_Y0 + (i / 6) * K_H;
    gfx->drawRoundRect(x + 2, y + 2, K_W - 4, K_H - 4, 6, C_RULE);
    char k[3] = {0, 0, 0};
    const char *lab = k;
    uint16_t c = C_FG;
    if (i < 28) k[0] = KEYS[i];
    else if (i == 28) lab = "<";
    else {
      lab = "GO";
      c = C_GOOD;
    }
    textAt(x + (K_W - textWidth(2, lab)) / 2, y + (K_H - FACES[1].cap) / 2, 2, c, lab);
  }
}
static void drawSearch(bool full) {
  static char cSearch[16];
  if (searchMode == 0) {
    if (!full) return;
    gfx->fillScreen(C_BG);
    textAt(8, 8, 2, C_MUTED, "SEARCH");
    textAt(LCD_W - 8 - textWidth(1, "v list"), 14, 1, C_DIM, "v list");
    drawQuery();
    textAt(8, 96, 1, C_DIM, "type a few letters, tap GO");
    drawKeyboard();
    return;
  }
  Hit h[MAX_HITS];
  uint8_t n;
  bool done, failed;
  xSemaphoreTake(mux, portMAX_DELAY);
  n = nHits;
  done = searchDone;
  failed = searchFailed;
  memcpy(h, hits, sizeof h);
  xSemaphoreGive(mux);
  if (full) {
    gfx->fillScreen(C_BG);
    textAt(8, 8, 2, C_MUTED, searchQ);
    textAt(LCD_W - 8 - textWidth(1, "v back"), 14, 1, C_DIM, "v back");
    gfx->drawFastHLine(8, 40, 224, C_RULE);
    cSearch[0] = '\0';
  }
  char key[16];
  snprintf(key, sizeof key, "%u|%d|%d", n, done, failed);
  if (strcmp(key, cSearch) == 0) return;
  strcpy(cSearch, key);
  gfx->fillRect(0, RES_Y0, LCD_W, Y_HINT - RES_Y0, C_BG);
  if (!done) {
    field(8, 150, 20, 1, C_DIM, "searching...");
    return;
  }
  if (failed || !n) {
    field(8, 150, 20, 1, C_DIM, failed ? "search failed" : "nothing found");
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    int16_t y = RES_Y0 + i * RES_H;
    bool listed = findRow(h[i].sym) >= 0;
    textAt(8, y + 4, 2, C_FG, h[i].sym);
    fieldRight(X_RIGHT, y + 8, 8, 1, listed ? C_GOOD : C_DIM, listed ? "on list" : h[i].exch);
    textAt(8, y + 24, 1, C_MUTED, h[i].name);
    gfx->drawFastHLine(8, y + RES_H - 1, 224, C_RULE);
  }
  drawHint("tap one to add it and open it");
}
// ── news page: swipe up from a stock ─────────────────────────────────────
static char cNews[24];
// Greedy word wrap by measured width into at most `lines` lines of `w` px.
static uint8_t wrapText(const char *s, int16_t w, char out[][64], uint8_t lines) {
  uint8_t n = 0;
  char line[64] = "";
  const char *p = s;
  while (*p && n < lines) {
    const char *e = p;
    while (*e && *e != ' ') e++;
    char cand[64];
    snprintf(cand, sizeof cand, "%s%s%.*s", line, line[0] ? " " : "", (int)(e - p), p);
    if (textWidth(1, cand) <= w || !line[0]) {
      strcpy(line, cand);
    } else {
      strcpy(out[n++], line);
      line[0] = '\0';
      continue;
    }
    p = *e ? e + 1 : e;
  }
  if (line[0] && n < lines) strcpy(out[n++], line);
  if (*p && n == lines) {  // ran out of lines: mark the cut
    size_t l = strlen(out[n - 1]);
    if (l > 3) strcpy(out[n - 1] + l - 3, "...");
  }
  return n;
}
static void drawNews(bool full) {
  Row r = rowCopy(detailIdx);
  if (full) {
    gfx->fillScreen(C_BG);
    field(8, 8, 5, 3, C_FG, r.label);
    textAt(8 + textWidth(3, r.label) + 8, 14, 1, C_MUTED, "headlines");
    gfx->drawFastHLine(8, 38, 224, C_RULE);
    drawHint("< next     v stock     prev >");
    cNews[0] = '\0';
  }
  NewsItem items[NEWS_N];
  uint8_t n;
  bool mine, failed;
  xSemaphoreTake(mux, portMAX_DELAY);
  mine = newsIdx == detailIdx;
  n = mine ? newsN : 0;
  failed = mine && newsFailed;
  memcpy(items, news, sizeof items);
  xSemaphoreGive(mux);
  char key[24];
  snprintf(key, sizeof key, "%u|%u|%d|%lu", detailIdx, n, failed, (unsigned long)newsAt);
  if (strcmp(key, cNews) == 0) return;
  strcpy(cNews, key);
  gfx->fillRect(0, 42, LCD_W, Y_HINT - 44, C_BG);
  if (!mine || (!n && !failed)) {
    field(8, 150, 20, 1, C_DIM, "loading headlines...");
    return;
  }
  if (!n) {
    field(8, 150, 20, 1, C_DIM, "no headlines");
    return;
  }
  int16_t y = 44;
  for (uint8_t i = 0; i < n && y + 24 <= Y_HINT - 4; i++) {
    char lines[2][64];
    uint8_t k = wrapText(items[i].title, LCD_W - 16 - 30, lines, 2);
    for (uint8_t j = 0; j < k; j++) textAt(8, y + j * 11, 1, j == 0 ? C_FG : C_MUTED, lines[j]);
    fieldRight(X_RIGHT, y, 5, 1, C_DIM, items[i].age);
    y += k * 11 + 3;
    gfx->drawFastHLine(8, y, 224, C_RULE);
    y += 5;
  }
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
                        "regularMarketDayLow", "fiftyTwoWeekHigh", "fiftyTwoWeekLow", "regularMarketTime"})
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
  r.traded = (time_t)(m["regularMarketTime"] | 0L);
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

// Days since the epoch for a civil date (Howard Hinnant's algorithm), so
// an RFC 822 pubDate can be compared with time() without a timezone dance.
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
static const char *const MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
// "Wed, 16 Sep 2026 14:05:28 +0000" -> UTC epoch, 0 if unparseable.
static time_t parseRfc822(const char *s) {
  char mon[4];
  int d, y, H, M, S;
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6) return 0;
  const char *m = strstr(MONTHS, mon);
  if (!m) return 0;
  return (time_t)daysFromCivil(y, (m - MONTHS) / 3 + 1, d) * 86400 + H * 3600 + M * 60 + S;
}
static void ageOf(time_t when, char *out, size_t n) {
  time_t now = time(nullptr);
  if (!when || now < 1000000000L || now < when) { out[0] = '\0'; return; }
  uint32_t s = now - when;
  if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}
// The few entities Yahoo's titles use, in place.
static void decodeEntities(char *s) {
  static const char *const from[] = {"&amp;", "&#39;", "&apos;", "&quot;", "&lt;", "&gt;", "&#x27;", "&nbsp;"};
  static const char *const to[] = {"&", "'", "'", "\"", "<", ">", "'", " "};
  for (uint8_t i = 0; i < 8; i++) {
    char *p;
    while ((p = strstr(s, from[i]))) {
      size_t fl = strlen(from[i]), tl = strlen(to[i]);
      memcpy(p, to[i], tl);
      memmove(p + tl, p + fl, strlen(p + fl) + 1);
    }
  }
}
// Text between <tag> and </tag> after `from`, CDATA unwrapped, into out.
static const char *xmlText(const char *from, const char *end, const char *tag, char *out, size_t n) {
  char open[24], close[24];
  snprintf(open, sizeof open, "<%s>", tag);
  snprintf(close, sizeof close, "</%s>", tag);
  const char *a = strstr(from, open);
  if (!a || a >= end) { out[0] = '\0'; return nullptr; }
  a += strlen(open);
  const char *b = strstr(a, close);
  if (!b || b > end) { out[0] = '\0'; return nullptr; }
  if (strncmp(a, "<![CDATA[", 9) == 0) { a += 9; if (b - 3 > a && strncmp(b - 3, "]]>", 3) == 0) b -= 3; }
  size_t l = (size_t)(b - a);
  if (l >= n) l = n - 1;
  memcpy(out, a, l);
  out[l] = '\0';
  return b;
}
static void doSearch() {
  char url[200];
  snprintf(url, sizeof url,
           "https://query1.finance.yahoo.com/v1/finance/search?q=%s&quotesCount=10&newsCount=0&listsCount=0", searchQ);
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  Hit got[MAX_HITS];
  uint8_t n = 0;
  bool ok = false;
  if (http.begin(client, url)) {
    http.addHeader("User-Agent", UA);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();  // ~2-4KB
      JsonDocument filter;
      JsonObject q = filter["quotes"][0].to<JsonObject>();
      for (const char *k : {"symbol", "shortname", "longname", "exchange", "quoteType"}) q[k] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        ok = true;
        for (JsonObject o : doc["quotes"].as<JsonArray>()) {
          const char *type = o["quoteType"] | "";
          if (strcmp(type, "EQUITY") && strcmp(type, "ETF") && strcmp(type, "CRYPTOCURRENCY") && strcmp(type, "INDEX") &&
              strcmp(type, "MUTUALFUND"))
            continue;  // futures and options are not for this panel
          const char *sym = o["symbol"] | "";
          if (!*sym || strlen(sym) >= sizeof got[n].sym || n >= MAX_HITS) continue;
          strcpy(got[n].sym, sym);
          snprintf(got[n].name, sizeof got[n].name, "%s", o["shortname"] | (o["longname"] | ""));
          snprintf(got[n].exch, sizeof got[n].exch, "%s", o["exchange"] | "");
          n++;
        }
      }
    } else {
      Serial.printf("search http %d\n", code);
    }
    http.end();
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  nHits = n;
  searchDone = true;
  searchFailed = !ok;
  memcpy(hits, got, sizeof hits);
  xSemaphoreGive(mux);
  Serial.printf("search '%s': %u hits%s (heap %u)\n", searchQ, n, ok ? "" : " FAILED", ESP.getFreeHeap());
}

static void doNews(uint8_t idx) {
  Row r = rowCopy(idx);
  char url[160];
  snprintf(url, sizeof url, "https://feeds.finance.yahoo.com/rss/2.0/headline?s=%s%s&region=US&lang=en-US", r.label,
           r.coin ? "-USD" : "");
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  NewsItem got[NEWS_N];
  uint8_t n = 0;
  bool ok = false;
  if (http.begin(client, url)) {
    http.addHeader("User-Agent", UA);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();  // ~12KB
      ok = true;
      const char *p = body.c_str();
      while (n < NEWS_N) {
        const char *item = strstr(p, "<item>");
        if (!item) break;
        const char *end = strstr(item, "</item>");
        if (!end) break;
        char date[40];
        xmlText(item, end, "title", got[n].title, sizeof got[n].title);
        xmlText(item, end, "pubDate", date, sizeof date);
        decodeEntities(got[n].title);
        ageOf(parseRfc822(date), got[n].age, sizeof got[n].age);
        if (got[n].title[0]) n++;
        p = end + 7;
      }
    } else {
      Serial.printf("news %s http %d\n", r.label, code);
    }
    http.end();
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  newsIdx = idx;
  newsN = n;
  newsFailed = !ok;
  newsAt = millis();
  memcpy(news, got, sizeof news);
  xSemaphoreGive(mux);
  Serial.printf("news %s: %u headlines%s (heap %u)\n", r.label, n, ok ? "" : " FAILED", ESP.getFreeHeap());
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
  uint32_t ver = listVersion;
  Row r = rowCopy(i);
  NetworkClientSecure client;
  client.setInsecure();  // public read-only quotes; pinning buys nothing
  bool ok = fetchStock(client, r);
  if (ok && ver != listVersion) {
    Serial.printf("%s: rows changed during the fetch, dropped\n", r.label);
    return;
  }
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
    if (searchWant) {
      searchWant = false;
      doSearch();
      continue;
    }
    int16_t nw = newsWant;
    if (nw >= 0) {  // the news page is waiting
      newsWant = -1;
      doNews((uint8_t)nw);
      continue;
    }
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
    Session ses = haveTime ? sessionNow(t) : Session::Regular;
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

// ── touch: tap, tap-up, vertical drag, horizontal swipe ──────────────────
// A finger that holds still for 100ms is a Tap, fired then and there (the
// list uses it to highlight the row; pages use it as the press). When a
// still finger lifts it is a TapUp -- the list opens the stock on that, so
// it is as quick as the lift itself and a drag can start on a row. If the
// press was quicker than 100ms, tapUpQuick says the Tap never fired so the
// TapUp counts as the tap. Once the finger has moved 24px with one axis
// clearly winning it locks to that axis: vertical is a drag, reported as
// the delta since the last poll; horizontal is a swipe if it goes 50px by
// release. Resistive panels jitter on first contact and drop contact for a
// poll or two mid-stroke, so a release counts only after three polls
// without contact. Every stroke is traced on serial.
enum class Gesture : uint8_t { None, Tap, TapUp, LongPress, Drag, SwipeRight, SwipeLeft, SwipeUp, SwipeDown };
static const uint32_t TAP_MS = 100, LONG_MS = 700;  // a still finger at 700ms is a long press (removal asks for one)
static const int16_t AXIS_PX = 24, SWIPE_PX = 50;
static bool tapUpQuick = false;
static Gesture pollGesture(int16_t *x, int16_t *y, int16_t *dy) {
  static int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
  static uint32_t t0 = 0, lastTap = 0;
  static uint8_t axis = 0, gap = 0;  // axis: 0 undecided, 1 horizontal, 2 vertical
  static bool tapped = false, longed = false;
  int16_t cx, cy;
  bool contact = touchRead(&cx, &cy);
  if (contact) gap = 0;
  else if (touchHeld && ++gap < 3) return Gesture::None;  // a dropped poll, not a release
  bool now = contact;
  Gesture g = Gesture::None;
  if (now && !touchHeld) {
    x0 = lx = cx;
    y0 = ly = cy;
    t0 = millis();
    axis = 0;
    tapped = longed = false;
  } else if (now) {
    int16_t ddx = cx - x0, ddy = cy - y0;
    if (!axis && (abs(ddx) >= AXIS_PX || abs(ddy) >= AXIS_PX)) {
      if (abs(ddx) * 2 >= abs(ddy) * 3) axis = 1;
      else if (abs(ddy) * 2 >= abs(ddx) * 3) axis = 2;
    }
    if (axis == 2) {
      *dy = cy - ly;
      g = Gesture::Drag;
    } else if (!axis && !tapped && millis() - t0 >= TAP_MS && millis() - lastTap > 150) {
      tapped = true;
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::Tap;
    } else if (!axis && tapped && !longed && millis() - t0 >= LONG_MS) {
      longed = true;
      *x = x0;
      *y = y0;
      g = Gesture::LongPress;
    }
    lx = cx;
    ly = cy;
  } else if (touchHeld) {  // release
    int16_t ddx = lx - x0, ddy = ly - y0;
    if (axis == 1) {
      if (ddx >= SWIPE_PX) g = Gesture::SwipeRight;
      else if (ddx <= -SWIPE_PX) g = Gesture::SwipeLeft;
    } else if (axis == 2) {  // a vertical drag that went far enough is also a swipe; the list ignores it
      if (ddy >= SWIPE_PX) g = Gesture::SwipeDown;
      else if (ddy <= -SWIPE_PX) g = Gesture::SwipeUp;
    } else {
      tapUpQuick = !tapped;
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::TapUp;
    }
    static const char *const names[] = {"none", "tap", "tap-up", "long", "drag", "swipe right", "swipe left", "swipe up", "swipe down"};
    Serial.printf("touch: %d,%d -> %d,%d  %lums  axis %c  %s\n", x0, y0, lx, ly, (unsigned long)(millis() - t0),
                  axis == 1 ? 'h' : axis == 2 ? 'v' : '-', names[(uint8_t)g]);
  }
  touchHeld = now;
  return g;
}

// ── the LED glows with the day; the speaker chimes on a crossing ─────────
// Green or red by the average move of the valid stocks, brighter for a
// bigger move (3% is full), scaled with the backlight so it fades with the
// room, off when the market is closed or the LED is off in settings.
static void ledTick(Session ses) {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (!(sLed & 1) || ses == Session::Closed || blApplied < 0) {
    ledGlow(0, 0, 0);
    return;
  }
  float sum = 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < nRows; i++)
    if (rows[i].valid && !rows[i].coin) {  // a float read; no lock needed
      sum += rows[i].pct;
      n++;
    }
  if (!n) {
    ledGlow(0, 0, 0);
    return;
  }
  float avg = sum / n, mag = min(fabsf(avg) / 3.0f, 1.0f);
  uint8_t v = (uint8_t)((16 + 160 * mag) * blApplied / 255);
  if (avg >= 0) ledGlow(0, v, 0);
  else ledGlow(v, 0, 0);
}
// Three rising notes and two white blinks. Blocking for ~0.6s, which is
// fine for something that happens a few times a day.
static void chime(const char *why) {
  Serial.printf("alert: %s\n", why);
  static const uint16_t notes[] = {880, 1109, 1319};
  for (uint8_t i = 0; i < 3; i++) {
    if (sSound & 2) tone(SPK, notes[i], 120);
    if (sLed & 2) ledGlow(i & 1 ? 0 : 255, i & 1 ? 0 : 255, i & 1 ? 0 : 255);
    delay(150);
  }
  ledGlow(0, 0, 0);
}
static void alertTick() {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  char why[48];
  for (uint8_t i = 0; i < nRows; i++) {
    Row r = rowCopy(i);
    if (!r.valid) continue;
    if (movePct > 0 && !r.coin) {
      uint32_t bit = 1UL << i;
      if (fabsf(r.pct) >= movePct && !(moveFired & bit)) {
        moveFired |= bit;
        snprintf(why, sizeof why, "%s moved %+.1f%% today", r.label, r.pct);
        chime(why);
      } else if (fabsf(r.pct) < movePct / 2) {
        moveFired &= ~bit;
      }
    }
    for (uint8_t a = 0; a < nAlerts; a++) {
      Alert &al = alerts[a];
      if (strcmp(al.sym, r.label) != 0) continue;
      bool hit = (al.above > 0 && r.price >= al.above) || (al.below > 0 && r.price <= al.below);
      bool back = !((al.above > 0 && r.price >= al.above * 0.99f) || (al.below > 0 && r.price <= al.below * 1.01f));
      if (hit && !al.fired) {
        al.fired = true;
        snprintf(why, sizeof why, "%s at %.2f, %s %.2f", r.label, r.price, al.above > 0 && r.price >= al.above ? "above" : "below",
                 al.above > 0 && r.price >= al.above ? al.above : al.below);
        chime(why);
      } else if (back) {
        al.fired = false;
      }
    }
  }
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
  assert(inNight(23, 23, 7) && inNight(3, 23, 7) && !inNight(7, 23, 7) && !inNight(12, 23, 7));
  assert(inNight(1, 0, 6) && !inNight(6, 0, 6) && !inNight(5, 23, 23));
  {  // wake timer: night ends at 07:00; closed ends at the next weekday's 04:00; six-hour cap
    struct tm w = {};
    w.tm_wday = 3; w.tm_hour = 23; w.tm_min = 30;
    sSleep = 1;  assert(secondsUntilWake(w) == 6UL * 3600);  // 7.5h away, capped
    w.tm_hour = 5; w.tm_min = 0;  assert(secondsUntilWake(w) == 2UL * 3600 + 5);  // 05:00 -> 07:00
    sSleep = 2;  w.tm_hour = 2;  assert(secondsUntilWake(w) == 2UL * 3600 + 5);  // Wed 02:00 -> 04:00
    w.tm_wday = 6; w.tm_hour = 12;  assert(secondsUntilWake(w) == 6UL * 3600);   // Sat -> Mon, capped
    sSleep = 0;
  }
  {  // calibration: a plain panel, a swapped-and-mirrored one, and two taps in one place
    TouchCal c;
    uint16_t plain[3][2] = {{500, 500}, {3400, 520}, {480, 3500}};
    assert(touchCalFrom(plain, &c) && !c.swap && c.xMin < 500 && c.xMax > 3400 && c.yMin < 500 && c.yMax > 3500);
    int16_t x, y;
    touchMap(c, 500, 500, &x, &y);
    assert(abs(x - 20) <= 1 && abs(y - 20) <= 1);
    uint16_t sw[3][2] = {{3500, 500}, {3480, 3400}, {600, 520}};  // chip Y runs along screen X; chip X mirrored
    assert(touchCalFrom(sw, &c) && c.swap && c.yMin > c.yMax);
    touchMap(c, 600, 520, &x, &y);
    assert(abs(x - 20) <= 1 && abs(y - (LCD_H - 21)) <= 1);
    uint16_t same[3][2] = {{500, 500}, {520, 510}, {480, 3500}};
    assert(!touchCalFrom(same, &c));
    assert(parseRfc822("Wed, 16 Sep 2026 14:05:28 +0000") == 1789567528L && parseRfc822("junk") == 0);
    char t[40] = "A &amp; B&#39;s &quot;x&quot;";
    decodeEntities(t);
    assert(strcmp(t, "A & B's \"x\"") == 0);
  }
  char nx[24];
  t.tm_wday = 6; t.tm_hour = 12;  nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "Mon 09:30") == 0);
  t.tm_wday = 5; t.tm_hour = 17;  nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "Mon 09:30") == 0);
  t.tm_wday = 2; t.tm_hour = 5;   nextOpen(t, nx, sizeof nx);  assert(strcmp(nx, "09:30") == 0);

  {
    char l[40] = "";
    listAppend(l, sizeof l, "AAPL");
    listAppend(l, sizeof l, "BTC-USD");
    listAppend(l, sizeof l, "AAPL");
    assert(strcmp(l, "AAPL,BTC-USD") == 0 && listHas(l, "AAPL") && listHas(l, "BTC-USD") && !listHas(l, "BTC"));
    listRemove(l, "AAPL");
    assert(strcmp(l, "BTC-USD") == 0);
    char lab[MAX_LABEL + 1];
    labelFor("BTC-USD", lab, sizeof lab);  assert(strcmp(lab, "BTC") == 0);
    labelFor("GOOGL", lab, sizeof lab);    assert(strcmp(lab, "GOOGL") == 0);
    labelFor("BRK-B", lab, sizeof lab);    assert(strcmp(lab, "BRK-B") == 0);
    assert(hitKey(0, K_Y0 - 1) == -1 && hitKey(0, K_Y0) == 0 && hitKey(239, K_Y0 + 4 * K_H) == 29);
  }
  {  // holiday: a weekday at 10:00 with the only stock last traded yesterday
    struct tm w = {};
    w.tm_wday = 3; w.tm_hour = 10; w.tm_year = 126; w.tm_yday = 258;
    assert(!holidayFrom(w));  // no rows: nothing to go on
    assert(addRow("AAA", "aaa", false));
    rows[0].valid = true;
    time_t now = time(nullptr);
    rows[0].traded = now;
    struct tm tn;
    localtime_r(&now, &tn);
    w.tm_year = tn.tm_year; w.tm_yday = tn.tm_yday; w.tm_wday = tn.tm_wday == 0 || tn.tm_wday == 6 ? 3 : tn.tm_wday;
    assert(!holidayFrom(w));
    rows[0].traded = now - 86400 * 3;
    assert(holidayFrom(w));
    w.tm_hour = 9; w.tm_min = 30;
    assert(!holidayFrom(w));  // before open + 15: yesterday's trade is normal
    nRows = 0;
  }
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
  for (const Face &f : FACES) assert(f.font[13] == f.cap && (uint8_t)(-(int8_t)f.font[14]) == f.desc);  // u8g2 header: ascent_A, descent_g
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoConfig, NoWifi, NoData, Running };
static uint32_t joinStarted = 0;  // the splash holds for 20s of joining, then the panel says why
enum class View { List, Detail, Settings, Info, Calib, News, Search, Splash };
static uint32_t removeArmedUntil = 0;  // a long press on a stock's page arms removal for a few seconds
static State state = State::Boot;
static View view = View::List;

void setup() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);  // dark until there is something to show; matters after a sleep wake
  Serial.begin(115200);
  delay(300);
  boardBegin();
  selfCheck();

  // What woke us. A timer wake inside the sleep window goes straight back
  // to sleep without touching the panel or the radio.
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool fromSleep = cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_TIMER;
  if (fromSleep) Serial.printf("woke by %s\n", cause == ESP_SLEEP_WAKEUP_EXT0 ? "touch" : "timer");

  mux = xSemaphoreCreateMutex();
  if (loadConfig()) applyOverlay();
  if (cfgErr) {
    Serial.printf("config error: %s\n", cfgErr);
    gfx = boardDisplay();
    gfx->begin();
    gfx->setTextWrap(false);
    backlight(blMax);
    return;  // loop() draws the panel
  }
  cfgRelease();
  loadSettings();
  setenv("TZ", tzString, 1);  // the clock survives deep sleep; the zone must be set before it is read
  tzset();
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  if (cause == ESP_SLEEP_WAKEUP_TIMER && haveTime && sleepDue(t)) goToSleep(secondsUntilWake(t));
  awakeUntil = millis() + 60000;

  gfx = boardDisplay();
  gfx->begin();
  gfx->fillScreen(C_BG);
  gfx->setTextWrap(false);
  rowCanvas = new Arduino_Canvas(LCD_W, ROW_H, nullptr);
  if (!rowCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) Serial.println("row canvas: alloc failed");  // 20KB
  rowCanvas->setTextWrap(false);
  logoInventory();

  // Back from sleep with the snapshot intact: show it now, join later.
  bool restored = fromSleep && restoreFromRtc();
  if (restored) {
    Serial.println("prices restored from RTC memory");
    listStart();
  } else {
    char st[40];
    snprintf(st, sizeof st, "connecting to %.24s", WIFI_SSID);
    drawSplash(st);
    state = State::Boot;
  }
  backlight(blMax);
  // The join runs in the background from here: netTick() in loop() issues
  // the begin() when the scan is in, starts the clock when the link is up,
  // and the state machine keeps the splash (or the restored list) meanwhile.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinStart();
  joinStarted = millis();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
}

// The non-blocking join: the scan's begin(), then NTP once the link is up.
static void netTick() {
  static bool scanning = true, clockStarted = false;
  if (scanning && netJoinTick(WIFI_SSID, WIFI_PASS)) scanning = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !clockStarted) {
    clockStarted = true;
    Serial.printf("wifi ok %s %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
    configTzTime(tzString, "pool.ntp.org", "time.nist.gov");
  }
  // Retry with a fresh scan, backing off, whenever the link is down for a
  // while -- whatever is on the screen.
  static uint32_t lastRetry = 0;
  static uint16_t retries = 0;
  if (up) {
    retries = 0;
    lastRetry = 0;
  } else if (millis() - joinStarted > WIFI_RETRY_MS &&
             (lastRetry == 0 || millis() - lastRetry > netRetryDelay(retries, WIFI_RETRY_MS))) {
    lastRetry = millis();
    Serial.printf("wifi retry #%u\n", ++retries);
    WiFi.disconnect();
    netJoinStart();
    scanning = true;
  }
}

static void drawFailPanel() {
  static char key[32];
  char k[32];
  snprintf(k, sizeof k, "%d-%u", (int)state, failures);
  if (strcmp(k, key) == 0) return;
  strcpy(key, k);
  if (state == State::NoConfig) {
    const char *l[] = {cfgErr, "", "upload the watchlist:", "  ./push-config ticker", "", "or swipe left to search"};
    drawPanel("NO LIST", C_WARN, l, 6);
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
  if (refreshOnOpen && !(getLocalTime(&t, 0) && sessionNow(t) == Session::Closed)) priority = idx;  // nothing new when closed
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
// Headlines for symbol idx; the fetch is skipped if they are under 10 min old.
static void openNews(uint8_t idx) {
  view = View::News;
  detailIdx = idx;
  detailOpenedAt = millis();
  if (!(newsIdx == idx && millis() - newsAt < 10UL * 60 * 1000)) newsWant = idx;
  panelScroll(0);
  drawNews(true);
}
static void openSearch() {
  view = View::Search;
  searchMode = 0;
  pageOpenedAt = millis();
  panelScroll(0);
  drawSearch(true);
}
static void searchKey(int8_t k) {
  size_t l = strlen(query);
  if (k == 28) {
    if (l) query[l - 1] = '\0';
  } else if (k == 29) {
    if (!l) return;
    strcpy(searchQ, query);
    xSemaphoreTake(mux, portMAX_DELAY);
    nHits = 0;
    searchDone = searchFailed = false;
    xSemaphoreGive(mux);
    searchWant = true;
    searchMode = 1;
    drawSearch(true);
    return;
  } else if (l < sizeof query - 1) {
    query[l] = KEYS[k];
    query[l + 1] = '\0';
  }
  drawQuery();
}
// A result was tapped: on the list already, or added now; either way its page opens.
static void chooseHit(uint8_t i) {
  Hit h;
  xSemaphoreTake(mux, portMAX_DELAY);
  h = hits[i];
  xSemaphoreGive(mux);
  int8_t idx = userAdd(h.sym);
  if (idx < 0) {
    drawHint("list is full (32)");
    return;
  }
  rows[idx].valid = false;
  priority = idx;  // a fresh row needs its first fetch whatever the session
  openDetail((uint8_t)idx);
}


void loop() {
  int16_t tx, ty;
  int16_t ddy = 0;
  Gesture g = pollGesture(&tx, &ty, &ddy);
  bool tap = g == Gesture::Tap || (g == Gesture::TapUp && tapUpQuick);
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  bool open = haveTime && marketOpen(t);
  static uint32_t holidayAt = 0;
  if (haveTime && millis() - holidayAt > 10000) {  // 32 localtime_r calls; not every pass
    holidayAt = millis();
    bool h = holidayFrom(t);
    if (h != holiday) Serial.println(h ? "market holiday: no stock has traded today" : "trading again");
    holiday = h;
  }
  Session ses = haveTime ? sessionNow(t) : Session::Regular;

  // Sleep: once the chosen condition holds and nobody has touched the panel
  // for a minute, the chip deep-sleeps (see goToSleep). Not from a
  // calibration or a fail panel: those need the screen.
  if (touchHeld) awakeUntil = millis() + 60000;
  if (state == State::Running && view != View::Calib && haveTime && millis() > awakeUntil && sleepDue(t))
    goToSleep(secondsUntilWake(t));

  netTick();
  bool anyValid = false;
  for (uint8_t i = 0; i < nRows; i++) anyValid |= rows[i].valid;  // a bool read; no lock needed
  bool up = WiFi.status() == WL_CONNECTED, joining = !up && millis() - joinStarted < 20000;
  bool firstFetch = up && !anyValid && failures == 0 && millis() - joinStarted < 60000;
  // Prices on hand (restored from sleep, or fetched before the link dropped)
  // beat any panel: the list stays up and the join or retry runs behind it.
  State want = cfgErr                  ? State::NoConfig
               : anyValid              ? State::Running
               : joining || firstFetch ? State::Boot  // the splash, with its status line
               : !up                   ? State::NoWifi
                                       : State::NoData;
  if (want != state) {
    state = want;
    invalidateCache();
    panelScroll(0);  // panels draw the screen 1:1
    if (state == State::Running) {
      if (view == View::List) listStart();
      else if (view == View::Settings || view == View::Calib) { view = View::Settings; drawSettings(); }
      else if (view == View::Info) drawInfo(true);
      else if (view == View::News) drawNews(true);
      else if (view == View::Search) drawSearch(true);
      else if (view == View::Splash) drawSplash("tap to return");
      else drawDetail(true);
    }
  }
  if (state == State::NoConfig) {
    if (g == Gesture::SwipeLeft && WiFi.status() == WL_CONNECTED && nRows < MAX_SYMBOLS) {
      cfgErr = nullptr;  // the search page will add one; the panel comes back if it does not
      openSearch();
      state = State::Running;
    } else {
      drawFailPanel();
    }
    delay(50);
    return;
  }

  if (state == State::Boot) {  // the splash is up from setup; only its status line moves
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 1000) {
      lastStatus = millis();
      char st[40];
      if (!up) snprintf(st, sizeof st, "connecting to %.24s", WIFI_SSID);
      else if (!haveTime) snprintf(st, sizeof st, "connected, setting the clock");
      else snprintf(st, sizeof st, "fetching prices");
      splashStatus(st);
    }
  } else if (state != State::Running) {
    drawFailPanel();
  } else if (view == View::List) {
    drawHead(&t, haveTime);
    listTick();
  } else if (view == View::Info) {
    drawInfo(false);
  } else if (view == View::Detail) {
    drawDetail(false);
  } else if (view == View::News) {
    drawNews(false);
  } else if (view == View::Search) {
    drawSearch(false);
  } else if (view == View::Calib) {
    if (calibTick()) {
      view = View::Settings;
      pageOpenedAt = millis();
      drawSettings();
    }
    brightnessTick(open);
    delay(20);
    return;  // raw touch only; no gestures on this page
  }

  // On the list: drag scrolls it, any touch holds the crawl for 3s after,
  // and a long press opens the stock. Swipe right opens settings; swipe
  // left there goes back.
  // On the list: a still finger highlights its row at once and opens the
  // stock when it lifts (touch-up, the way a phone list works); a moving
  // finger drags the list; any touch holds the crawl for 3s after.
  static int8_t hilite = -1;
  if (state == State::Running && view == View::List) {
    if (touchHeld) holdUntil = millis() + 3000;
    if (g == Gesture::Drag && ddy) scrollBy(-ddy);
    if (g == Gesture::Tap && hilite < 0) {
      int16_t r = hitRow(ty, pos, nRows);
      if (r >= 0) {
        hilite = slotOf(floordiv(pos + ty - Y_ROW0, ROW_H));
        gfx->fillRect(0, Y_ROW0 + hilite * ROW_H + 2, 3, ROW_H - 4, C_FG);
        if (sSound & 1) tone(SPK, 1200, 15);
      }
    }
    if (hilite >= 0 && (!touchHeld || g == Gesture::Drag)) {
      gfx->fillRect(0, Y_ROW0 + hilite * ROW_H + 2, 3, ROW_H - 4, C_BG);
      hilite = -1;
    }
    if (g == Gesture::TapUp) {
      int16_t r = hitRow(ty, pos, nRows);
      if (r >= 0) openDetail((uint8_t)r);
    }
  }
  // Swipes are the navigation, phone style. List: right opens settings.
  // Detail: left is the next stock, right the previous, down the list.
  // Settings: left is the list. Info: left the list, down settings.
  bool swipe = g == Gesture::SwipeLeft || g == Gesture::SwipeRight || g == Gesture::SwipeDown || g == Gesture::SwipeUp;
  if (swipe && state == State::Running) {
    bool acts = (view == View::List && (g == Gesture::SwipeRight || g == Gesture::SwipeLeft)) || view == View::Detail ||
                (view == View::News && g != Gesture::SwipeUp) ||
                (view == View::Settings && g == Gesture::SwipeLeft) ||
                (view == View::Search && g == Gesture::SwipeDown) ||
                (view == View::Info && (g == Gesture::SwipeLeft || g == Gesture::SwipeDown)) || view == View::Splash;
    if (acts && (sSound & 1)) tone(SPK, 1200, 15);
    if (view == View::List && g == Gesture::SwipeRight) {
      view = View::Settings;
      pageOpenedAt = millis();
      panelScroll(0);
      drawSettings();
    } else if (view == View::List && g == Gesture::SwipeLeft) {
      openSearch();
    } else if (view == View::Search && g == Gesture::SwipeDown) {
      if (searchMode == 1) {
        searchMode = 0;
        pageOpenedAt = millis();
        drawSearch(true);
      } else {
        backToList();
      }
    } else if (view == View::Detail) {
      if (g == Gesture::SwipeLeft) openDetail((detailIdx + 1) % nRows);
      else if (g == Gesture::SwipeRight) openDetail((detailIdx + nRows - 1) % nRows);
      else if (g == Gesture::SwipeDown) backToList();
      else if (g == Gesture::SwipeUp) openNews(detailIdx);
    } else if (view == View::News) {
      if (g == Gesture::SwipeLeft) openNews((detailIdx + 1) % nRows);
      else if (g == Gesture::SwipeRight) openNews((detailIdx + nRows - 1) % nRows);
      else if (g == Gesture::SwipeDown) { view = View::Detail; detailOpenedAt = millis(); drawDetail(true); }
    } else if (view == View::Settings && g == Gesture::SwipeLeft) {
      backToList();
    } else if (view == View::Info) {
      if (g == Gesture::SwipeLeft) backToList();
      else if (g == Gesture::SwipeDown) {
        view = View::Settings;
        pageOpenedAt = millis();
        drawSettings();
      }
    } else if (view == View::Splash) {  // any swipe brings the info page back
      view = View::Info;
      pageOpenedAt = millis();
      drawInfo(true);
    }
  }

  // A long press on a stock's page arms removal; a tap on the hint line
  // within five seconds does it. Anything else lets it lapse.
  if (view == View::Detail && g == Gesture::LongPress && state == State::Running) {
    removeArmedUntil = millis() + 5000;
    char m[40];
    snprintf(m, sizeof m, "tap here to remove %s", rows[detailIdx].label);
    drawHint(m, C_WARN);
    if (sSound & 1) tone(SPK, 600, 40);
  }
  if (view == View::Detail && removeArmedUntil && millis() > removeArmedUntil) {
    removeArmedUntil = 0;
    drawHint("< next    ^ news    v list    prev >");
  }
  if (tap && state == State::Running && view == View::Detail && removeArmedUntil && ty > D_Y_WK) {
    removeArmedUntil = 0;
    userRemove(detailIdx);
    if (sSound & 1) tone(SPK, 500, 120);
    if (nRows == 0) {
      cfgErr = "every symbol removed; swipe left to add one";
      state = State::Boot;  // the loop routes to the panel
    } else {
      backToList();
    }
    tap = false;
  }

  if (tap && state == State::Running && view != View::List) {
    if (sSound & 1) tone(SPK, 1200, 15);
    if (view == View::Search) {
      pageOpenedAt = millis();
      if (searchMode == 0) {
        int8_t k = hitKey(tx, ty);
        if (k >= 0) searchKey(k);
      } else {
        int8_t i = hitResult(ty);
        if (i >= 0) chooseHit((uint8_t)i);
      }
    } else if (view == View::Settings) {
      int8_t i = hitSetting(ty);
      pageOpenedAt = millis();
      uint8_t r = i >= 0 ? tapSetting((uint8_t)i) : 0;
      if (r == 1) {
        view = View::Info;
        drawInfo(true);
      } else if (r == 2) {
        view = View::Calib;
        calStep = 0;
        drawCalTarget();
      }
    } else if (view == View::Info) {  // a tap shows the splash, as the last line says
      view = View::Splash;
      pageOpenedAt = millis();
      drawSplash("tap to return");
    } else if (view == View::Splash) {
      view = View::Info;
      pageOpenedAt = millis();
      drawInfo(true);
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

  if ((view == View::Detail || view == View::News) && returnMs && !touchHeld && millis() - detailOpenedAt > returnMs)
    backToList();
  if ((view == View::Info || view == View::Settings || view == View::Search || view == View::Splash) && returnMs && !touchHeld &&
      millis() - pageOpenedAt > returnMs)
    backToList();

  brightnessTick(open);
  ledTick(ses);
  alertTick();

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
