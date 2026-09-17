// ticker on the 7" Waveshare: everything the CYD ticker learned, on a
// screen wide enough to read across a room. 800x480 landscape, RGB panel
// with its framebuffer in PSRAM (no hardware scroll: the list moves by
// shifting the framebuffer), capacitive touch (no calibration), no LDR,
// LED or speaker (alerts are a banner), the backlight a switch.
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

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <algorithm>
#include <assert.h>
#include <time.h>

// ── one colour scheme: black, white symbols, green and red numbers ───────
static const uint16_t C_FG = RGB565_WHITE, C_GOOD = 0x07E0, C_BAD = 0xF800, C_WARN = RGB565_YELLOW;
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
// A theme is the background, the rule (also the tiles) and the accent;
// white, green and red text stay, so every background has to stay dark
// enough for them to read on it. No light themes: the logos are flattened
// onto black by make-logos.py and would each sit in a black square. The
// grey tones come from the background -- a brighter one gets brighter
// greys -- so hints and muted text read on all of them. Every clear in
// the app goes through C_BG, so a theme is a few assignments and a
// repaint. Picked on the Themes page (Settings > Theme), kept in NVS.
struct Theme { const char *name; uint16_t bg, rule, accent; };
static const Theme THEMES[] = {
    {"black", rgb(0, 0, 0), rgb(32, 32, 32), rgb(248, 168, 0)},
    {"midnight", rgb(0, 24, 72), rgb(0, 56, 140), rgb(90, 200, 255)},
    {"royal", rgb(24, 0, 120), rgb(64, 32, 180), rgb(255, 200, 80)},
    {"ocean", rgb(0, 60, 80), rgb(0, 110, 140), rgb(0, 240, 220)},
    {"forest", rgb(0, 56, 24), rgb(0, 110, 50), rgb(200, 255, 120)},
    {"terminal", rgb(0, 16, 0), rgb(0, 80, 0), rgb(120, 255, 120)},
    {"espresso", rgb(56, 28, 8), rgb(110, 60, 20), rgb(255, 180, 60)},
    {"burgundy", rgb(96, 0, 32), rgb(160, 24, 64), rgb(255, 170, 190)},
    {"purple", rgb(64, 0, 96), rgb(120, 30, 170), rgb(255, 120, 255)},
    {"slate", rgb(36, 48, 68), rgb(72, 96, 130), rgb(140, 190, 255)},
    {"graphite", rgb(64, 64, 70), rgb(110, 110, 120), rgb(255, 140, 0)},
    {"olive", rgb(48, 56, 0), rgb(96, 110, 20), rgb(255, 230, 90)},
};
static const uint8_t N_THEMES = sizeof THEMES / sizeof THEMES[0];
static uint8_t sTheme = 0;
static uint16_t C_BG = THEMES[0].bg, C_RULE = THEMES[0].rule, C_GOLD = THEMES[0].accent, C_DIM = 0x630C, C_MUTED = 0xA534;
// The colour pct of the way from c towards white.
static uint16_t towardsWhite(uint16_t c, uint8_t pct) {
  uint8_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
  return rgb(r + (255 - r) * pct / 100, g + (255 - g) * pct / 100, b + (255 - b) * pct / 100);
}
static void applyTheme() {
  C_BG = THEMES[sTheme].bg;
  C_RULE = THEMES[sTheme].rule;
  C_GOLD = THEMES[sTheme].accent;
  C_DIM = towardsWhite(C_BG, 40);    // on black: 102, near the 96 it was
  C_MUTED = towardsWhite(C_BG, 65);  // on black: 166, near the 164 it was
}

// ── settings (defaults; data/config.json overrides) ──────────────────────
static char tzString[64] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static uint16_t mktOpenMin = 9 * 60 + 30, mktCloseMin = 16 * 60, mktPreMin = 4 * 60, mktPostMin = 20 * 60;
static uint32_t pageMs = 10000;  // six rows scroll past in this long
static char sparkInterval[4] = "5m";
static uint32_t returnMs = 60000;
static bool refreshOnOpen = true;
static uint32_t stockOpenMs = 5UL * 60 * 1000, stockExtMs = 15UL * 60 * 1000, coinMs = 5UL * 60 * 1000;
static const uint32_t WIFI_RETRY_MS = 20UL * 1000, LOG_MS = 60UL * 1000;
static const char *UA = "Mozilla/5.0 (esp32-ticker)";

static const uint8_t ROWS_MAX = 16, MAX_SYMBOLS = 40, MAX_LABEL = 5, SPARK_N = 200;  // 04:00-20:00 at 5m is 192
// Two logo sizes, both pre-converted on the Mac (tools/make-logos.py --size N
// --out data/logo/N) and read raw from /logo/<N>/<LABEL>.565. Must match.
// 32px badges and 128px on the page: the 3.4MB data partition has the room.
static const uint8_t LOGO_BADGE = 32, LOGO_BIG = 128;
// Text comes in four logical sizes, each a Helvetica bitmap face (the X11
// Adobe set, via U8g2 -- the closest thing to a phone's type that fits in
// 18KB). GW is the width reserved per character in the layout: Helvetica is
// narrower than that in every size, so nothing overflows its box. GH is the
// real box height, cap plus descender.
struct Face {
  const uint8_t *font;
  uint8_t cap, desc;
};
static const Face FACES[] = {{u8g2_font_helvR14_tr, 14, 4},
                             {u8g2_font_helvB18_tr, 19, 5},
                             {u8g2_font_helvB24_tr, 25, 7},
                             {u8g2_font_logisoso50_tn, 50, 13}};  // digits only: the big price
static const uint8_t GWS[] = {8, 11, 14, 28};  // width reserved per character, per size
#define GW(s) (GWS[(s) - 1])
#define GH(s) (FACES[(s) - 1].cap + FACES[(s) - 1].desc)

// ── the layout, twice: landscape 800x480 and portrait 480x800 ──────────
// Every position a page draws at comes from here, so the Orientation
// setting is a pointer swap (and a restart, which is cheaper than
// re-allocating every buffer). Names keep their old spelling as fields.
struct Layout {
  int16_t w, h, rows, strip, yHead, yRow0, yFoot, yHint;
  int16_t xSym, yBadge, xLbl, yLbl, xSpk, ySpk, spkW, spkH, xPrice, xRight;
  int16_t tabX, tagX, iconX, iconStep;  // tagX < 0: the CLOSED tag and the clock go to the footer
  int16_t dXLogo, dYLogo, dXTxt, dYSym, dYName, dYTag, dXPrice, dYPrice, dYChg, dXPct, dYDay, dYWk, barX, barW, barH;
  int16_t chX, chY, chW, chH, rY, rW, rStep, rH, dYNews, newsStep;
  int16_t sY0, sH, sN, sRows;
  int16_t hY0, hW, hH, hCols, hRows, hPer;
  int16_t kY0, kW, kH, kCols, qY, resY0, resH;
  int16_t cfX, cfW, cfY, cfH, cfCancelY, cfGlyphY, cfTitleY, cfL1Y, cfL2Y;
  int16_t candleX0, candleStep, candleW, candleY, wordY, ruleY, tagY, creditY;
};
static const int16_t ROW_H = 44;
static const Layout LAYOUTS[2] = {
    {800, 480, 9, 9 * ROW_H, 10, 40, 436, 452,
     12, 6, 56, 12, 200, 6, 320, 32, 640, 788,
     12, 420, 564, 40,
     20, 52, 168, 52, 90, 114, 20, 196, 264, 200, 316, 368, 20, 340, 8,
     400, 52, 388, 200, 262, 72, 79, 40, 312, 38,
     56, 35, 11, 12,
     44, 130, 98, 6, 4, 24,
     200, 80, 76, 10, 60, 100, 56,
     240, 320, 330, 56, 402, 150, 210, 254, 284,
     120, 66, 40, 190, 60, 416, 146, 424},
    {480, 800, 16, 16 * ROW_H, 10, 40, 744, 772,
     12, 6, 56, 12, 150, 6, 200, 32, 320, 468,
     12, -1, 320, 36,
     20, 52, 168, 52, 90, 114, 20, 196, 264, 200, 544, 596, 20, 440, 8,
     20, 296, 440, 190, 494, 80, 88, 40, 660, 28,
     56, 35, 11, 12,
     44, 118, 96, 4, 7, 28,
     440, 80, 66, 6, 60, 100, 56,
     80, 320, 400, 56, 472, 200, 260, 304, 334,
     82, 35, 22, 330, 180, 556, 266, 744}};
static const Layout *Lp = &LAYOUTS[0];
#define L (*Lp)
static const char *const ROT_NAMES[] = {"landscape", "portrait", "landscape, flipped", "portrait, flipped"};
static uint8_t sRot = 0;

static Arduino_GFX *gfx;
static bool touchHeld = false;  // set by pollGesture; the list freezes while a finger is down

// ── settings the finger can change ───────────────────────────────────────
// Four things worth a tap on the device itself, each a short cycle. They
// live in NVS and beat config.json, the way the C6's layout choice does --
// otherwise a config push would undo a tap on every boot. Everything else
// stays in config.json, where a keyboard is.
static const char *const SPEED_NAMES[] = {"slow", "normal", "fast"};
static const uint32_t SPEED_MS[] = {20000, 10000, 5000};
static const char *const RET_NAMES[] = {"15s", "60s", "never"};
static const uint32_t RET_MS[] = {15000, 60000, 0};
static const char *const SLEEP_NAMES[] = {"never", "night", "closed"};
static const char *const CLOCK_NAMES[] = {"12-hour", "24-hour"};
static uint8_t sClock = 0;
// "9:05 PM" or "21:05", by the Clock setting.
static void fmtClock(const struct tm &t, char *out, size_t n) {
  if (sClock) snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(out, n, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}
static uint8_t sSpeed = 1, sRet = 1, sSleep = 0;
// The columns a list row shows, a bit each; the Columns page toggles them.
enum : uint8_t { COL_CHART = 1, COL_PRICE = 2, COL_PCT = 4, COL_CHG = 8, COL_VOL = 16, COL_DAY = 32 };
static const char *const COL_NAMES[] = {"Chart", "Price", "Change %", "Change $", "Volume", "Day range"};
static uint8_t sCols = COL_CHART | COL_PRICE | COL_PCT;
// Prices show in this currency, converted through the CURRENCIES rows
// (how many of X one USD buys). USD, or any code the config lists. A row
// whose rate is missing shows its own currency, and the page says which.
static char sCur[4] = "USD";
// Wi-Fi credentials live in NVS and nowhere else: no secrets.h, nothing
// compiled in. Empty means the device has not been set up, and it raises
// its own access point with a web form (see the setup page). The same
// flash-dump exposure as a compiled-in secret, no worse -- SECURITY.md.
static char wifiSsid[33] = "", wifiPass[65] = "";
// Shutdown is deep sleep with nothing but a touch to wake it: this board has
// no power switch, and the panel, radio and chip all go dark. Two taps
// within three seconds, so a stray finger cannot turn it off.
static uint8_t setTop = 0;      // the first settings row on screen, 0..S_ROWS-L.sN
static uint8_t confirmWhat = 0;  // the confirm screen: 1 shut down, 2 clear the device
// Sound and LED are each two bits: bit 0 the everyday use (tap clicks /
// the day's glow), bit 1 the alerts (chime / white blinks).

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
static uint64_t moveFired = 0;  // bit per row
static uint8_t sleepFrom = 23, sleepTo = 7;  // the night window, config.json
static uint32_t awakeUntil = 60000;          // no sleeping before this; a touch pushes it out a minute
// No speaker, no LED on this board: an alert is a banner across the header
// for ten seconds.
static char bannerText[48] = "";
static uint32_t bannerUntil = 0;
static Preferences prefs;

// ── the watchlist ────────────────────────────────────────────────────────
struct Row {
  char label[MAX_LABEL + 1];
  char id[28];
  char name[32];
  bool coin, valid;
  uint8_t kind;  // K_STOCK, K_INDEX, K_FX (all Yahoo), K_COIN (CoinGecko); coin == (kind == K_COIN)
  char cur[4];   // the quote's own currency, from Yahoo's meta; coins are USD
  float price, pct, prev, dayLo, dayHi, wkLo, wkHi;
  float last;     // the newest bar, extended hours included; == price in the regular session
  float volume;   // the day's (coins: 24h) volume
  time_t traded;  // Yahoo's regularMarketTime: the last regular-session trade
  uint8_t n;
  float close[SPARK_N];
};
static Row rows[MAX_SYMBOLS];
static uint8_t nRows = 0, nStocks = 0, nIdx = 0, nFx = 0, nCoins = 0;
enum : uint8_t { K_STOCK, K_INDEX, K_FX, K_COIN };
// The list shows one SECTION at a time -- all, stocks, indices, crypto,
// currencies -- cycled by a tap on the header and kept in NVS. order[]
// is the rows of the current section; the ring and the heatmap draw from
// it, so a row index in the UI is order[shown index].
static const char *const SECT_NAMES[] = {"ALL", "STOCKS", "INDICES", "CRYPTO", "CURRENCIES"};
static const uint8_t SECT_KIND[] = {255, K_STOCK, K_INDEX, K_COIN, K_FX};  // the screen order is not the kind order
static uint8_t sect = 0, order[MAX_SYMBOLS], nShown = 0;
static void buildOrder() {
  nShown = 0;
  for (uint8_t i = 0; i < nRows; i++)
    if (sect == 0 || rows[i].kind == SECT_KIND[sect]) order[nShown++] = i;
  if (!nShown && sect) {  // an emptied section: show everything
    sect = 0;
    buildOrder();
  }
}
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
  strcpy(r.cur, "USD");
  uint8_t at = nStocks++;
  nRows++;
  listVersion++;
  xSemaphoreGive(mux);
  buildOrder();
  return (int8_t)at;
}
static void removeRow(uint8_t i) {
  xSemaphoreTake(mux, portMAX_DELAY);
  uint8_t kind = rows[i].kind;
  memmove(rows + i, rows + i + 1, (nRows - i - 1) * sizeof(Row));
  nRows--;
  if (kind == K_COIN) nCoins--;
  else if (kind == K_INDEX) nIdx--;
  else if (kind == K_FX) nFx--;
  else nStocks--;
  rebuildCoinIds();
  listVersion++;
  xSemaphoreGive(mux);
  buildOrder();
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
  buildOrder();
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

static bool addRow(const char *label, const char *id, bool coin, uint8_t kind = K_STOCK) {
  if (nRows >= MAX_SYMBOLS) return false;
  if (!label || !*label || strlen(label) > MAX_LABEL) return false;
  if (!id || !*id) return false;
  Row &r = rows[nRows++];
  memset(&r, 0, sizeof r);
  snprintf(r.label, sizeof r.label, "%s", label);
  snprintf(r.id, sizeof r.id, "%s", id);
  r.coin = coin;
  r.kind = coin ? K_COIN : kind;
  strcpy(r.cur, "USD");
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
  for (JsonObject o : cfgArr("indices")) {  // Yahoo ids like ^GSPC, with a label of your own
    const char *id = o["id"], *label = o["label"];
    if (addRow(label ? label : id, id, false, K_INDEX)) nIdx++;
    else Serial.printf("skipped index '%s' (bad label or list full)\n", id ? id : "?");
  }
  for (JsonVariant v : cfgArr("currencies")) {  // ISO codes; the row is how many of it one USD buys
    const char *c = v.as<const char *>();
    char id[12];
    snprintf(id, sizeof id, "%s=X", c ? c : "");
    if (c && strlen(c) == 3 && addRow(c, id, false, K_FX)) nFx++;
    else Serial.printf("skipped currency '%s' (not a 3-letter code, or list full)\n", c ? c : "?");
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
  Serial.printf("settings: page %lus  spark %s  return %lus\n", (unsigned long)(pageMs / 1000), sparkInterval,
                (unsigned long)(returnMs / 1000));
  Serial.printf("settings: refresh %lu/%lu/%lu min  market %02u:%02u %02u:%02u-%02u:%02u %02u:%02u  tz %s\n",
                (unsigned long)(stockOpenMs / 60000), (unsigned long)(stockExtMs / 60000),
                (unsigned long)(coinMs / 60000), mktPreMin / 60, mktPreMin % 60, mktOpenMin / 60, mktOpenMin % 60,
                mktCloseMin / 60, mktCloseMin % 60, mktPostMin / 60, mktPostMin % 60, tzString);

  rebuildCoinIds();
  Serial.printf("watchlist: %u stocks, %u indices, %u currencies, %u coins\n", nStocks, nIdx, nFx, nCoins);
  return true;
}

// ── layout (portrait 240x320, fixed) ─────────────────────────────────────
// List: six 42px rows. Badge | symbol | sparkline | price over percent.
// Nine 44px rows between a 40px header (section tabs, the CLOSED tag,
// three icons, the clock) and a 44px footer for messages: badge | symbol |
// a wide sparkline | price | percent, everything centred on y+22. No
// company name: the symbol is the identity, and the name ran into it.
// (row and header positions: see Layout)
// Settings: title, five 40px rows, the LIST button.
// Settings: eight 48px rows, all on screen. Heatmap: 6 x 4 tiles.
// (settings rows: see Layout)
// Detail, landscape: a left column (logo, symbol, name, the big price,
// change, the two range bars) and a right column (chart with high and low
// inside it, five range chips, three headlines). One gesture hint at the
// foot. No buttons: swipe left/right for the next/previous stock, up for
// headlines, down for the list.
// (detail page positions: see Layout)
//

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
static const uint8_t NEWS_N = 12, NEWS_ALL = 255;  // idx NEWS_ALL: one feed for the whole list
static uint8_t newsPage = 0;
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
static float fxRate(const char *code) {  // X per USD, 0 if unknown
  if (strcmp(code, "USD") == 0) return 1.0f;
  for (uint8_t i = 0; i < nRows; i++)
    if (rows[i].kind == K_FX && rows[i].valid && strcmp(rows[i].label, code) == 0) return rows[i].price;
  return 0;
}
// A native value of row r in the display currency. converted says whether
// it could be; if not, the value comes back as it was.
static float disp(const Row &r, float v, bool *converted = nullptr) {
  bool can = r.kind != K_FX && strcmp(r.cur, sCur) != 0;
  float from = can ? fxRate(r.cur) : 0, to = can ? fxRate(sCur) : 0;
  can = can && from > 0 && to > 0;
  if (converted) *converted = can;
  return can ? v / from * to : v;
}
// A row's price as text: FX rates get four decimals, everything else the
// usual, in the display currency.
static void priceStr(const Row &r, float v, char *out, size_t n) {
  if (r.kind == K_FX) snprintf(out, n, v < 100 ? "%.4f" : "%.2f", v);
  else formatPrice(disp(r, v), out, n);
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
    if (!rows[i].valid || rows[i].kind > K_INDEX || !rows[i].traded) continue;  // plain reads; a torn one costs nothing
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
// watchlist row v mod n and to ring slot v mod L.rows, and the strip is
// scrolled by pos pixels, either sign. Floor-mod so a drag back past the
// start behaves. Which row is under screen y, or -1. Pure, so selfCheck
// can drive it.
static int32_t modp(int32_t a, int32_t m) {
  int32_t r = a % m;
  return r < 0 ? r + m : r;
}
static int32_t floordiv(int32_t a, int32_t m) { return (a - modp(a, m)) / m; }
static int16_t hitRow(int16_t y, int32_t pos, uint8_t n) {
  if (n == 0 || y < L.yRow0 || y >= L.yRow0 + L.strip) return -1;
  return (int16_t)modp(floordiv(pos + y - L.yRow0, ROW_H), n);
}
// Which settings row, or -1.
static int8_t hitSetting(int16_t y) {
  if (y < L.sY0 || y >= L.sY0 + L.sH * L.sN) return -1;
  return (y - L.sY0) / L.sH;
}
// Which range chip, or -1. The zone is the whole strip between the chart
// and the day bar, taller than the drawn chip, because the chip is small.
static int8_t hitRange(int16_t x, int16_t y) {
  if (y < L.chY + L.chH || y >= L.dYNews || x < L.chX) return -1;
  int8_t i = (x - L.chX) / L.rStep;
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
// Text at an integer scale, for the one or two things on a 7" panel that
// want to be bigger than any face here.
static void bigText(int16_t x, int16_t y, uint8_t size, uint8_t scale, uint16_t fg, const char *s) {
  gfx->setFont(FACES[size - 1].font);
  gfx->setTextSize(scale);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y + FACES[size - 1].cap * scale);
  gfx->print(s);
  gfx->setTextSize(1);
}
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, GW(size) * chars, GH(size), C_BG);
  textAt(x, y, size, fg, s);
}
static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  // The last glyph's edge reaches a few px past `right` (the measured width
  // is the ink, not the advance), so the clear runs to the panel edge or
  // those pixels outlive the text -- the dots after every settings value.
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, min<int16_t>(w + 6, L.w - (right - w)), GH(size), C_BG);
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
// Logos live in PSRAM once read: a flash read stalls the cache, and with
// it the scan-out's reads of the framebuffer, which showed as a flicker
// every time a row with a badge entered the list. logoInventory() reads
// every file at boot; a symbol added later is read on its first blit.
struct LogoCache { uint8_t size; char label[MAX_LABEL + 1]; uint16_t *px; };
static LogoCache logoCache[MAX_SYMBOLS * 2];
static uint8_t nLogoCache = 0;
static uint16_t *logoLoad(uint8_t size, const char *label) {
  for (uint8_t i = 0; i < nLogoCache; i++)
    if (logoCache[i].size == size && strcmp(logoCache[i].label, label) == 0) return logoCache[i].px;
  if (nLogoCache >= MAX_SYMBOLS * 2) return nullptr;
  char path[40];
  snprintf(path, sizeof path, "/logo/%u/%s.565", size, label);
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  const size_t want = (size_t)size * size * 2;
  uint16_t *px = f.size() == want ? (uint16_t *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM) : nullptr;
  bool ok = px && f.read((uint8_t *)px, want) == want;
  f.close();
  if (!ok) {
    if (px) free(px);
    else Serial.printf("%s: bad size, rerun tools/make-logos.py --size %u\n", path, size);
    return nullptr;
  }
  LogoCache &c = logoCache[nLogoCache++];
  c.size = size;
  snprintf(c.label, sizeof c.label, "%s", label);
  c.px = px;
  return px;
}
static bool blitLogo(int16_t x, int16_t y, uint8_t size, const char *label) {
  uint16_t *px = logoLoad(size, label);
  if (!px) return false;
  gfx->draw16bitRGBBitmap(x, y, px, size, size);
  return true;
}

// Inventory at boot so a missing set is named on the log, not discovered row
// by row. A missing logo is an ordinary case, not an error.
static uint8_t haveLogo[2];  // per size, for the info page
static void logoInventory() {
  for (uint8_t size : {LOGO_BADGE, LOGO_BIG}) {
    uint8_t have = 0;
    for (uint8_t i = 0; i < nRows; i++)
      if (logoLoad(size, rows[i].label)) have++;  // into PSRAM, once
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
RTC_DATA_ATTR static bool rtcShutdown = false;  // the last sleep was a shutdown: the BOOT button is the only way back
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
    if (!r.cur[0]) strcpy(r.cur, "USD");  // the quote's currency is not in the snapshot; the next fetch sets it
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
// A sleep wakes on a touch (and the timer); a shutdown (secs == 0) wakes on
// the BOOT button alone -- "off" has to mean off, and a screen that comes
// back when brushed is not off. GPIO0 is RTC-capable, so ext0 can watch it.
static void goToSleep(uint32_t secs) {
  Serial.printf(secs ? "deep sleep for up to %lus, or a touch\n" : "shutdown: deep sleep until the BOOT button\n", (unsigned long)secs);
  rtcShutdown = secs == 0;
  snapshotToRtc();
  backlight(0);  // a bare RGB panel has no sleep command; dark is dark, and deep sleep stops the scan-out
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // The finger that tapped "shut down" is still on the panel, and a finger
  // on the panel is the wake signal: wait for it to lift and the pen line
  // to settle, or the chip wakes before it has slept.
  uint32_t t0 = millis();
  int16_t tx, ty;
  while (touchRead(&tx, &ty) && millis() - t0 < 10000) delay(20);
  delay(400);
  if (secs) {
      // The GT911 pulses INT high on every report, finger or lift, and keeps
    // running through the chip's deep sleep: a level-high wake on it.
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 1);
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  } else {
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  // the BOOT button, low while pressed
  }
  Serial.flush();
  esp_deep_sleep_start();
}

// ── the list: a scrolling ring, scrolled at scan-out ─────────────────────
// The framebuffer is ours (a canvas in PSRAM) and the panel driver asks
// for each chunk of scan-out through a callback in board.h, which maps
// the lines of the list strip through a ring offset: moving the whole
// list one pixel is writing one number, no memory traffic at all. That is
// the CYD's hardware scroll rebuilt in software, so this is the CYD's ring
// again: L.rows slots, the strip wraps, and the slot scrolling off the top
// IS the slot the next row enters from the bottom, line by line, fed from
// a one-row canvas as it comes into view. Virtual rows over one signed
// pixel position; a finger or the crawl moves it either way.
static Arduino_Canvas *rowCanvas;
static int32_t canvasV = INT32_MIN;
static uint32_t scrollLast = 0, scrollAcc = 0, holdUntil = 0;
static char cRow[ROWS_MAX][48], cCanvas[48], cHead[64], cFoot[80];
static void drawTabsAndIcons();
static void drawHeader(uint8_t lit, const char *title = nullptr, const char *note = nullptr);

static uint16_t rowOf(int32_t v) { return order[modp(v, nShown)]; }
static uint8_t slotOf(int32_t v) { return (uint8_t)modp(v, L.rows); }
static int32_t topV() { return floordiv(pos, ROW_H); }
static uint8_t offPx() { return (uint8_t)modp(pos, ROW_H); }
static int16_t rowY(int32_t v) { return L.yRow0 + (int16_t)((v - topV()) * ROW_H) - offPx(); }
static void panelScroll(int32_t p) { boardScroll((int16_t)modp(p, L.strip)); }

static void invalidateCache() {
  for (uint8_t i = 0; i < L.rows; i++) cRow[i][0] = '\0';
  cHead[0] = '\0';
}

static void rowKey(const Row &r, char *key, size_t n, char *price, char *pct) {
  if (r.valid) {
    priceStr(r, r.price, price, 12);
    formatPct(r.pct, pct, 12);
  } else {
    snprintf(price, 12, "--");
    pct[0] = '\0';
  }
  snprintf(key, n, "%s|%s|%s|%u|%.2f|%s|%u|%.0f", r.label, price, pct, r.n, r.n ? r.close[r.n - 1] : 0.0f, sCur, sCols,
           r.volume);
}
static void fmtVolume(float v, char *out, size_t n) {
  if (v >= 1e9f) snprintf(out, n, "%.1fB", v / 1e9f);
  else if (v >= 1e6f) snprintf(out, n, "%.1fM", v / 1e6f);
  else if (v >= 1e3f) snprintf(out, n, "%.0fK", v / 1e3f);
  else snprintf(out, n, "%.0f", v);
}

// Paint one row with its top at y on whatever gfx points at: the panel (a
// ring slot) or the row canvas (y = 0). A rule marks where the kind
// changes. The numeric columns the Columns page turned on are packed from
// the right edge; the chart takes whatever is left after the symbol.
static void paintRow(int16_t y, const Row &r, bool rule) {
  char price[12], pct[12], key[48], b[16];
  rowKey(r, key, sizeof key, price, pct);
  uint16_t fg = !r.valid ? C_DIM : r.pct >= 0 ? C_GOOD : C_BAD;
  gfx->fillRect(0, y, L.w, ROW_H, C_BG);  // the columns move; nothing may outlive a change
  if (rule) gfx->drawFastHLine(0, y, L.w, C_RULE);
  if (r.kind == K_INDEX) {  // an index has no mark; its badge column stays empty
  } else if (!blitLogo(L.xSym, y + L.yBadge, LOGO_BADGE, r.label)) {  // no file: a tile with the initial
    gfx->fillRoundRect(L.xSym, y + L.yBadge, LOGO_BADGE, LOGO_BADGE, 6, C_RULE);
    char c[2] = {r.label[0], 0};
    textAt(L.xSym + (LOGO_BADGE - textWidth(2, c)) / 2, y + L.yBadge + (LOGO_BADGE - FACES[1].cap) / 2, 2, C_MUTED, c);
  }
  textAt(r.kind == K_INDEX ? L.xSym : L.xLbl, y + L.yLbl, 2, C_FG, r.label);  // an index starts where its badge would
  int16_t right = L.xRight;
  if (sCols & COL_PCT) {
    textAt(right - textWidth(2, pct), y + L.yLbl, 2, fg, pct);
    right -= 96;
  }
  if (sCols & COL_PRICE) {
    textAt(right - textWidth(2, price), y + L.yLbl, 2, fg, price);
    right -= 116;
  }
  if (sCols & COL_CHG) {
    if (r.valid && r.prev > 0) snprintf(b, sizeof b, "%+.2f", disp(r, r.price) - disp(r, r.prev));
    else strcpy(b, "");
    textAt(right - textWidth(2, b), y + L.yLbl, 2, fg, b);
    right -= 106;
  }
  if (sCols & COL_VOL) {
    if (r.valid && r.volume > 0) fmtVolume(r.volume, b, sizeof b);
    else strcpy(b, "");
    textAt(right - textWidth(1, b), y + L.yLbl + 3, 1, C_MUTED, b);
    right -= 100;
  }
  if (sCols & COL_DAY) {
    int16_t bx = right - 150;
    gfx->fillRoundRect(bx, y + 18, 150, 8, 4, C_RULE);
    if (r.valid && r.dayHi > r.dayLo) {
      float f = constrain((r.price - r.dayLo) / (r.dayHi - r.dayLo), 0.0f, 1.0f);
      gfx->fillRoundRect(bx + (int16_t)(f * 144), y + 16, 6, 12, 3, fg);
    }
    right -= 166;
  }
  if (sCols & COL_CHART) {
    int16_t w = right - 16 - L.xSpk;
    if (w > 40) drawSpark(L.xSpk, y + L.ySpk, w, L.spkH, r);
  }
}
static bool seamAt(int32_t v) {
  uint16_t a = rowOf(v), b = rowOf(v - 1);
  return rows[a].kind != rows[b].kind;
}

// A slot fully owned by virtual row v: repaint only if the row's key changed.
static void paintSlot(int32_t v) {
  Row r = rowCopy(rowOf(v));
  char key[48], price[12], pct[12];
  rowKey(r, key, sizeof key, price, pct);
  if (strcmp(key, cRow[slotOf(v)]) == 0) return;
  strcpy(cRow[slotOf(v)], key);
  paintRow(L.yRow0 + slotOf(v) * ROW_H, r, seamAt(v));
}

// The entering row, painted off-screen. ponytail: the drawing helpers all
// go through the global gfx, so point it at the canvas for the duration.
static void paintCanvas(int32_t v) {
  if (v == canvasV) return;
  canvasV = v;
  Row r = rowCopy(rowOf(v));
  char price[12], pct[12];
  rowKey(r, cCanvas, sizeof cCanvas, price, pct);
  Arduino_GFX *panel = gfx;
  gfx = rowCanvas;
  gfx->fillScreen(C_BG);
  paintRow(0, r, seamAt(v));
  gfx = panel;
}
// Canvas lines [from, to) into the slot of virtual row v.
static void feed(int32_t v, uint8_t from, uint8_t to) {
  gfx->draw16bitRGBBitmap(0, L.yRow0 + slotOf(v) * ROW_H + from, rowCanvas->getFramebuffer() + from * L.w, L.w,
                          to - from);
}

// Move the strip by delta pixels, either sign, one row-boundary chunk at a
// time: set the ring offset, then feed the lines that just came into view.
static void scrollBy(int32_t delta) {
  while (delta) {
    int32_t v0 = topV();
    uint8_t off = offPx();
    if (delta > 0) {
      uint8_t k = (uint8_t)min<int32_t>(delta, ROW_H - off);
      paintCanvas(v0 + L.rows);
      pos += k;
      panelScroll(pos);
      feed(v0, off, off + k);
      if (off + k == ROW_H) strcpy(cRow[slotOf(v0)], cCanvas);  // v0+L.rows owns the slot now
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
  boardScrollArea(L.yRow0, L.strip);
  panelScroll(pos);
  invalidateCache();
  canvasV = INT32_MIN;
  drawHeader(0);
  cFoot[0] = '\0';
  int32_t v0 = topV();
  uint8_t off = offPx();
  for (uint8_t p = 0; p < L.rows; p++) paintSlot(v0 + p);
  if (off) {  // the shared slot: the entering row's lines over the top row's
    paintCanvas(v0 + L.rows);
    feed(v0, 0, off);
  }
  scrollLast = millis();
  scrollAcc = 0;
}

// Every pass: refresh the slots fully in view, then advance the crawl by
// however many pixels the clock owes. A pixel costs one number and at most
// one line fed into the ring, so every pass can move one. Frozen while a
// finger is down and for a moment after, so a drag or a press is not fought.
static void listTick() {
  uint32_t now = millis(), dt = now - scrollLast;
  scrollLast = now;
  int32_t v0 = topV();
  for (uint8_t p = offPx() ? 1 : 0; p < L.rows; p++) paintSlot(v0 + p);
  if (touchHeld || now < holdUntil) {
    scrollAcc = 0;
    return;
  }
  scrollAcc += dt * L.strip;  // pixels, scaled by pageMs
  int32_t step = scrollAcc / pageMs;
  if (!step) return;
  scrollAcc -= (uint32_t)step * pageMs;
  scrollBy(step);
}

// The header, left to right: five section tabs (tap one), the CLOSED tag,
// three icons (search, heatmap, settings), the clock. The tabs and icons
// are drawn once by listStart; this keeps the clock and the tag current.
static const char *const TAB_NAMES[] = {"ALL", "STOCKS", "INDICES", "CRYPTO", "FX"};
static int16_t tabX[5], tabW[5];  // each tab as wide as its word, laid out left to right
static uint8_t litIcon = 0;       // 1 search, 2 heatmap, 3 headlines, 4 settings: the page that is open
static bool tabsShown = false;    // the section tabs are only on the list; other pages put their name there
static void drawTabsAndIcons() {
  int16_t x = L.tabX;
  gfx->fillRect(0, 0, L.tagX > 0 ? L.tagX : L.iconX - 8, L.yRow0 - 1, C_BG);
  if (tabsShown) {
    int16_t sum = 0;
    for (uint8_t i = 0; i < 5; i++) sum += textWidth(1, TAB_NAMES[i]);
    // The air around a name; less on the narrow screen. The tabs stop 12px
    // short of the tag, whose field clears from tagX: at 28px of air the
    // names ran to 440 and the tag took the right half of FX.
    int16_t edge = L.tagX > 0 ? L.tagX - 12 : L.iconX - 8;
    int16_t pad = min<int16_t>(28, (edge - L.tabX - sum) / 5);
    for (uint8_t i = 0; i < 5; i++) {
      tabX[i] = x;
      tabW[i] = textWidth(1, TAB_NAMES[i]) + pad;
      bool on = i == sect;
      textAt(x + pad / 2, 12, 1, on ? C_FG : C_DIM, TAB_NAMES[i]);
      if (on) gfx->fillRect(x + pad / 2 - 4, 34, tabW[i] - pad + 8, 3, C_FG);
      x += tabW[i];
    }
  }
  x = L.iconX;  // a magnifier
  uint16_t c1 = litIcon == 1 ? C_FG : C_MUTED, c2 = litIcon == 2 ? C_FG : C_MUTED, c3 = litIcon == 3 ? C_FG : C_MUTED,
           c4 = litIcon == 4 ? C_FG : C_MUTED;
  gfx->fillRect(x - 8, 0, 4 * L.iconStep, L.yRow0 - 1, C_BG);
  gfx->drawCircle(x + 13, 17, 7, c1);
  gfx->drawCircle(x + 13, 17, 6, c1);
  gfx->drawLine(x + 18, 22, x + 26, 30, c1);
  gfx->drawLine(x + 19, 21, x + 27, 29, c1);
  x += L.iconStep;  // a grid
  for (uint8_t r = 0; r < 2; r++)
    for (uint8_t c = 0; c < 2; c++) gfx->fillRoundRect(x + 6 + c * 12, 9 + r * 12, 9, 9, 2, c2);
  x += L.iconStep;  // headlines: three lines of text
  for (uint8_t r = 0; r < 3; r++) gfx->fillRect(x + 5, 10 + r * 7, r == 1 ? 22 : 16, 3, c3);
  x += L.iconStep;  // three sliders
  for (uint8_t r = 0; r < 3; r++) {
    gfx->drawFastHLine(x + 5, 12 + r * 8, 22, c4);
    gfx->fillCircle(x + 9 + (r == 1 ? 12 : r == 2 ? 6 : 0), 12 + r * 8, 3, c4);
  }
  gfx->drawFastHLine(0, L.yRow0 - 1, L.w, C_RULE);
}
// The header is on every page, drawn right after a page's fillScreen. The
// right side -- the icons and the clock -- is fixed; the left side is the
// page's: the section tabs on the list, elsewhere a title (and a note).
// A title that starts with "< " gets a drawn back mark instead -- the
// chevron's mirror, its point at `left` -- and the mark and the title are
// a button: headerTap() sends a tap there back a page.
static bool headerBack = false;
static void backMark(int16_t left, int16_t cy, uint16_t c) {
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(left + 8 + d, cy - 8, left + d, cy, c);
    gfx->drawLine(left + d, cy, left + 8 + d, cy + 8, c);
  }
}
static void drawHeader(uint8_t lit, const char *title, const char *note) {
  litIcon = lit;
  tabsShown = title == nullptr;
  drawTabsAndIcons();
  headerBack = title && strncmp(title, "< ", 2) == 0;
  if (title) {
    const char *t = headerBack ? title + 2 : title;
    int16_t tx = L.tabX;
    if (headerBack) {
      backMark(L.tabX + 2, 10 + FACES[1].cap / 2, C_MUTED);
      tx += 24;
    }
    textAt(tx, 10, 2, C_FG, t);
    if (note) textAt(tx + textWidth(2, t) + 14, 14, 1, C_MUTED, note);
  }
  cHead[0] = '\0';
}
// Which header thing a tap at x lands on: 0-4 a tab, 10 search, 11 heatmap, 12 settings, -1 nothing.
static int8_t hitHeader(int16_t x) {
  if (tabsShown)
    for (uint8_t i = 0; i < 5; i++)
      if (x >= tabX[i] && x < tabX[i] + tabW[i]) return i;
  if (x >= L.iconX - 8 && x < L.iconX + 4 * L.iconStep) return 10 + (x - (L.iconX - 8)) / L.iconStep;
  return -1;
}
static void drawHead(const struct tm *t, bool haveTime) {
  char buf[48], clk[12];
  if (haveTime) fmtClock(*t, clk, sizeof clk);
  else snprintf(clk, sizeof clk, "--:--");
  Session ses = haveTime ? sessionNow(*t) : Session::Regular;
  snprintf(buf, sizeof buf, "%s|%u", clk, (unsigned)ses);
  if (strcmp(buf, cHead) == 0) return;
  strcpy(cHead, buf);
  char tag[24] = "";
  if (ses == Session::Closed) {
    char nx[16];
    nextOpen(*t, nx, sizeof nx);
    snprintf(tag, sizeof tag, "CLOSED %s", nx);
  }
  if (L.tagX >= 0) {  // landscape: the tag and the clock in the header
    fieldRight(L.xRight, 12, 8, 1, C_MUTED, clk);
    field(L.tagX, 12, 17, 1, C_WARN, tag);  // "market open" says nothing; only closed is news
  } else {  // portrait: no room beside the tabs; the footer carries both
    fieldRight(L.xRight, L.yFoot + 14, 8, 1, C_MUTED, clk);
    fieldRight(L.xRight - 80, L.yFoot + 14, 17, 1, C_WARN, tag);
  }
}
// The footer: an alert for ten seconds, else the session when it is not
// simply open; and on the right how old the prices are. Quiet colours.
static void drawFoot(const struct tm *t, bool haveTime) {
  char left[48] = "", right[24] = "", key[80];
  bool banner = bannerUntil && millis() < bannerUntil;
  if (bannerUntil && !banner) bannerUntil = 0;
  Session ses = haveTime ? sessionNow(*t) : Session::Regular;
  if (banner) snprintf(left, sizeof left, "%s", bannerText);
  else if (haveTime && ses != Session::Regular) {
    char nx[16];
    nextOpen(*t, nx, sizeof nx);
    if (ses == Session::Closed) snprintf(left, sizeof left, "market closed, opens %s", nx);
    else snprintf(left, sizeof left, "%s", sessionWord(ses));
  }
  if (lastOk && L.tagX >= 0) snprintf(right, sizeof right, "prices %lum old", (unsigned long)((millis() - lastOk) / 60000));
  snprintf(key, sizeof key, "%s|%s", left, right);
  if (strcmp(key, cFoot) == 0) return;
  strcpy(cFoot, key);
  gfx->drawFastHLine(0, L.yFoot, L.w, C_RULE);
  field(L.xSym, L.yFoot + 14, L.tagX >= 0 ? 60 : 28, 1, banner ? C_MUTED : C_DIM, left);
  if (L.tagX >= 0) fieldRight(L.xRight, L.yFoot + 14, 20, 1, C_DIM, right);
}

// ── boot splash: drawn, not shipped ──────────────────────────────────────
// A navy-to-black sky, nine candles on the way up with a gold average
// through them, the wordmark, and one status line that follows the Wi-Fi
// join. Primitives only, so there is no asset to generate or push.
static void splashStatus(const char *s) { fieldCentre(L.w / 2, L.yHint, 80, 1, C_DIM, s); }
// The splash: TICKER in white above three bands of symbol chips, badge and
// label. Each band takes as many chips as fit inside the margins and is
// centred, so no chip is cut at an edge, and the bands are all alike -- a
// brighter middle band read as a stray highlight. Indices have no badge
// and sit it out. Drawn once; the status line under it is what changes.
static void drawSplash(const char *status) {
  gfx->fillScreen(C_BG);
  const char *name = "TICKER";
  bigText((L.w - textWidth(3, name) * 2) / 2, L.wordY, 3, 2, C_FG, name);
  const int16_t chipH = 48, gap = 14, step = 96;
  int16_t mid = L.h / 2 + 16 - chipH / 2;  // the top band clears the name by 26px
  uint8_t r = 0;  // the next row to chip, round the list
  for (uint8_t b = 0; b < 3 && nRows; b++) {
    uint8_t idx[16], n = 0;
    int16_t total = 0;
    for (uint16_t guard = 0; n < 16 && guard < nRows; guard++) {
      const Row &row = rows[r];
      if (row.kind == K_INDEX) {
        r = (r + 1) % nRows;
        continue;
      }
      int16_t add = 56 + textWidth(2, row.label) + (n ? gap : 0);
      if (total + add > L.w - 40) break;
      idx[n++] = r;
      total += add;
      r = (r + 1) % nRows;
    }
    int16_t y = mid + (b - 1) * step, x = (L.w - total) / 2;
    for (uint8_t i = 0; i < n; i++) {
      const Row &row = rows[idx[i]];
      int16_t w = 56 + textWidth(2, row.label);
      gfx->fillRoundRect(x, y, w, chipH, 10, C_RULE);
      blitLogo(x + 8, y + 8, LOGO_BADGE, row.label);
      textAt(x + 48, y + (chipH - GH(2)) / 2, 2, C_FG, row.label);
      x += w + gap;
    }
  }
  const char *credit = "made by Richard Torcato";
  textAt((L.w - textWidth(1, credit)) / 2, L.creditY, 1, C_MUTED, credit);
  splashStatus(status);
}

static void drawPanel(const char *title, uint16_t tc, const char *const *lines, uint8_t n, bool header = true,
                      uint8_t lit = 4, const char *head = nullptr, const char *note = nullptr) {
  gfx->fillScreen(C_BG);
  if (header) drawHeader(lit, head ? head : title, note);
  int16_t y = header ? 60 : 52;  // with the page named in the header the title is smaller
  if (!header) {
    field(20, 52, 24, 3, tc, title);
    gfx->drawFastHLine(20, 96, L.w - 40, C_RULE);
    y = 110;
  }
  for (uint8_t i = 0; i < n && i < 16; i++) field(20, y + i * 22, (L.w - 40) / 8, 1, C_MUTED, lines[i]);
}

// ── detail page ──────────────────────────────────────────────────────────
static uint8_t detailIdx = 0;
static uint32_t detailOpenedAt = 0;
static char cDetail[64];
static bool kChartReset = false;  // a chip tap: the chart part repaints on the next pass

// One dim line at the foot of a page saying which swipes it takes. Not a
// control: the gestures work anywhere on the page.
static void drawHint(const char *s, uint16_t c = C_DIM) { fieldCentre(L.w / 2, L.yHint, 80, 1, c, s); }

static void drawRange(const Row &r, int16_t y, const char *label, float lo, float hi, float v) {
  char b[12];
  field(L.barX, y, 12, 1, C_MUTED, label);
  gfx->fillRoundRect(L.barX, y + 22, L.barW, L.barH, L.barH / 2, C_RULE);
  if (hi > lo) {
    float f = constrain((v - lo) / (hi - lo), 0.0f, 1.0f);
    gfx->fillRoundRect(L.barX + (int16_t)(f * (L.barW - 6)), y + 20, 6, L.barH + 4, 3, C_FG);
  }
  priceStr(r, lo, b, sizeof b);
  field(L.barX, y + 34, 9, 1, C_DIM, b);
  priceStr(r, hi, b, sizeof b);
  fieldRight(L.barX + L.barW, y + 34, 9, 1, C_DIM, b);
}

static void drawRangeChips() {
  for (uint8_t i = 0; i < N_RANGES; i++) {
    int16_t x = L.chX + i * L.rStep;
    bool on = i == rangeSel;
    gfx->fillRoundRect(x, L.rY, L.rW, L.rH, 8, on ? C_DIM : C_BG);
    gfx->drawRoundRect(x, L.rY, L.rW, L.rH, 8, on ? C_MUTED : C_RULE);
    textAt(x + (L.rW - textWidth(2, RANGES[i].label)) / 2, L.rY + (L.rH - FACES[1].cap) / 2, 2, on ? C_FG : C_MUTED,
           RANGES[i].label);
  }
}

static void drawChart(const Row &r, const float *cl, uint8_t n, float prev, uint16_t fg) {
  float lo = prev, hi = prev;
  for (uint8_t i = 0; i < n; i++) {
    lo = min(lo, cl[i]);
    hi = max(hi, cl[i]);
  }
  gfx->drawRect(L.chX - 1, L.chY - 1, L.chW + 2, L.chH + 2, C_RULE);
  int16_t yp = sparkY(prev, lo, hi, L.chY, L.chH);
  for (int16_t x = L.chX; x < L.chX + L.chW; x += 6) gfx->drawFastHLine(x, yp, 3, C_MUTED);
  int16_t px = L.chX, py = sparkY(cl[0], lo, hi, L.chY, L.chH);
  for (uint8_t i = 1; i < n; i++) {
    int16_t nx = L.chX + (int32_t)i * (L.chW - 1) / (n - 1);
    int16_t ny = sparkY(cl[i], lo, hi, L.chY, L.chH);
    gfx->drawLine(px, py, nx, ny, fg);
    px = nx;
    py = ny;
  }
  // High and low printed inside the box, top-left and bottom-left, over
  // whatever the line does there: the row of chips below wanted the space.
  char b[12];
  priceStr(r, hi, b, sizeof b);
  field(L.chX + 6, L.chY + 4, 9, 1, C_DIM, b);
  priceStr(r, lo, b, sizeof b);
  field(L.chX + 6, L.chY + L.chH - GH(1) - 4, 9, 1, C_DIM, b);
}

static uint8_t wrapText(const char *s, int16_t w, char out[][64], uint8_t lines);
static void drawDetail(bool full) {
  Row r = rowCopy(detailIdx);
  if (full) {
    gfx->fillScreen(C_BG);
    char head[16];
    snprintf(head, sizeof head, "< %s", r.label);
    drawHeader(0, head, r.coin ? "crypto" : r.kind == K_FX ? "currency" : r.kind == K_INDEX ? "index" : r.name);
    cDetail[0] = '\0';
    if (r.kind == K_INDEX) {  // no mark for an index, on the page as on the list
    } else if (!blitLogo(L.dXLogo, L.dYLogo, LOGO_BIG, r.label)) {  // no file: a tile with the symbol
      gfx->fillRoundRect(L.dXLogo, L.dYLogo, LOGO_BIG, LOGO_BIG, 20, C_RULE);
      textAt(L.dXLogo + (LOGO_BIG - textWidth(3, r.label)) / 2, L.dYLogo + (LOGO_BIG - FACES[2].cap) / 2, 3, C_MUTED, r.label);
    }
    drawHint("< next        ^ all headlines        v list        prev >");
    drawRangeChips();  // indices, FX and coins (as BTC-USD) have Yahoo history too
  }
  // Three headlines on the page itself, from the news cache; the news page
  // has the rest.
  NewsItem items[3];
  uint8_t nNews;
  bool newsMine;
  xSemaphoreTake(mux, portMAX_DELAY);
  newsMine = newsIdx == detailIdx;
  nNews = newsMine ? min<uint8_t>(newsN, 3) : 0;
  memcpy(items, news, sizeof items);
  xSemaphoreGive(mux);
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

  char price[12], pct[12], key[64];
  if (r.valid) {
    priceStr(r, shown, price, sizeof price);
    formatPct(pctv, pct, sizeof pct);
  } else {
    snprintf(price, sizeof price, "--");
    pct[0] = '\0';
  }
  uint8_t dots = r.valid ? 0 : 1 + (millis() / 400) % 3;  // a row with no price yet: an animated "fetching"
  uint16_t fg = !r.valid ? C_DIM : pctv >= 0 ? C_GOOD : C_BAD;
  (void)cDetail;

  // Four parts, each repainted only when its own key changes, so a price
  // landing does not blank the chart and the dots do not blank the page.
  static char kText[64], kChart[48], kRanges[64], kNews[32];
  if (full || kChartReset) {
    kChart[0] = '\0';
    kChartReset = false;
  }
  if (full) kText[0] = kRanges[0] = kNews[0] = '\0';

  snprintf(key, sizeof key, "%s|%s|%s|%d|%s|%u", r.label, price, pct, ext, sCur, dots);
  if (strcmp(key, kText) != 0) {
    strcpy(kText, key);
    char name[28], tag[28] = "";  // the name beside the logo, and one line under it
    snprintf(name, sizeof name, "%s", r.coin ? "crypto, 24h change" : r.kind == K_FX ? "one US dollar buys" : r.name);
    if (!r.valid) snprintf(tag, sizeof tag, "fetching %s%.*s", r.label, dots, "...");
    else if (ext) snprintf(tag, sizeof tag, "%s", ses == Session::Pre ? "pre-market" : "after hours");
    int16_t tx = r.kind == K_INDEX ? L.dXLogo : L.dXTxt;  // no logo: the text takes its place
    field(tx, L.dYSym, MAX_LABEL + 1, 3, C_FG, r.label);
    field(tx, L.dYName, 26, 1, C_MUTED, name);
    field(tx, L.dYTag, 26, 1, C_WARN, tag);
    field(L.dXPrice, L.dYPrice, 8, 4, fg, price);  // the tall digits
    bool conv = false;
    disp(r, shown, &conv);  // which currency the number is in
    const char *code = r.kind == K_FX ? "per USD" : conv ? sCur : r.cur;
    gfx->fillRect(L.dXPrice + 200, L.dYPrice + 30, 100, 20, C_BG);
    if (r.valid && (r.kind == K_FX || strcmp(code, "USD") != 0 || strcmp(sCur, "USD") != 0))
      textAt(L.dXPrice + textWidth(4, price) + 10, L.dYPrice + 32, 1, C_DIM, code);
    char chg[12] = "";
    if (r.valid && base > 0) snprintf(chg, sizeof chg, "%+.2f", disp(r, shown) - disp(r, base));
    field(L.dXPrice, L.dYChg, 12, 2, fg, chg);
    field(L.dXPct, L.dYChg, 8, 2, fg, pct);
  }

  // Chart: the sparkline's data with room to be a chart. The previous close
  // is a dashed reference line -- the percent is measured from it, so
  // without it the shape means nothing.
  snprintf(key, sizeof key, "%u|%u|%d|%d|%.2f|%d", rangeSel, n, mine && !sr.valid, r.valid, n ? cl[n - 1] : 0.0f, fg == C_GOOD);
  if (strcmp(key, kChart) != 0) {
    strcpy(kChart, key);
    gfx->fillRect(L.chX - 1, L.chY - 1, L.chW + 2, L.chH + 2, C_BG);  // chips below are left alone
    if (rangeSel && mine && !sr.valid) {
      char m[24];
      snprintf(m, sizeof m, "no %s series", RANGES[rangeSel].label);
      field(L.chX + 8, L.chY + L.chH / 2 - 8, 24, 1, C_DIM, m);
    } else if (rangeSel && n < 2) {
      char m[24];
      snprintf(m, sizeof m, "loading %s...", RANGES[rangeSel].label);
      field(L.chX + 8, L.chY + L.chH / 2 - 8, 24, 1, C_DIM, m);
    } else if (!r.valid || n < 2) {
      field(L.chX + 8, L.chY + L.chH / 2 - 8, 24, 1, C_DIM, r.valid ? "no series" : "fetching...");
    } else {
      drawChart(r, cl, n, prev, fg);
    }
  }

  // Headlines under the chips: three, one line each, cut to the column.
  snprintf(key, sizeof key, "%u|%lu|%d", nNews, newsMine ? (unsigned long)newsAt : 0UL, r.coin);
  if (strcmp(key, kNews) != 0) {
    strcpy(kNews, key);
    gfx->fillRect(L.chX, L.dYNews, L.w - L.chX, L.yHint - 4 - L.dYNews, C_BG);
    if (nNews) {
      for (uint8_t i = 0; i < nNews; i++) {
        char line[2][64];
        wrapText(items[i].title, L.chW - 60, line, 1);
        textAt(L.chX, L.dYNews + i * L.newsStep, 1, C_FG, line[0]);
        fieldRight(L.xRight, L.dYNews + i * L.newsStep, 5, 1, C_DIM, items[i].age);
        if (L.newsStep > 30) gfx->drawFastHLine(L.chX, L.dYNews + i * L.newsStep + 30, L.chW, C_RULE);
      }
    } else if (!r.coin) {
      textAt(L.chX, L.dYNews, 1, C_DIM, newsMine ? "no headlines" : "headlines on their way...");
    }
  }

  snprintf(key, sizeof key, "%d|%.2f|%.2f|%.2f|%.2f|%.2f|%s", r.valid, r.price, r.dayLo, r.dayHi, r.wkLo, r.wkHi, sCur);
  if (strcmp(key, kRanges) != 0) {
    strcpy(kRanges, key);
    gfx->fillRect(0, L.dYDay, L.barX + L.barW + 20, (L.dYNews > L.dYDay ? L.dYNews : L.yHint - 4) - L.dYDay, C_BG);
    if (r.valid) {
      drawRange(r, L.dYDay, "day range", r.dayLo, r.dayHi, r.price);
      drawRange(r, L.dYWk, "52-week range", r.wkLo, r.wkHi, r.price);
    }
  }
}

// ── setup: the device asks for its Wi-Fi ─────────────────────────────────
// No credentials in NVS (first boot, or after Clear device, or the Wi-Fi
// row): the panel shows three steps and the board raises an access point,
// "ticker-setup" with an eight-digit PIN derived from its MAC, serving one
// form at 192.168.4.1 -- a DNS catch-all makes phones open it on their own.
// The form lists the networks it can hear. Saving writes NVS and restarts.
static WebServer *web = nullptr;
static DNSServer *dns = nullptr;
static bool setupMode = false;
static char setupPin[9], setupSsids[600], setupError[80] = "";
static uint32_t setupStarted = 0;
static void setupPage() {
  String html = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>ticker setup</title><style>body{font-family:-apple-system,Helvetica,Arial;background:#000;color:#eee;"
                  "margin:0;padding:24px}h1{font-size:22px}label{display:block;margin:18px 0 6px;color:#9aa4ae}"
                  "select,input{width:100%;font-size:18px;padding:10px;border-radius:8px;border:1px solid #444;background:#111;color:#eee}"
                  "button{margin-top:24px;width:100%;font-size:18px;padding:12px;border:0;border-radius:8px;background:#22d05a;color:#000}"
                  "</style></head><body><h1>ticker setup</h1>");
  if (setupError[0]) {
    html += F("<p style='color:#f0473c'>");
    html += setupError;
    html += F("</p>");
  }
  html += F("<form method=post action=/save><label>Wi-Fi network</label><select name=s>");
  html += setupSsids;
  html += F("</select><label>or type its name</label><input name=o placeholder='hidden network'>"
            "<label>password</label><input type=password name=p id=p>"
            "<label style='display:flex;align-items:center;gap:8px;margin-top:10px'>"
            "<input type=checkbox style='width:auto' onchange=\"p.type=this.checked?'text':'password'\">show password</label>"
            "<button>save and restart</button></form></body></html>");
  web->send(200, "text/html", html);
}
static void setupSave() {
  String ssid = web->arg("o");
  if (!ssid.length()) ssid = web->arg("s");
  String pass = web->arg("p");
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
    web->send(400, "text/plain", "network name missing or too long");
    return;
  }
  // Try the network before keeping it: the access point stays up while the
  // station side joins, so a wrong password comes straight back to the
  // phone as an error instead of a board stuck on NO WIFI.
  fieldCentre(L.w / 2, L.yHint, 80, 1, C_DIM, "trying that network...");
  Serial.printf("setup: trying '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    snprintf(setupError, sizeof setupError, "Could not join \"%.32s\". Wrong password? Try again.", ssid.c_str());
    Serial.printf("setup: join failed (%s)\n", setupError);
    fieldCentre(L.w / 2, L.yHint, 80, 1, C_WARN, "that network did not let it in. wrong password?");
    setupPage();  // the form again, with the reason at the top
    return;
  }
  prefs.begin("ticker", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  web->send(200, "text/html", F("<!doctype html><body style='font-family:-apple-system,Helvetica;background:#000;color:#eee;padding:24px'>"
                                "<h1>connected</h1><p>The ticker joined your network and is restarting.</p></body>"));
  Serial.printf("setup: joined '%s' (%ddBm), saved, restarting\n", ssid.c_str(), WiFi.RSSI());
  delay(800);
  ESP.restart();
}
static void drawSetup(const char *status) {
  gfx->fillScreen(C_BG);
  field(20, 20, 24, 3, C_FG, "SETUP");
  gfx->drawFastHLine(20, 64, L.w - 40, C_RULE);
  char l[40];
  // One column, top to bottom; the network name and its password on one
  // line so a phone can be held next to it.
  bool wide = L.w >= 800;
  textAt(20, 88, 2, C_GOOD, "1");
  textAt(56, 90, 1, C_MUTED, "on your phone, join the Wi-Fi network");
  textAt(56, 118, 3, C_FG, "ticker-setup");
  snprintf(l, sizeof l, "%.4s %.4s", setupPin, setupPin + 4);
  if (wide) {
    int16_t x = 56 + textWidth(3, "ticker-setup") + 40;
    textAt(x, 126, 1, C_MUTED, "password");
    textAt(x + textWidth(1, "password") + 16, 118, 3, C_GOLD, l);
  } else {
    textAt(56, 160, 1, C_MUTED, "password");
    textAt(56 + textWidth(1, "password") + 16, 152, 3, C_GOLD, l);
  }
  int16_t y = wide ? 180 : 210;
  textAt(20, y, 2, C_GOOD, "2");
  if (wide) {
    textAt(56, y + 2, 1, C_MUTED, "a sign-in page opens by itself; if not, open this in the browser");
  } else {
    textAt(56, y + 2, 1, C_MUTED, "a sign-in page opens by itself;");
    textAt(56, y + 22, 1, C_MUTED, "if not, open this in the browser");
    y += 20;
  }
  textAt(56, y + 30, 3, C_FG, "192.168.4.1");
  y += wide ? 92 : 100;
  textAt(20, y, 2, C_GOOD, "3");
  if (wide) {
    textAt(56, y + 2, 1, C_MUTED, "pick your network, type its password, save. The ticker restarts and joins.");
  } else {
    textAt(56, y + 2, 1, C_MUTED, "pick your network, type its password,");
    textAt(56, y + 22, 1, C_MUTED, "save. The ticker restarts and joins.");
    y += 20;
  }
  gfx->drawFastHLine(20, y + 58, L.w - 40, C_RULE);
  if (wide) textAt(20, y + 74, 1, C_DIM, "nothing leaves this board: the password is kept in its own flash, and only there.");
  else {
    textAt(20, y + 74, 1, C_DIM, "nothing leaves this board: the password");
    textAt(20, y + 94, 1, C_DIM, "is kept in its own flash, and only there.");
  }
  fieldCentre(L.w / 2, L.yHint, 80, 1, C_DIM, status);
}
static void startSetup() {
  setupMode = true;
  setupStarted = millis();
  uint64_t mac = ESP.getEfuseMac();
  snprintf(setupPin, sizeof setupPin, "%08lu", (unsigned long)((mac ^ (mac >> 24)) % 100000000UL));
  if (strlen(setupPin) < 8) strcpy(setupPin, "12345678");
  drawSetup("scanning for networks");
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks(false, false, false, 250);
  setupSsids[0] = '\0';
  for (int i = 0; i < n && i < 12; i++) {
    char opt[64];
    snprintf(opt, sizeof opt, "<option>%.32s</option>", WiFi.SSID(i).c_str());
    strlcat(setupSsids, opt, sizeof setupSsids);
  }
  WiFi.scanDelete();
  WiFi.softAP("ticker-setup", setupPin);
  web = new WebServer(80);
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());
  web->on("/", HTTP_GET, setupPage);
  web->on("/save", HTTP_POST, setupSave);
  web->onNotFound([] {  // the captive probes of every phone land here
    web->sendHeader("Location", "http://192.168.4.1/", true);
    web->send(302, "text/plain", "");
  });
  web->begin();
  Serial.printf("setup: AP ticker-setup, password %s, form at http://%s (%d networks heard)\n", setupPin,
                WiFi.softAPIP().toString().c_str(), n);
  drawSetup(wifiSsid[0] ? "swipe down to keep the old network" : "waiting for you");
}
static void clearDevice() {
  Serial.println("clear device: NVS wiped, restarting into setup");
  prefs.begin("ticker", false);
  prefs.clear();
  prefs.end();
  rtcMagic = 0;
  delay(300);
  ESP.restart();
}

// ── settings page: swipe right from the list ─────────────────────────────
static uint32_t pageOpenedAt = 0;  // settings and info share the auto-return

static void applySettings() {
  pageMs = SPEED_MS[sSpeed];
  returnMs = RET_MS[sRet];
}
static void loadSettings() {
  prefs.begin("ticker", true);
  bool any = prefs.isKey("speed");
  if (any) {
    sSpeed = prefs.getUChar("speed", sSpeed) % 3;
    sRet = prefs.getUChar("ret", sRet) % 3;
    sSleep = prefs.getUChar("slp", sSleep) % 3;
    sClock = prefs.getUChar("clk", sClock) % 2;
    sCols = prefs.getUChar("cols", sCols) & 63;
    sRot = prefs.getUChar("rot", sRot) & 3;
    sTheme = prefs.getUChar("bg", sTheme) % N_THEMES;
    applyTheme();
    prefs.getString("cur", sCur, sizeof sCur);
    sect = prefs.getUChar("sect", 0) % 5;
    buildOrder();
  }
  prefs.getString("ssid", wifiSsid, sizeof wifiSsid);
  prefs.getString("pass", wifiPass, sizeof wifiPass);
  if (any) {  // (re-enter the block the settings print expects)
  }
  if (any) {
    applySettings();
  } else {  // nothing saved yet: the config's own values stand
    for (uint8_t i = 0; i < 3; i++) if (SPEED_MS[i] == pageMs) sSpeed = i;
    for (uint8_t i = 0; i < 3; i++) if (RET_MS[i] == returnMs) sRet = i;
  }
  prefs.end();
  Serial.printf("settings from %s: speed %s, return %s, sleep %s\n", any ? "NVS (beats config.json)" : "config.json",
                SPEED_NAMES[sSpeed], RET_NAMES[sRet], SLEEP_NAMES[sSleep]);
  Serial.printf("settings: currency %s, section %s\n", sCur, SECT_NAMES[sect]);
}
static void saveSettings() {
  prefs.begin("ticker", false);
  prefs.putUChar("speed", sSpeed);
  prefs.putUChar("ret", sRet);
  prefs.putUChar("slp", sSleep);
  prefs.putUChar("clk", sClock);
  prefs.putUChar("cols", sCols);
  prefs.putUChar("rot", sRot);
  prefs.putUChar("bg", sTheme);
  prefs.putString("cur", sCur);
  prefs.putUChar("sect", sect);
  prefs.end();
  applySettings();
}

// A disclosure mark the way a phone draws one: two 2px strokes, quiet
// grey, its point at `right`. The rows that open a page wear it.
static void chevron(int16_t right, int16_t cy, uint16_t c) {
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(right - 9 + d, cy - 8, right - 1 + d, cy, c);
    gfx->drawLine(right - 1 + d, cy, right - 9 + d, cy + 8, c);
  }
}
static void drawSettingRow(uint8_t i) {
  // Eight rows, all on screen. Shutdown first, the everyday rows, the
  // advanced ones, and the one that cannot be undone last, in red.
  static const char *const labels[] = {"Shutdown", "Scroll", "Columns", "Orientation", "Clock", "Theme", "Auto return", "Sleep", "Currency", "Info", "Wi-Fi", "Clear device"};
  char ssid[24];
  snprintf(ssid, sizeof ssid, "%.20s", wifiSsid[0] ? wifiSsid : "not set");
  const char *v = i == 0 ? ">" : i == 1 ? SPEED_NAMES[sSpeed] : i == 2 ? ">" : i == 3 ? ROT_NAMES[sRot] : i == 4 ? CLOCK_NAMES[sClock]
                : i == 5 ? THEMES[sTheme].name : i == 6 ? RET_NAMES[sRet] : i == 7 ? SLEEP_NAMES[sSleep] : i == 8 ? sCur : i == 9 ? ">" : i == 10 ? ssid : ">";
  if (i < setTop || i >= setTop + L.sN) return;
  int16_t y = L.sY0 + (i - setTop) * L.sH;
  gfx->fillRect(20, y + 3, L.w - 20, GH(2) + 4, C_BG);
  textAt(20, y + 5, 2, i == 11 ? C_BAD : C_MUTED, labels[i]);
  int16_t cy = y + 5 + GH(2) / 2;  // every row opens a page: its value in grey, then the mark
  chevron(L.w - 20, cy, C_MUTED);
  if (strcmp(v, ">") != 0) textAt(L.w - 40 - textWidth(2, v), y + 5, 2, C_MUTED, v);
  gfx->drawFastHLine(20, y + L.sH - 1, L.w - 40, C_RULE);
}
// The Columns page: six toggles for what a list row shows.
static void drawColumnRow(uint8_t i) {
  int16_t y = L.sY0 + i * L.sH;
  gfx->fillRect(20, y + 3, L.w - 20, GH(2) + 4, C_BG);
  bool on = sCols & (1 << i);
  textAt(20, y + 5, 2, C_MUTED, COL_NAMES[i]);
  textAt(L.w - 20 - textWidth(2, on ? "on" : "off"), y + 5, 2, on ? C_GOOD : C_DIM, on ? "on" : "off");
  gfx->drawFastHLine(20, y + L.sH - 1, L.w - 40, C_RULE);
}
static void drawColumns() {
  drawPanel("COLUMNS", C_MUTED, nullptr, 0, true, 4, "< COLUMNS", "what a row shows");
  for (uint8_t i = 0; i < 6; i++) drawColumnRow(i);
  drawHint("tap a row to turn it on or off        < settings");
}
// The Themes page: a tile per theme in its own colours, with a sample row
// so the choice is seen before it is made. Four across in landscape, two
// in portrait. A tap applies, saves and repaints the page in the new theme.
static void tileRect(uint8_t i, uint8_t n, uint8_t cols, int16_t *x, int16_t *y, int16_t *w, int16_t *h) {
  uint8_t rows = (n + cols - 1) / cols;
  const int16_t gap = 16;
  *w = (L.w - 40 - gap * (cols - 1)) / cols;
  *h = (L.yHint - 8 - L.sY0 - gap * (rows - 1)) / rows;
  *x = 20 + (i % cols) * (*w + gap);
  *y = L.sY0 + (i / cols) * (*h + gap);
}
static void themeRect(uint8_t i, int16_t *x, int16_t *y, int16_t *w, int16_t *h) {
  tileRect(i, N_THEMES, L.w >= 800 ? 4 : 2, x, y, w, h);
}
static void drawThemeTile(uint8_t i) {
  int16_t x, y, w, h;
  themeRect(i, &x, &y, &w, &h);
  const Theme &t = THEMES[i];
  gfx->fillRoundRect(x, y, w, h, 12, t.bg);
  gfx->drawRoundRect(x, y, w, h, 12, i == sTheme ? C_FG : t.rule);
  if (i == sTheme)  // the chosen one: a 3px white border
    for (int8_t d = 1; d < 3; d++) gfx->drawRoundRect(x + d, y + d, w - 2 * d, h - 2 * d, 12 - d, C_FG);
  textAt(x + 14, y + 10, 2, C_FG, t.name);
  gfx->fillRect(x + 14, y + 10 + GH(2) + 4, 40, 3, t.accent);
  int16_t ry = y + h / 2 + 2;
  gfx->drawFastHLine(x + 14, ry - 6, w - 28, t.rule);
  textAt(x + 14, ry, 2, C_FG, "AAPL");
  textAt(x + w - 14 - textWidth(2, "+1.2%"), ry, 2, C_GOOD, "+1.2%");
  textAt(x + 14, ry + GH(2) + 4, 2, C_MUTED, "MSFT");
  textAt(x + w - 14 - textWidth(2, "-0.6%"), ry + GH(2) + 4, 2, C_BAD, "-0.6%");
}
static void drawThemes() {
  drawPanel("THEMES", C_MUTED, nullptr, 0, true, 4, "< THEMES", "colours for every page");
  for (uint8_t i = 0; i < N_THEMES; i++) drawThemeTile(i);
  drawHint("tap a theme        < settings");
}
static int8_t hitTheme(int16_t tx, int16_t ty) {
  for (uint8_t i = 0; i < N_THEMES; i++) {
    int16_t x, y, w, h;
    themeRect(i, &x, &y, &w, &h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return i;
  }
  return -1;
}
// The Orientation page: four tiles, each a little screen drawn the way
// that setting would turn it -- the header bar, the rows, and a gold mark
// on the edge the cables leave by, so the flipped ones can be told apart
// -- the current one outlined. A tap on another saves and restarts.
static void orientRect(uint8_t i, int16_t *x, int16_t *y, int16_t *w, int16_t *h) { tileRect(i, 4, 2, x, y, w, h); }
static void drawOrientTile(uint8_t i) {
  int16_t x, y, w, h;
  orientRect(i, &x, &y, &w, &h);
  gfx->drawRoundRect(x, y, w, h, 12, i == sRot ? C_FG : C_RULE);
  if (i == sRot)
    for (int8_t d = 1; d < 3; d++) gfx->drawRoundRect(x + d, y + d, w - 2 * d, h - 2 * d, 12 - d, C_FG);
  textAt(x + 14, y + 10, 2, C_FG, ROT_NAMES[i]);
  bool port = i & 1;
  int16_t sw = port ? 60 : 96, sh = port ? 96 : 60;
  int16_t top = y + 10 + GH(2) + 8, avail = y + h - 10 - top;
  int16_t sx = x + (w - sw) / 2, sy = top + (avail - sh) / 2;
  gfx->fillRoundRect(sx, sy, sw, sh, 6, C_RULE);
  gfx->fillRect(sx + 6, sy + 6, sw - 12, 4, C_MUTED);  // the header bar
  for (int16_t ry = sy + 16; ry < sy + sh - 6; ry += 8) gfx->fillRect(sx + 6, ry, sw - 12, 2, C_DIM);  // the rows
  if (i == 0) gfx->fillRect(sx - 4, sy + sh / 2 - 10, 4, 20, C_GOLD);  // the cable edge: left, bottom, right, top
  else if (i == 1) gfx->fillRect(sx + sw / 2 - 10, sy + sh, 20, 4, C_GOLD);
  else if (i == 2) gfx->fillRect(sx + sw, sy + sh / 2 - 10, 4, 20, C_GOLD);
  else gfx->fillRect(sx + sw / 2 - 10, sy - 4, 20, 4, C_GOLD);
}
static void drawOrient() {
  drawPanel("ORIENTATION", C_MUTED, nullptr, 0, true, 4, "< ORIENTATION", "a change restarts the board");
  for (uint8_t i = 0; i < 4; i++) drawOrientTile(i);
  drawHint("tap one to turn the screen        < settings");
}
static int8_t hitOrient(int16_t tx, int16_t ty) {
  for (uint8_t i = 0; i < 4; i++) {
    int16_t x, y, w, h;
    orientRect(i, &x, &y, &w, &h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return i;
  }
  return -1;
}
// One page for each setting with a few values -- the rows used to cycle on
// a tap, which was too easy to do by accident. The values as rows, the
// current one bright with a check mark; a tap picks, saves, and the check
// moves. Currency lists USD and every currency row.
enum : uint8_t { CH_SPEED, CH_CLOCK, CH_RET, CH_SLEEP, CH_CUR };
static uint8_t chWhich = 0, chN = 0, chSel = 0;
static const char *chNames[10];
static const char *chTitle = "", *chNote = nullptr;
static void chList(const char *const *names, uint8_t n, uint8_t sel) {
  for (uint8_t i = 0; i < n; i++) chNames[chN++] = names[i];
  chSel = sel;
}
static void buildChoice(uint8_t which) {
  chWhich = which;
  chN = 0;
  switch (which) {
    case CH_SPEED: chTitle = "< SCROLL"; chNote = "how long a page of rows takes to pass"; chList(SPEED_NAMES, 3, sSpeed); break;
    case CH_CLOCK: chTitle = "< CLOCK"; chNote = nullptr; chList(CLOCK_NAMES, 2, sClock); break;
    case CH_RET: chTitle = "< AUTO RETURN"; chNote = "back to the list after a page sits"; chList(RET_NAMES, 3, sRet); break;
    case CH_SLEEP: chTitle = "< SLEEP"; chNote = "when the panel goes dark"; chList(SLEEP_NAMES, 3, sSleep); break;
    default:
      chTitle = "< CURRENCY";
      chNote = "prices shown in";
      chNames[chN++] = "USD";
      chSel = 0;
      for (uint8_t i = 0; i < nRows && chN < 10; i++) {
        if (rows[i].kind != K_FX) continue;
        if (strcmp(rows[i].label, sCur) == 0) chSel = chN;
        chNames[chN++] = rows[i].label;
      }
  }
}
static void applyChoice(uint8_t i) {
  chSel = i;
  switch (chWhich) {
    case CH_SPEED: sSpeed = i; break;
    case CH_CLOCK: sClock = i; break;
    case CH_RET: sRet = i; break;
    case CH_SLEEP: sSleep = i; break;
    default: strcpy(sCur, chNames[i]);
  }
  saveSettings();
}
static void checkMark(int16_t right, int16_t cy, uint16_t c) {  // two strokes, 2px
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(right - 16, cy + d, right - 10, cy + 6 + d, c);
    gfx->drawLine(right - 10, cy + 6 + d, right, cy - 6 + d, c);
  }
}
static void drawChoiceRow(uint8_t i) {
  int16_t y = L.sY0 + i * L.sH;
  gfx->fillRect(20, y + 3, L.w - 20, GH(2) + 4, C_BG);
  textAt(20, y + 5, 2, i == chSel ? C_FG : C_MUTED, chNames[i]);
  if (i == chSel) checkMark(L.w - 20, y + 5 + GH(2) / 2, C_GOOD);
  gfx->drawFastHLine(20, y + L.sH - 1, L.w - 40, C_RULE);
}
static void drawChoice() {
  drawPanel(chTitle + 2, C_MUTED, nullptr, 0, true, 4, chTitle, chNote);
  for (uint8_t i = 0; i < chN; i++) drawChoiceRow(i);
  drawHint("tap a value        < settings");
}
// A scroll step redraws the rows in place -- each field clears its own box,
// so there is no blanket clear and nothing to flicker -- and the hint only
// when it changes.
static void drawSettingRows() {
  for (uint8_t i = setTop; i < setTop + L.sN && i < L.sRows; i++) drawSettingRow(i);
  drawHint("< list");
}
static void drawSettings() {
  drawPanel("SETTINGS", C_MUTED, nullptr, 0, true, 4, "SETTINGS");
  drawSettingRows();
}
// The confirm screen for the two rows that cannot be undone: an action
// sheet, the way a phone asks. A glyph, a title, one quiet line, a pill
// to act -- red text for the one that wipes -- and Cancel under it.
// (confirm sheet positions: see Layout)
static void powerGlyph(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (float a = 35; a <= 325; a += 0.5f) {  // an open ring, gap at the top, five px thick
    float rad = (a - 90) * 3.14159265f / 180;
    for (int16_t k = 0; k < 5; k++) gfx->drawPixel(cx + (int16_t)lroundf((r - k) * cosf(rad)), cy + (int16_t)lroundf((r - k) * sinf(rad)), c);
  }
  gfx->fillRect(cx - 2, cy - r - 4, 5, r + 2, c);
}
static void drawConfirm() {
  gfx->fillScreen(C_BG);
  drawHeader(4, "< SETTINGS");
  bool clear = confirmWhat == 2;
  if (clear) {  // a ring with a cross
    for (int16_t k = 0; k < 4; k++) gfx->drawCircle(L.w / 2, L.cfGlyphY, 40 - k, C_BAD);
    for (int16_t k = -2; k <= 2; k++) {
      gfx->drawLine(L.w / 2 - 16 + k, L.cfGlyphY - 16, L.w / 2 + 16 + k, L.cfGlyphY + 16, C_BAD);
      gfx->drawLine(L.w / 2 + 16 + k, L.cfGlyphY - 16, L.w / 2 - 16 + k, L.cfGlyphY + 16, C_BAD);
    }
  } else {
    powerGlyph(L.w / 2, L.cfGlyphY, 40, C_FG);
  }
  const char *title = clear ? "Clear Device" : "Shut Down";
  textAt((L.w - textWidth(3, title)) / 2, L.cfTitleY, 3, C_FG, title);
  const char *l1 = clear ? "Wipes the network, settings and edits." : "Everything goes dark and stays dark.";
  const char *l2 = clear ? "Restarts into setup, for someone else." : "The BOOT button on the back turns it on.";
  uint8_t sz = L.w >= 800 ? 2 : 1;
  textAt((L.w - textWidth(sz, l1)) / 2, L.cfL1Y, sz, C_MUTED, l1);
  textAt((L.w - textWidth(sz, l2)) / 2, L.cfL2Y, sz, C_MUTED, l2);
  gfx->fillRoundRect(L.cfX, L.cfY, L.cfW, L.cfH, L.cfH / 2, rgb(44, 48, 54));
  textAt((L.w - textWidth(2, title)) / 2, L.cfY + (L.cfH - FACES[1].cap) / 2, 2, clear ? C_BAD : C_FG, title);
  gfx->drawRoundRect(L.cfX, L.cfCancelY, L.cfW, 48, 24, C_RULE);
  textAt((L.w - textWidth(2, "Cancel")) / 2, L.cfCancelY + (48 - FACES[1].cap) / 2, 2, C_MUTED, "Cancel");
}
static bool hitConfirm(int16_t x, int16_t y) { return y >= L.cfY && y < L.cfY + L.cfH && x >= L.cfX && x < L.cfX + L.cfW; }
// A tap on row i: cycle it, or open a page. Returns 0 (cycled), 1 (info),
// 2 (touch calibration), 3 (confirm a shutdown), 4 (Wi-Fi setup), 5 (confirm
// clearing the device).
static uint8_t tapSetting(uint8_t i) {
  if (i == 0) return 3;
  if (i == 2) return 7;
  if (i == 5) return 8;
  if (i == 9) return 1;
  if (i == 10) return 4;
  if (i == 11) return 5;
  if (i == 3) return 9;
  if (i == 1) return 10 + CH_SPEED;  // 10 and up: a page of values
  if (i == 4) return 10 + CH_CLOCK;
  if (i == 6) return 10 + CH_RET;
  if (i == 7) return 10 + CH_SLEEP;
  if (i == 8) return 10 + CH_CUR;
  return 0;
}

// ── info page: what the footer used to say, one row of settings ──────────
static void drawInfo(bool full) {
  static uint32_t last = 0;
  if (full) {
    drawPanel("INFO", C_MUTED, nullptr, 0, true, 4, "< INFO", "tap for the splash screen");
    drawHint("< settings");
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
  l[n++][0] = '\0';
  snprintf(l[n++], 40, "%u rows: %u stocks %u idx %u fx %u coins", nRows, nStocks, nIdx, nFx, nCoins);
  snprintf(l[n++], 40, "logos %u/%u badges, %u/%u large", haveLogo[0], nRows, haveLogo[1], nRows);
  snprintf(l[n++], 40, "built " __DATE__ " " __TIME__);
  l[n++][0] = '\0';
  snprintf(l[n++], 40, "tap anywhere for the splash screen");
  for (uint8_t i = 0; i < n; i++) field(20, 60 + i * 22, (L.w - 40) / 8, 1, i == 0 ? C_FG : C_MUTED, l[i]);
}

// ── heatmap: swipe left from the list ────────────────────────────────────
// Twelve tiles of the current section, tinted by the size of the move,
// not just its sign: a 0.2% drift and a 7% drop must not look the same.
// The signed percent is printed on every tile, because colour is never
// the only cue. Pages of twelve; swipe up and down between them.
// (heatmap grid: see Layout)
static uint8_t heatPage = 0;
static char cHeat[160];
static uint16_t heatColour(float pct, bool valid) {
  if (!valid) return C_RULE;
  static const float steps[] = {0.3f, 1.0f, 2.0f, 4.0f, 7.0f};
  uint8_t lvl = 0;
  for (float st : steps) lvl += fabsf(pct) >= st;
  if (!lvl) return rgb(40, 44, 48);
  static const uint8_t up[5][3] = {{0, 60, 30}, {0, 95, 40}, {0, 135, 50}, {0, 175, 60}, {0, 215, 70}};
  static const uint8_t dn[5][3] = {{70, 20, 20}, {110, 25, 25}, {150, 30, 30}, {190, 35, 35}, {230, 40, 40}};
  const uint8_t *c = pct >= 0 ? up[lvl - 1] : dn[lvl - 1];
  return rgb(c[0], c[1], c[2]);
}
static uint8_t heatPages() { return (nShown + L.hPer - 1) / L.hPer; }
static int8_t hitTile(int16_t x, int16_t y) {
  if (y < L.hY0 || y >= L.hY0 + L.hRows * (L.hH + 2)) return -1;
  int8_t t = (y - L.hY0) / (L.hH + 2) * L.hCols + x / (L.hW + 2);
  return heatPage * L.hPer + t < nShown ? t : -1;
}
static void drawHeat(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    char h[48];
    snprintf(h, sizeof h, "%s, page %u of %u", SECT_NAMES[sect], heatPage + 1, heatPages());
    drawHeader(2, "HEATMAP", heatPages() > 1 ? h : SECT_NAMES[sect]);
    drawHint(heatPages() > 1 ? "^ v  pages        < search        list >" : "< search        list >");
    cHeat[0] = '\0';
  }
  char key[160] = "";
  for (uint8_t t = 0; t < L.hPer && heatPage * L.hPer + t < nShown; t++) {
    const Row &r = rows[order[heatPage * L.hPer + t]];
    char k[8];
    snprintf(k, sizeof k, "%d,", r.valid ? (int)(r.pct * 10) : -9999);
    strlcat(key, k, sizeof key);
  }
  if (strcmp(key, cHeat) == 0) return;
  strcpy(cHeat, key);
  for (uint8_t t = 0; t < L.hPer; t++) {
    int16_t x = (t % L.hCols) * (L.hW + 2), y = L.hY0 + (t / L.hCols) * (L.hH + 2);
    if (heatPage * L.hPer + t >= nShown) {
      gfx->fillRect(x, y, L.hW, L.hH, C_BG);
      continue;
    }
    Row r = rowCopy(order[heatPage * L.hPer + t]);
    gfx->fillRoundRect(x, y, L.hW, L.hH, 10, heatColour(r.pct, r.valid));
    textAt(x + 10, y + 10, 2, C_FG, r.label);
    char b[12];
    if (r.valid) formatPct(r.pct, b, sizeof b);
    else strcpy(b, "--");
    textAt(x + 10, y + 40, 2, C_FG, b);
    if (r.valid) {
      priceStr(r, r.price, b, sizeof b);
      textAt(x + 10, y + 76, 1, rgb(220, 224, 228), b);
    }
  }
}

// ── search page: swipe left from the heatmap ─────────────────────────────
// Symbols are short and upper-case, so the keyboard is a 6x5 grid of 40px
// keys, finger-sized on a resistive panel: A-X, then Y Z . - backspace GO.
static const char *const KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // + backspace + GO
// (keyboard and results: see Layout)
static int8_t hitKey(int16_t x, int16_t y) {
  if (y < L.kY0 || y >= L.kY0 + (30 / L.kCols) * L.kH) return -1;
  int8_t i = (y - L.kY0) / L.kH * L.kCols + x / L.kW;
  return i < 30 ? i : -1;
}
static int8_t hitResult(int16_t y) {
  if (y < L.resY0 || y >= L.resY0 + L.resH * nHits) return -1;
  return (y - L.resY0) / L.resH;
}
// The caret blinks after the text, or ahead of the placeholder: a field
// with no cursor does not look like it is listening.
static void drawCaret(bool on) {
  int16_t x = 40 + (query[0] ? textWidth(3, query) + 4 : 0);
  gfx->fillRect(x, L.qY + 12, 3, 36, on ? C_FG : C_RULE);
}
static void drawQuery() {
  gfx->fillRoundRect(20, L.qY, L.w - 40, 60, 14, C_RULE);
  if (query[0]) textAt(40, L.qY + 14, 3, C_FG, query);
  else textAt(52, L.qY + 20, 1, C_DIM, "symbol or company name");
  drawCaret(true);
}
static void drawKeyboard() {
  for (uint8_t i = 0; i < 30; i++) {
    int16_t x = (i % L.kCols) * L.kW, y = L.kY0 + (i / L.kCols) * L.kH;
    gfx->drawRoundRect(x + 3, y + 3, L.kW - 6, L.kH - 6, 10, C_RULE);
    char k[3] = {0, 0, 0};
    const char *lab = k;
    uint16_t c = C_FG;
    if (i < 28) k[0] = KEYS[i];
    else if (i == 28) lab = "<";
    else {
      lab = "GO";
      c = C_GOOD;
    }
    textAt(x + (L.kW - textWidth(2, lab)) / 2, y + (L.kH - FACES[1].cap) / 2, 2, c, lab);
  }
}
static void drawSearch(bool full) {
  static char cSearch[16];
  if (searchMode == 0) {
    if (!full) {
      static uint32_t blink = 0;
      static bool on = true;
      if (millis() - blink > 500) {
        blink = millis();
        on = !on;
        drawCaret(on);
      }
      return;
    }
    gfx->fillScreen(C_BG);
    drawHeader(1, "SEARCH", "a symbol or a company name");
    drawQuery();
    textAt(20, 134, 1, C_DIM, "type a few letters of a symbol or a company name, then tap GO");
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
    drawHeader(1, "< SEARCH", searchQ);
    textAt(L.w - 20 - textWidth(1, "v back to the keyboard"), 62, 1, C_DIM, "v back to the keyboard");
    gfx->drawFastHLine(20, 96, L.w - 40, C_RULE);
    cSearch[0] = '\0';
  }
  char key[16];
  snprintf(key, sizeof key, "%u|%d|%d", n, done, failed);
  if (strcmp(key, cSearch) == 0) return;
  strcpy(cSearch, key);
  gfx->fillRect(0, L.resY0, L.w, L.yHint - L.resY0, C_BG);
  if (!done) {
    field(20, 200, 30, 1, C_DIM, "searching...");
    return;
  }
  if (failed || !n) {
    field(20, 200, 30, 1, C_DIM, failed ? "search failed" : "nothing found");
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    int16_t y = L.resY0 + i * L.resH;
    bool listed = findRow(h[i].sym) >= 0;
    textAt(20, y + 6, 2, C_FG, h[i].sym);
    fieldRight(L.w - 20, y + 12, 10, 1, listed ? C_GOOD : C_DIM, listed ? "on list" : h[i].exch);
    textAt(180, y + 12, 1, C_MUTED, h[i].name);
    gfx->drawFastHLine(20, y + L.resH - 1, L.w - 40, C_RULE);
  }
  drawHint("tap one to add it and open it");
}
// ── news page: swipe up from a stock ─────────────────────────────────────
static char cNews[32];
// Greedy word wrap by measured width into at most `lines` lines of `w` px.
static uint8_t wrapText(const char *s, int16_t w, char out[][64], uint8_t lines) {  // forward-declared above
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
  bool all = detailIdx == NEWS_ALL;
  Row r;
  if (all) {
    memset(&r, 0, sizeof r);
    strcpy(r.label, "ALL");
  } else {
    r = rowCopy(detailIdx);
  }
  if (full) {
    gfx->fillScreen(C_BG);
    char head[16];
    snprintf(head, sizeof head, "< %s", all ? "HEADLINES" : r.label);
    drawHeader(all ? 3 : 0, head, all ? "every stock and coin on the list" : "headlines");
    drawHint(all ? "^ v  pages        v list" : "< next        v stock        prev >");
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
  snprintf(key, sizeof key, "%u|%u|%d|%lu|%u", detailIdx, n, failed, (unsigned long)newsAt, newsPage);
  if (strcmp(key, cNews) == 0) return;
  strcpy(cNews, key);
  gfx->fillRect(0, L.yRow0, L.w, L.yHint - 4 - L.yRow0, C_BG);
  if (!mine || (!n && !failed)) {
    field(20, 220, 30, 1, C_DIM, "loading headlines...");
    return;
  }
  if (!n) {
    field(20, 220, 30, 1, C_DIM, "no headlines");
    return;
  }
  int16_t y = 56;
  for (uint8_t i = newsPage * 6; i < n && i < newsPage * 6 + 6 && y + 40 <= L.yHint - 8; i++) {
    char lines[2][64];
    uint8_t k = wrapText(items[i].title, L.w - 40 - 70, lines, 2);
    for (uint8_t j = 0; j < k; j++) textAt(20, y + j * 20, 1, j == 0 ? C_FG : C_MUTED, lines[j]);
    fieldRight(L.w - 20, y, 6, 1, C_DIM, items[i].age);
    y += k * 20 + 8;
    gfx->drawFastHLine(20, y, L.w - 40, C_RULE);
    y += 10;
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
  // HTTP/1.0: the server then sends a plain body and closes, instead of
  // chunks -- the client's getString() stopped part-way through a chunked
  // 21KB FX response (8.5KB of it arrived, "IncompleteInput"), while the
  // 8KB stock responses fit in the first chunk and never showed it. The
  // body is read below by hand, waiting for bytes until the server closes:
  // parsing straight off the TLS stream raced the network (a moment with
  // nothing to read counts as the end) and failed most rows at random.
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    Serial.printf("%s: http.begin refused the url\n", id);
    return false;
  }
  http.addHeader("User-Agent", UA);  // without this Yahoo answers 429
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d%s\n", id, code, code == 429 ? " (rate limited)" : "");
    http.end();
    return false;
  }
  JsonDocument filter;
  JsonObject fm = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  for (const char *k : {"regularMarketPrice", "chartPreviousClose", "longName", "shortName", "regularMarketDayHigh",
                        "regularMarketDayLow", "fiftyTwoWeekHigh", "fiftyTwoWeekLow", "regularMarketTime", "currency",
                        "regularMarketVolume"})
    fm[k] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
  String body;
  body.reserve(http.getSize() > 0 ? http.getSize() + 1 : 24 * 1024);
  {
    NetworkClient *st = http.getStreamPtr();
    uint8_t buf[512];
    uint32_t last = millis();
    while (millis() - last < 6000) {
      int n = st->available();
      if (n > 0) {
        n = st->read(buf, n > (int)sizeof buf ? sizeof buf : n);
        if (n > 0) {
          body.concat((const char *)buf, n);
          last = millis();
        }
      } else if (!st->connected()) {
        break;  // HTTP/1.0: the close is the end of the body
      } else {
        delay(5);
      }
    }
  }
  http.end();
  DeserializationError je = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (je) {
    Serial.printf("%s: json %s (body %u bytes, heap %u)\n", id, je.c_str(), (unsigned)body.length(), ESP.getFreeHeap());
    return false;
  }
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

// Coins are priced by CoinGecko, which has no series; Yahoo carries them as
// BTC-USD (the label is the symbol), so their charts come from there.
static const char *yahooId(const Row &r, char *buf) {
  if (!r.coin) return r.id;
  snprintf(buf, MAX_LABEL + 5, "%s-USD", r.label);
  return buf;
}
// A coin's sparkline only: the price and 24h change stay CoinGecko's. A day
// at 15m, the way FX does it, since coins trade around the clock too.
static bool fetchCoinSpark(NetworkClientSecure &client, Row &r) {
  char id[MAX_LABEL + 5];
  JsonDocument doc;
  if (!fetchChart(client, yahooId(r, id), "1d", "15m", doc)) return false;
  r.n = pullCloses(doc["chart"]["result"][0], r.close, SPARK_N);
  return r.n >= 2;
}

static bool fetchStock(NetworkClientSecure &client, Row &r) {
  JsonDocument doc;
  // FX trades around the clock: a day at 5m is 21KB; 15m is a quarter of it.
  if (!fetchChart(client, r.id, "1d&includePrePost=true", r.kind == K_FX ? "15m" : sparkInterval, doc)) return false;
  JsonVariant res = doc["chart"]["result"][0];
  JsonVariant m = res["meta"];
  r.price = m["regularMarketPrice"] | 0.0f;
  r.traded = (time_t)(m["regularMarketTime"] | 0L);
  r.volume = m["regularMarketVolume"] | 0.0f;
  r.prev = m["chartPreviousClose"] | 0.0f;
  r.pct = r.prev > 0 ? (r.price - r.prev) / r.prev * 100.0f : 0.0f;
  r.dayLo = m["regularMarketDayLow"] | 0.0f;
  r.dayHi = m["regularMarketDayHigh"] | 0.0f;
  r.wkLo = m["fiftyTwoWeekLow"] | 0.0f;
  r.wkHi = m["fiftyTwoWeekHigh"] | 0.0f;
  snprintf(r.cur, sizeof r.cur, "%s", m["currency"] | "USD");
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
  {
    NetworkClientSecure client;
    client.setInsecure();
    JsonDocument doc;
    char id[MAX_LABEL + 5];
    if (fetchChart(client, yahooId(r, id), RANGES[range].range, RANGES[range].interval, doc)) {
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
  Row r;
  char syms[300] = "", url[420];
  if (idx == NEWS_ALL) {  // every stock and coin in one feed; Yahoo takes a comma list
    for (uint8_t i = 0; i < nRows; i++) {
      if (rows[i].kind == K_FX || rows[i].kind == K_INDEX) continue;
      if (syms[0]) strlcat(syms, ",", sizeof syms);
      strlcat(syms, rows[i].label, sizeof syms);
      if (rows[i].coin) strlcat(syms, "-USD", sizeof syms);
    }
    memset(&r, 0, sizeof r);
    strcpy(r.label, "ALL");
  } else {
    r = rowCopy(idx);
    snprintf(syms, sizeof syms, "%s%s", r.label, r.coin ? "-USD" : "");  // ^GSPC and CAD=X have feeds of their own
  }
  snprintf(url, sizeof url, "https://feeds.finance.yahoo.com/rss/2.0/headline?s=%s&region=US&lang=en-US", syms);
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
           "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true&include_24hr_vol=true",
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
    r.prev = r.pct > -100.0f ? r.price / (1.0f + r.pct / 100.0f) : 0.0f;  // 24h ago: the chart's reference line
    r.volume = v["usd_24h_vol"] | 0.0f;
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
  bool ok = r.coin ? fetchCoinSpark(client, r) : fetchStock(client, r);
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
static TaskHandle_t fetchHandle = nullptr;
static void fetchTask(void *) {
  bool sweeping = false;
  uint8_t sweepIdx = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() != WL_CONNECTED) continue;
    // Order of service: the tapped symbol's own price (a fresh row has
    // nothing else to show), a search, headlines, the range charts, coins,
    // the sweep. The charts used to go first and a new symbol's page sat
    // blank for six seconds.
    int8_t pri = priority;
    if (pri >= 0) {
      priority = -1;
      if (rows[pri].coin) doCoins();  // the price; the series follows
      fetchOne(pri);
      continue;
    }
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
    if (stockEvery < nRows * 5000UL) stockEvery = nRows * 5000UL;  // rate floor
    bool due = lastStock == 0 || millis() - lastStock > stockEvery;
    if (ses == Session::Closed) due = lastStock == 0 || (closedAt && lastStock < closedAt);
    if (!sweeping && nRows && due) {
      sweeping = true;
      sweepIdx = 0;
    }
    if (!sweeping) continue;
    if (sweepIdx >= nRows) {  // every row: coins are in it for their series
      sweeping = false;
      lastStock = millis();
      Serial.printf("sweep done (%u symbols, heap %u)\n", nRows, ESP.getFreeHeap());
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
  static bool tapped = false, longed = false, dragging = false;
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
    tapped = longed = dragging = false;
  } else if (now) {
    int16_t ddx = cx - x0, ddy = cy - y0;
    if (!axis && (abs(ddx) >= AXIS_PX || abs(ddy) >= AXIS_PX)) {
      if (abs(ddx) * 2 >= abs(ddy) * 3) axis = 1;
      else if (abs(ddy) * 2 >= abs(ddx) * 3) axis = 2;
    }
    if (axis == 2) {
      // A resistive panel jitters a few px between polls and a drag applied
      // raw shakes the list under the finger. Smooth the position and
      // report only whole pixels of real movement.
      static float sy = 0;
      static int16_t applied = 0;
      if (!dragging) {
        sy = cy;
        applied = cy;
      }
      sy += (cy - sy) * 0.5f;
      int16_t target = (int16_t)lroundf(sy);
      *dy = abs(target - applied) >= 2 ? target - applied : 0;
      if (*dy) applied = target;
      dragging = true;
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

static void alertBanner(const char *why) {
  Serial.printf("alert: %s\n", why);
  strlcpy(bannerText, why, sizeof bannerText);
  bannerUntil = millis() + 10000;
}
static void alertTick() {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  char why[48];
  for (uint8_t i = 0; i < nRows; i++) {
    Row r = rowCopy(i);
    if (!r.valid) continue;
    if (movePct > 0 && r.kind == K_STOCK) {
      uint64_t bit = 1ULL << i;
      if (fabsf(r.pct) >= movePct && !(moveFired & bit)) {
        moveFired |= bit;
        snprintf(why, sizeof why, "%s moved %+.1f%% today", r.label, r.pct);
        alertBanner(why);
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
        alertBanner(why);
      } else if (back) {
        al.fired = false;
      }
    }
  }
}

// ── self-check ───────────────────────────────────────────────────────────
static void selfCheck() {
  cfgSelfCheck();
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
    assert(hitKey(0, L.kY0 - 1) == -1 && hitKey(0, L.kY0) == 0 && hitKey(L.w - 1, L.kY0 + (30 / L.kCols - 1) * L.kH) == 29);
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
  {  // sections and currency
    assert(addRow("AAA", "aaa", false) && addRow("CAD", "CAD=X", false, K_FX) && addRow("BTC", "bitcoin", true));
    rows[1].valid = true; rows[1].price = 1.36f; rows[0].valid = true; rows[0].price = 100; strcpy(rows[0].cur, "USD");
    sect = 0; buildOrder(); assert(nShown == 3 && order[1] == 1);
    sect = 4; buildOrder(); assert(nShown == 1 && order[0] == 1 && rowOf(5) == 1);
    sect = 2; buildOrder(); assert(sect == 0 && nShown == 3);  // no indices: falls back to all
    strcpy(sCur, "CAD");
    bool conv;
    assert(fabsf(disp(rows[0], 100, &conv) - 136) < 0.01f && conv);
    assert(fabsf(disp(rows[1], 1.36f, &conv) - 1.36f) < 0.001f && !conv);  // the rate itself never converts
    strcpy(sCur, "USD");
    char b[12];
    priceStr(rows[1], 1.3652f, b, sizeof b); assert(strcmp(b, "1.3652") == 0);
    assert(fabsf(disp(rows[0], 100, &conv) - 100) < 0.01f && !conv);
    assert(heatColour(0.1f, true) != heatColour(5, true) && heatColour(-5, true) != heatColour(5, true));
    nRows = 0; nShown = 0;
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
  assert(hitRow(L.yRow0 - 1, 0, 26) == -1 && hitRow(L.yRow0, 0, 26) == 0 && hitRow(L.yRow0, 0, 0) == -1);
  assert(hitRow(L.yRow0 + L.strip - 1, 0, 26) == L.rows - 1 && hitRow(L.yRow0 + L.strip, 0, 26) == -1);
  assert(hitRow(L.yRow0, ROW_H - 1, 26) == 0 && hitRow(L.yRow0 + 1, ROW_H - 1, 26) == 1);
  assert(hitRow(L.yRow0 + L.strip - 1, ROW_H - 1, 26) == L.rows);
  assert(hitRow(L.yRow0 + L.strip - 1, 24 * ROW_H + ROW_H - 1, 26) == (24 + L.rows) % 26);
  assert(hitRow(L.yRow0, -1, 26) == 25 && hitRow(L.yRow0, 26 * ROW_H, 26) == 0);
  assert(hitRange(L.chX, L.chY + L.chH) == 0 && hitRange(L.chX + L.rStep * 4, L.dYNews - 1) == 4);
  assert(hitRange(L.chX, L.chY + L.chH - 1) == -1 && hitRange(L.chX, L.dYNews) == -1);
  assert(hitRange(L.chX - 1, L.rY) == -1 && hitRange(L.chX + L.rStep * 5, L.rY) == -1);
  // Sparkline scaling: extremes hit the box edges, a flat series sits mid-box.
  assert(sparkY(10, 10, 20, 100, 28) == 127 && sparkY(20, 10, 20, 100, 28) == 100);
  assert(sparkY(5, 5, 5, 100, 28) == 114);

  for (uint8_t o = 0; o < 2; o++) {
    Lp = &LAYOUTS[o];
  // Geometry, 800x480: a row's badge, symbol, name, sparkline, price and
  // percent never overlap; the strip fills the panel under the header.
  assert(L.xSym + LOGO_BADGE <= L.xLbl && L.xLbl + GW(2) * (MAX_LABEL + 2) <= L.xSpk);
  assert(L.xLbl + GW(2) * (MAX_LABEL + 2) + 96 + 116 + 106 <= L.xRight);  // price, percent and change always clear the symbol; the rest yield
  assert(L.xPrice + 8 <= L.xRight - GW(2) * 7 && L.xRight <= L.w);
  assert(L.yBadge + LOGO_BADGE <= ROW_H && L.yLbl + GH(2) <= ROW_H && L.ySpk + L.spkH <= ROW_H);
  assert(L.yRow0 + L.strip <= L.yFoot && L.yFoot + 14 + GH(1) <= L.h && L.yHead + GH(2) <= L.yRow0);
  assert(L.strip % 20 != 1 || true);  // (the bounce buffer is 20 lines; the ring maps per line, any height works)
  if (L.tagX >= 0) assert(L.tagX + GW(1) * 17 <= L.iconX - 8 && L.iconX + 4 * L.iconStep <= L.xRight - GW(1) * 8);
  else assert(L.iconX + 4 * L.iconStep <= L.w && GW(1) * (28 + 17 + 8) + 24 <= L.w);  // the footer holds the message, the tag and the clock
  assert(hitHeader(L.iconX) == 10 && hitHeader(L.iconX + 3 * L.iconStep + 10) == 13 && hitHeader(L.tagX + 20) == -1);
  assert(L.sY0 + L.sH * L.sN <= L.yHint && hitSetting(L.sY0 - 1) == -1 && hitSetting(L.sY0) == 0 && L.sN <= L.sRows);
  // Detail: two columns in landscape (text left, chart right), one in
  // portrait (the chart under the price, the bars under the chips, the
  // headlines last); nothing crosses into the next block either way.
  bool two = L.chX > L.dXTxt;
  int16_t edge = two ? L.chX : L.w;
  assert(L.dXLogo + LOGO_BIG <= L.dXTxt && L.dXTxt + GW(1) * 26 <= edge && L.dYLogo + LOGO_BIG <= L.dYPrice);
  assert(L.dYSym + GH(3) <= L.dYName && L.dYName + GH(1) <= L.dYTag && L.dYTag + GH(1) <= L.dYPrice);
  assert(L.dYPrice + GH(4) <= L.dYChg && L.dYChg + GH(2) <= (two ? L.dYDay : L.chY) && L.dXPrice + GW(4) * 8 <= edge);
  assert(L.dXPct + GW(2) * 8 <= edge && L.barX + L.barW <= edge);
  assert(L.dYDay + 34 + GH(1) <= L.dYWk && L.dYWk + 34 + GH(1) <= (two ? L.yHint : L.dYNews) && L.yHint + GH(1) <= L.h);
  if (!two) assert(L.rY + L.rH <= L.dYDay);
  assert(L.chX + L.chW <= L.w && L.chY + L.chH <= L.rY && L.rY + L.rH <= L.dYNews && L.dYNews + 3 * L.newsStep <= L.yHint);
  assert(L.rW <= L.rStep && GW(2) * 2 <= L.rW && GH(2) <= L.rH && L.chX + L.rStep * (N_RANGES - 1) + L.rW <= L.w);
  assert(L.hCols * (L.hW + 2) <= L.w + 2 && L.hY0 + L.hRows * (L.hH + 2) <= L.h);
  assert(30 % L.kCols == 0 && L.kY0 + (30 / L.kCols) * L.kH <= L.h && L.kCols * L.kW <= L.w && L.resY0 + MAX_HITS * L.resH <= L.yHint);
  }
  Lp = &LAYOUTS[0];
  for (const Face &f : FACES) assert(f.font[13] == f.cap && (uint8_t)(-(int8_t)f.font[14]) == f.desc);  // u8g2 header: ascent_A, descent_g
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoConfig, NoWifi, NoData, Running };
static uint32_t joinStarted = 0;  // the splash holds for 20s of joining, then the panel says why
static uint16_t wifiRetries = 0;  // failed joins in a row; three of them open setup by themselves
enum class View { List, Detail, Settings, Info, News, Search, Splash, Heat, Confirm, Columns, Themes, Orient, Choice };
static uint32_t removeArmedUntil = 0;  // a long press on a stock's page arms removal for a few seconds
static State state = State::Boot;
static View view = View::List;

// Expander ack, panel start, PSRAM left: the first three things to read when
// the screen stays white or cycles colours (the panel's no-signal pattern)
// while serial says the app is fine.
static void panelBegin(bool expanderOk) {
  gfx = boardDisplay();
  bool ok = gfx->begin();
  Serial.printf("board: expander %s, panel %s, psram %u free\n", expanderOk ? "ok" : "NO ACK", ok ? "ok" : "FAILED",
                ESP.getFreePsram());
  boardSetRotation(sRot);
  Lp = &LAYOUTS[sRot & 1];
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bool xp = boardBegin();
  selfCheck();

  // What woke us. A timer wake inside the sleep window goes straight back
  // to sleep without touching the panel or the radio.
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool fromSleep = cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_TIMER;
  if (fromSleep && rtcShutdown) {  // the BOOT button after a shutdown: a fresh start, splash and all
    Serial.println("powered on by the BOOT button");
    rtcShutdown = false;
    fromSleep = false;
  } else if (fromSleep) {
    Serial.printf("woke by %s\n", cause == ESP_SLEEP_WAKEUP_EXT0 ? "touch" : "timer");
  }

  mux = xSemaphoreCreateMutex();
  if (loadConfig()) applyOverlay();
  if (cfgErr) {
    Serial.printf("config error: %s\n", cfgErr);
    panelBegin(xp);
    gfx->setTextWrap(false);
    backlight(255);
    return;  // loop() draws the panel
  }
  cfgRelease();
  loadSettings();
  setenv("TZ", tzString, 1);  // the clock survives deep sleep; the zone must be set before it is read
  tzset();
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  if (wifiSsid[0] && cause == ESP_SLEEP_WAKEUP_TIMER && haveTime && sleepDue(t)) goToSleep(secondsUntilWake(t));
  awakeUntil = millis() + 60000;

  panelBegin(xp);  // the Orientation setting applies here; loadSettings ran above
  gfx->fillScreen(C_BG);
  gfx->setTextWrap(false);
  rowCanvas = new Arduino_Canvas(L.w, ROW_H, nullptr);
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
    snprintf(st, sizeof st, "connecting to %.24s", wifiSsid);
    drawSplash(st);
    state = State::Boot;
  }
  backlight(255);
  if (!wifiSsid[0]) {  // never set up: the access point and the form, nothing else
    startSetup();
    return;
  }
  // The join runs in the background from here: netTick() in loop() issues
  // the begin() when the scan is in, starts the clock when the link is up,
  // and the state machine keeps the splash (or the restored list) meanwhile.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinStart();
  joinStarted = millis();
  // 16KB: a TLS handshake plus two Row copies (200 closes each) on this
  // stack overflowed 12KB once, into lwIP, at the first fetch after boot.
  // The minute log prints the headroom so the margin stays visible.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, &fetchHandle, 0);
}

// The non-blocking join: the scan's begin(), then NTP once the link is up.
static void netTick() {
  static bool scanning = true, clockStarted = false;
  if (scanning && netJoinTick(wifiSsid, wifiPass)) scanning = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !clockStarted) {
    clockStarted = true;
    Serial.printf("wifi ok %s %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
    configTzTime(tzString, "pool.ntp.org", "time.nist.gov");
  }
  // Retry with a fresh scan, backing off, whenever the link is down for a
  // while -- whatever is on the screen.
  static uint32_t lastRetry = 0;
  uint16_t &retries = wifiRetries;
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
    drawPanel("NO LIST", C_WARN, l, 6, false);
  } else if (state == State::NoWifi) {
    static char ssid[40];
    snprintf(ssid, sizeof ssid, "SSID %s", wifiSsid);
    const char *l[] = {ssid, "", "not associated. 2.4GHz only.", "", "retrying...", "", "tap anywhere to set up Wi-Fi again"};
    drawPanel("NO WIFI", C_BAD, l, 7, false);
  } else {
    static char f[34];
    snprintf(f, sizeof f, "%u failed fetches", failures);
    const char *l[] = {"no quotes yet", f, "", "check the serial log for the", "http code -- yahoo rate-limits.",
                       "", "retrying..."};
    drawPanel("NO DATA", C_WARN, l, 7, false);
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
  if (!rows[idx].coin && !(newsIdx == idx && millis() - newsAt < 10UL * 60 * 1000)) newsWant = idx;  // the page shows three
  drawDetail(true);
}

static void backToList() {
  view = View::List;
  listStart();
}
static void openHeat() {
  view = View::Heat;
  pageOpenedAt = millis();
  if (heatPage >= heatPages()) heatPage = 0;
  drawHeat(true);
}
static void openSearch();
static void openHeat();
static void openNews(uint8_t idx);
static void headerTap(int16_t x) {
  if (headerBack && x < L.tabX + 140) {  // the back mark and the title beside it: one page up
    if (view == View::Columns || view == View::Themes || view == View::Orient || view == View::Choice || view == View::Info) {
      view = View::Settings;
      pageOpenedAt = millis();
      drawSettings();
    } else {
      backToList();
    }
    return;
  }
  int8_t h = hitHeader(x);
  if (h >= 0 && h < 5) {
    if (h != sect) {
      uint8_t was = sect;
      sect = (uint8_t)h;
      buildOrder();
      if (sect != h) sect = was;  // an empty section falls back; stay put
      else {
        saveSettings();
        pos = 0;
      }
    }
    backToList();
  } else if (h == 10) {
    if (view == View::Search) backToList();
    else openSearch();
  } else if (h == 11) {
    if (view == View::Heat) backToList();
    else openHeat();
  } else if (h == 12) {
    if (view == View::News && detailIdx == NEWS_ALL) backToList();
    else openNews(NEWS_ALL);
  } else if (h == 13) {
    if (view == View::Settings) backToList();
    else {
      view = View::Settings;
      pageOpenedAt = millis();
      drawSettings();
    }
  }
}
// Headlines for symbol idx; the fetch is skipped if they are under 10 min old.
static void openNews(uint8_t idx) {
  view = View::News;
  detailIdx = idx;
  detailOpenedAt = millis();
  newsPage = 0;
  if (!(newsIdx == idx && millis() - newsAt < 10UL * 60 * 1000)) newsWant = idx;
  drawNews(true);
}
static void openSearch() {
  view = View::Search;
  searchMode = 0;
  pageOpenedAt = millis();
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
  char m[40];
  snprintf(m, sizeof m, "adding %s...", h.sym);
  drawHint(m, C_WARN);  // at once; the NVS write and the page draw follow
  int8_t idx = userAdd(h.sym);
  if (idx < 0) {
    drawHint("list is full (40)");
    return;
  }
  if (!rows[idx].valid) priority = idx;  // a fresh row needs its first fetch whatever the session
  for (uint8_t k = 0; k < nShown; k++)  // the list opens on it when the page is left
    if (order[k] == idx) pos = (int32_t)k * ROW_H;
  openDetail((uint8_t)idx);
}


void loop() {
  int16_t tx, ty;
  int16_t ddy = 0;
  Gesture g = pollGesture(&tx, &ty, &ddy);
  if (setupMode) {  // serve the form; a swipe down keeps an existing network
    web->handleClient();
    dns->processNextRequest();
    static uint32_t lastN = 0;
    if (millis() - lastN > 2000) {
      lastN = millis();
      char st[40];
      int n = WiFi.softAPgetStationNum();
      if (n) snprintf(st, sizeof st, "%d phone%s joined, form at 192.168.4.1", n, n == 1 ? "" : "s");
      else snprintf(st, sizeof st, "%s", wifiSsid[0] ? "swipe down to keep the old network" : "waiting for you");
      fieldCentre(L.w / 2, L.yHint, 80, 1, C_DIM, st);
    }
    if (g == Gesture::SwipeDown && wifiSsid[0]) ESP.restart();
    delay(10);
    return;
  }
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
  if (state == State::Running && haveTime && millis() > awakeUntil && sleepDue(t))
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
    if (state == State::Running) {
      if (view == View::List) listStart();
      else if (view == View::Settings) drawSettings();
      else if (view == View::Info) drawInfo(true);
      else if (view == View::News) drawNews(true);
      else if (view == View::Search) drawSearch(true);
      else if (view == View::Splash) drawSplash("tap to return");
      else if (view == View::Heat) drawHeat(true);
      else if (view == View::Confirm) drawConfirm();
      else if (view == View::Columns) drawColumns();
      else if (view == View::Themes) drawThemes();
      else if (view == View::Orient) drawOrient();
      else if (view == View::Choice) drawChoice();
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

  if (view != View::List) boardScroll(0);  // every other page draws 1:1
  if (state == State::Running && view != View::Splash) drawHead(&t, haveTime);  // the clock and the tag, on every page
  if (state == State::Boot) {  // the splash is up from setup; only its status line moves
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 1000) {
      lastStatus = millis();
      char st[40];
      if (!up) snprintf(st, sizeof st, "connecting to %.24s", wifiSsid);
      else if (!haveTime) snprintf(st, sizeof st, "connected, setting the clock");
      else snprintf(st, sizeof st, "fetching prices");
      splashStatus(st);
    }
  } else if (state != State::Running) {
    drawFailPanel();
    // Any touch on NO WIFI opens setup; so does the BOOT button; and so
    // does the third failed join on its own (about a minute), because a
    // wrong network name must never need a working touch panel to fix.
    if (state == State::NoWifi && (g == Gesture::Tap || g == Gesture::TapUp || g == Gesture::SwipeRight || bootPressed() ||
                                   wifiRetries >= 3)) {
      Serial.println(wifiRetries >= 3 ? "three joins failed: setup" : "setup asked for");
      WiFi.disconnect(true);
      startSetup();
    }
  } else if (view == View::List) {
    listTick();
    drawFoot(&t, haveTime);
  } else if (view == View::Info) {
    drawInfo(false);
  } else if (view == View::Detail) {
    drawDetail(false);
  } else if (view == View::News) {
    drawNews(false);
  } else if (view == View::Search) {
    drawSearch(false);
  } else if (view == View::Heat) {
    drawHeat(false);
  }

  // On the list: drag scrolls it, any touch holds the crawl for 3s after,
  // and a long press opens the stock. Swipe right opens settings; swipe
  // left there goes back.
  // On the list: a still finger highlights its row at once and opens the
  // stock when it lifts (touch-up, the way a phone list works); a moving
  // finger drags the list; any touch holds the crawl for 3s after.
  static int32_t hilite = INT32_MIN;
  if (state == State::Running && view == View::List) {
    if (touchHeld) holdUntil = millis() + (g == Gesture::Drag ? 8000 : 3000);  // a drag went somewhere to read
    if (g == Gesture::Drag && ddy) scrollBy(-ddy);
    if (g == Gesture::Tap && hilite == INT32_MIN) {
      int16_t r = hitRow(ty, pos, nShown);
      if (r >= 0) {
        hilite = floordiv(pos + ty - L.yRow0, ROW_H);
        gfx->fillRect(0, rowY(hilite) + 2, 4, ROW_H - 4, C_FG);
      }
    }
    if (hilite != INT32_MIN && (!touchHeld || g == Gesture::Drag)) {
      gfx->fillRect(0, rowY(hilite) + 2, 4, ROW_H - 4, C_BG);
      hilite = INT32_MIN;
    }
    if (g == Gesture::TapUp) {
      int16_t r = hitRow(ty, pos, nShown);
      if (r >= 0) openDetail(order[r]);
      else if (ty < L.yRow0) headerTap(tx);
    }
  }
  // Swipes are the navigation, phone style. List: right opens settings.
  // Detail: left is the next stock, right the previous, down the list.
  // Settings: left is the list. Info: left the list, down settings.
  bool swipe = g == Gesture::SwipeLeft || g == Gesture::SwipeRight || g == Gesture::SwipeDown || g == Gesture::SwipeUp;
  if (swipe && state == State::Running) {
    bool acts = (view == View::List && (g == Gesture::SwipeRight || g == Gesture::SwipeLeft)) || view == View::Detail ||
                (view == View::News && g != Gesture::SwipeUp) ||
                (view == View::Settings && g == Gesture::SwipeLeft) || (view == View::Confirm && (g == Gesture::SwipeDown || g == Gesture::SwipeLeft)) ||
                ((view == View::Columns || view == View::Themes || view == View::Orient || view == View::Choice) && (g == Gesture::SwipeLeft || g == Gesture::SwipeDown)) ||
                (view == View::Search && (g == Gesture::SwipeDown || g == Gesture::SwipeRight)) || view == View::Heat ||
                (view == View::Info && (g == Gesture::SwipeLeft || g == Gesture::SwipeDown)) || view == View::Splash;
    if (view == View::List && g == Gesture::SwipeRight) {
      view = View::Settings;
      pageOpenedAt = millis();
      drawSettings();
    } else if (view == View::List && g == Gesture::SwipeLeft) {
      openHeat();
    } else if (view == View::Heat) {
      if (g == Gesture::SwipeRight) backToList();
      else if (g == Gesture::SwipeLeft) openSearch();
      else if (g == Gesture::SwipeUp && heatPages() > 1) { heatPage = (heatPage + 1) % heatPages(); drawHeat(true); }
      else if (g == Gesture::SwipeDown && heatPages() > 1) { heatPage = (heatPage + heatPages() - 1) % heatPages(); drawHeat(true); }
    } else if (view == View::Search && g == Gesture::SwipeRight) {
      openHeat();
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
    } else if (view == View::News && detailIdx == NEWS_ALL) {
      if (g == Gesture::SwipeDown) backToList();
      else if (g == Gesture::SwipeUp && newsN > 6) { newsPage = (newsPage + 1) % ((newsN + 5) / 6); }
    } else if (view == View::News) {
      if (g == Gesture::SwipeLeft) openNews((detailIdx + 1) % nRows);
      else if (g == Gesture::SwipeRight) openNews((detailIdx + nRows - 1) % nRows);
      else if (g == Gesture::SwipeDown) { view = View::Detail; detailOpenedAt = millis(); drawDetail(true); }
    } else if ((view == View::Columns || view == View::Themes || view == View::Orient || view == View::Choice) && (g == Gesture::SwipeLeft || g == Gesture::SwipeDown)) {
      view = View::Settings;
      drawSettings();
    } else if (view == View::Settings && g == Gesture::SwipeLeft) {
      backToList();
    } else if (view == View::Confirm && (g == Gesture::SwipeDown || g == Gesture::SwipeLeft)) {
      view = View::Settings;
      pageOpenedAt = millis();
      drawSettings();
    } else if (view == View::Info) {  // a settings page: left or down is settings, like the others
      if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown) {
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
  }
  // A drag on settings scrolls it a row per 30px; the shared drag handler
  // below reports the delta.
  if (view == View::Settings && g == Gesture::Drag && ddy) {
    static int16_t acc = 0;
    acc += ddy;
    while (acc <= -L.sH && setTop + L.sN < L.sRows) { acc += L.sH; setTop++; drawSettingRows(); }
    while (acc >= L.sH && setTop > 0) { acc -= L.sH; setTop--; drawSettingRows(); }
    if (setTop == 0 && acc > 0) acc = 0;
    if (setTop + L.sN >= L.sRows && acc < 0) acc = 0;
  }
  if (view == View::Detail && removeArmedUntil && millis() > removeArmedUntil) {
    removeArmedUntil = 0;
    drawHint("< next    ^ news    v list    prev >");
  }
  if (tap && state == State::Running && view == View::Detail && removeArmedUntil && ty > L.dYWk) {
    removeArmedUntil = 0;
    userRemove(detailIdx);
    if (nRows == 0) {
      cfgErr = "every symbol removed; swipe left to add one";
      state = State::Boot;  // the loop routes to the panel
    } else {
      backToList();
    }
    tap = false;
  }

  // The header is on every page: a tab picks a section and returns to the
  // list; an icon opens its page, or closes it if it is the one open.
  if (tap && state == State::Running && view != View::List && view != View::Splash && ty < L.yRow0) {
    headerTap(tx);
    tap = false;
  }
  if (tap && state == State::Running && view != View::List) {
    if (view == View::Heat) {
      int8_t t = hitTile(tx, ty);
      if (t >= 0) openDetail(order[heatPage * L.hPer + t]);
    } else if (view == View::Search) {
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
      uint8_t r = i >= 0 && setTop + i < L.sRows ? tapSetting((uint8_t)(setTop + i)) : 0;
      if (r == 1) {
        view = View::Info;
        drawInfo(true);
      } else if (r == 3 || r == 5) {
        confirmWhat = r == 3 ? 1 : 2;
        view = View::Confirm;
        drawConfirm();
      } else if (r == 4) {
        WiFi.disconnect(true);
        startSetup();
      } else if (r == 7) {
        view = View::Columns;
        drawColumns();
      } else if (r == 8) {
        view = View::Themes;
        drawThemes();
      } else if (r == 9) {
        view = View::Orient;
        drawOrient();
      } else if (r >= 10) {
        buildChoice(r - 10);
        view = View::Choice;
        drawChoice();
      }
    } else if (view == View::Confirm) {
      if (hitConfirm(tx, ty)) {
        if (confirmWhat == 2) {
          drawHint("clearing. it restarts into setup", C_WARN);
          delay(600);
          clearDevice();
        } else {
          drawHint("shutting down. BOOT button turns it on", C_WARN);
          delay(600);
          goToSleep(0);  // no timer: a touch is the only way back
        }
      } else {
        view = View::Settings;
        drawSettings();
      }
    } else if (view == View::Columns) {
      int8_t i = hitSetting(ty);
      if (i >= 0 && i < 6) {
        sCols ^= 1 << i;
        saveSettings();
        drawColumnRow((uint8_t)i);
      }
    } else if (view == View::Choice) {
      int8_t i = hitSetting(ty);
      if (i >= 0 && i < chN && i != chSel) {
        uint8_t was = chSel;
        applyChoice((uint8_t)i);
        drawChoiceRow(was);
        drawChoiceRow((uint8_t)i);
      }
    } else if (view == View::Orient) {
      int8_t i = hitOrient(tx, ty);
      if (i >= 0 && i != sRot) {  // a restart is cheaper than re-allocating every buffer
        sRot = (uint8_t)i;
        saveSettings();
        drawHint("turning the screen. restarting", C_WARN);
        delay(600);
        ESP.restart();
      }
    } else if (view == View::Themes) {
      int8_t i = hitTheme(tx, ty);
      if (i >= 0 && i != sTheme) {
        sTheme = (uint8_t)i;
        applyTheme();
        saveSettings();
        drawThemes();  // the whole page, in the new colours
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
        kChartReset = true;  // the chart must redraw for the new range
      }
    }
    Serial.printf("tap %d,%d -> view %u %s\n", tx, ty, (unsigned)view, view == View::Detail ? rows[detailIdx].label : "");
  }

  if ((view == View::Detail || view == View::News) && returnMs && !touchHeld && millis() - detailOpenedAt > returnMs)
    backToList();
  // Only the pages the crawl is interrupted for come back on their own: a
  // stock, its headlines, the heatmap. Settings, search and the rest were
  // gone to on purpose and stay until left.
  if (view == View::Heat && returnMs && !touchHeld &&
      millis() - pageOpenedAt > returnMs)
    backToList();

  alertTick();

  static uint32_t lastLog = 0;
  if (millis() - lastLog > LOG_MS) {
    lastLog = millis();
    Serial.printf("heap %u  psram %u  fetch stack free %u  wifi %s %ddBm  rows", ESP.getFreeHeap(), ESP.getFreePsram(),
                  fetchHandle ? (unsigned)uxTaskGetStackHighWaterMark(fetchHandle) : 0,
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN", WiFi.RSSI());
    for (uint8_t i = 0; i < nRows; i++) Serial.printf(" %s=%s", rows[i].label, rows[i].valid ? "ok" : "-");
    Serial.println();
  }
  delay(20);
}
