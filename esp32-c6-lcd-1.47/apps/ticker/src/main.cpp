// ticker: a watchlist panel -- a few stocks and a few crypto, price and daily
// change, green or red.
//
// The watchlist AND every setting live in data/config.json on the device's
// LittleFS partition, not in this file: `./push-config ticker` applies a change
// in about four seconds with no rebuild. See appcfg.h for the shared loader.
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
#include <appcfg.h>
#include <netjoin.h>
#include <ui.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── settings ─────────────────────────────────────────────────────────────
// Everything here is a DEFAULT. data/config.json on the device's LittleFS
// overrides any of it, so tuning the panel is a `./push-config ticker` rather
// than a rebuild. The values below are what you get if the file says nothing.
//
// Reading that file is a trust boundary and appcfg.h handles it: every cfgInt()
// range-checks, logs anything it rejects, and keeps the previous value rather
// than clamping silently. A zero page interval or a zero-length refresh would
// present as a broken app, not as a bad config.
static char tzString[64] = "EST5EDT,M3.2.0/2,M11.1.0/2";

// Market hours, in tzString's zone. Configurable so the timezone above is not
// silently load-bearing: point this at a non-Eastern zone and you must move
// these too, which is exactly the coupling that used to be a buried comment.
static uint16_t mktOpenMin = 9 * 60 + 30;
static uint16_t mktCloseMin = 16 * 60;

static uint32_t pageMs = 8000;   // how long each list page stays up
static uint32_t soloMs = 5000;   // how long one symbol holds the solo screen
static uint16_t cascadeMs = 22;  // per-row stagger when a page flips

static uint8_t layoutDefault = 0;  // 0 = list, 1 = solo; NVS wins once set

// Six rows is the readable budget on a 172px panel at text size 2, and size 1
// is the density already rejected as too small to read. So six is a layout
// fact, asserted in checkLayout() -- but it caps rows ON SCREEN, not symbols:
// a longer list pages through six at a time.
static const uint8_t ROWS_PER_PAGE = 6;
static const uint8_t MAX_SYMBOLS = 32;
static const uint8_t MAX_LABEL = 5;  // glyphs; the symbol box is 5 wide at size 2


// Layouts, cycled by the 2s hold. ui.h calls that gesture UiPress::Scheme
// because most apps here use it for colour; ticker pins one scheme (black with
// green and red, nothing else) and spends the gesture on this instead.
// Add a layout by adding an enum value, a geometry constant, a draw function
// and a checkX() in selfCheck -- nothing else dispatches on it.
enum class Mode : uint8_t { List, Solo };
static const uint8_t MODE_COUNT = 2;
static Mode mode = Mode::List;
static const char *modeName() { return mode == Mode::Solo ? "SOLO" : "LIST"; }

static const int16_t LOGO_PX = 96;  // must match tools/make-logos.py --size

// Backlight duty, overridable from config.json. This is the only dial on
// this board that measurably moves power: measured steady-state die
// temperature is 45.1C at duty 140, 43.5 at 100, 40.5 at 60 and 37.1 at 0, so
// the panel is worth ~8C and the SoC floor is 37C. Duty is proportional to LED
// current, so the saving is real even where the die sensor barely registers it.
//
// Defaults are well below the theme's 140 because this screen is mostly black
// with high-contrast text, which stays legible far dimmer than a light UI.
static uint8_t blOpen = 96;    // market open: the only time it needs to be bright
static uint8_t blClosed = 64;  // shut, but daytime -- crypto still moves
static uint8_t blNight = 24;   // ui.h's night window

// Yahoo 429s an anonymous client, so this is required rather than polite.
static const char *UA = "Mozilla/5.0 (esp32-ticker)";

static uint32_t stockOpenMs = 5UL * 60 * 1000;   // market open: every 5 min
static uint32_t stockShutMs = 60UL * 60 * 1000;  // closed: hourly is plenty
static uint32_t coinMs = 5UL * 60 * 1000;        // crypto never closes
static const uint32_t WIFI_RETRY_MS = 20UL * 1000;
static const uint32_t LOG_MS = 60UL * 1000;

#define POWER_SAVE 1

#define GW(s) (6 * (s))
#define GH(s) (8 * (s))

static Arduino_GFX *gfx;
static Preferences cfg;  // ticker's own namespace; ui.h keeps rotation in its

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

// ticker is the one app on this board whose config is also its DATA: with no
// watchlist there is nothing to show, so a missing config.json is fatal here
// where every other app just falls back to its compiled defaults.
static bool loadConfig() {
  if (!cfgLoad()) {
    cfgErr = cfgError();
    return false;
  }

  // Stocks first, then coins -- addRow() enforces the cap, so an over-long list
  // is truncated rather than overflowing. Say which rows were dropped.
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

  // ── settings ──
  // The floor of 8 on brightness is deliberate: 0 reads as a dead board rather
  // than a dim one, and there is no way back from it without the serial log.
  blOpen = (uint8_t)cfgInt("brightness.open", blOpen, 8, 255);
  blClosed = (uint8_t)cfgInt("brightness.closed", blClosed, 8, 255);
  blNight = (uint8_t)cfgInt("brightness.night", blNight, 8, 255);

  // Night window lives in ui.h, shared with every other app on the board.
  uiNightFrom = (uint8_t)cfgInt("night.from", uiNightFrom, 0, 23);
  uiNightTo = (uint8_t)cfgInt("night.to", uiNightTo, 0, 23);

  pageMs = (uint32_t)cfgInt("timing.pageSeconds", pageMs / 1000, 2, 600) * 1000UL;
  soloMs = (uint32_t)cfgInt("timing.soloSeconds", soloMs / 1000, 2, 600) * 1000UL;
  cascadeMs = (uint16_t)cfgInt("timing.cascadeMs", cascadeMs, 0, 400);

  stockOpenMs = (uint32_t)cfgInt("refresh.openMinutes", stockOpenMs / 60000, 1, 240) * 60000UL;
  stockShutMs = (uint32_t)cfgInt("refresh.closedMinutes", stockShutMs / 60000, 1, 1440) * 60000UL;
  coinMs = (uint32_t)cfgInt("refresh.coinMinutes", coinMs / 60000, 1, 240) * 60000UL;

  cfgStr("timezone", tzString, sizeof tzString);

  cfgHhMm("market.open", &mktOpenMin);
  cfgHhMm("market.close", &mktCloseMin);
  if (mktCloseMin <= mktOpenMin) {
    // An inverted window would make marketOpen() always false, which silently
    // turns off the open brightness and the 5-minute refresh at once.
    Serial.printf("config market: close %u <= open %u, restoring 09:30-16:00\n", mktCloseMin,
                  mktOpenMin);
    mktOpenMin = 9 * 60 + 30;
    mktCloseMin = 16 * 60;
  }

  char lay[8] = "";
  if (cfgStr("layout", lay, sizeof lay)) {
    if (!strcasecmp(lay, "solo")) layoutDefault = 1;
    else if (!strcasecmp(lay, "list")) layoutDefault = 0;
    else Serial.printf("config layout: '%s' is not list or solo, ignored\n", lay);
  }

  Serial.printf("settings: bl %u/%u/%u  night %02u-%02u  page %lus solo %lus cascade %ums\n",
                blOpen, blClosed, blNight, uiNightFrom, uiNightTo,
                (unsigned long)(pageMs / 1000), (unsigned long)(soloMs / 1000), cascadeMs);
  Serial.printf("settings: refresh %lu/%lu/%lu min  market %02u:%02u-%02u:%02u  tz %s\n",
                (unsigned long)(stockOpenMs / 60000), (unsigned long)(stockShutMs / 60000),
                (unsigned long)(coinMs / 60000), mktOpenMin / 60, mktOpenMin % 60,
                mktCloseMin / 60, mktCloseMin % 60, tzString);

  coinIds[0] = '\0';
  for (uint8_t i = 0; i < nRows; i++) {
    if (!rows[i].coin) continue;
    if (coinIds[0]) strlcat(coinIds, ",", sizeof coinIds);
    strlcat(coinIds, rows[i].id, sizeof coinIds);
  }
  nPages = (nRows + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
  Serial.printf("watchlist: %u stocks + %u coins over %u page(s) (%s)\n", nStocks, nCoins,
                nPages, coinIds[0] ? coinIds : "no coins");

  // Inventory the logos at boot rather than discovering a gap when the solo
  // layout reaches that symbol. A missing logo is fine -- the symbol is drawn
  // large instead -- but it should be a line in the log, not a surprise.
  uint8_t haveLogo = 0;
  const size_t want = (size_t)LOGO_PX * LOGO_PX * 2;
  for (uint8_t i = 0; i < nRows; i++) {
    char path[40];
    snprintf(path, sizeof path, "/logo/%s.565", rows[i].label);
    File lf = LittleFS.open(path, "r");
    if (lf && lf.size() == want) haveLogo++;
    else Serial.printf("no logo for %s (text fallback)\n", rows[i].label);
    if (lf) lf.close();
  }
  Serial.printf("logos: %u/%u present\n", haveLogo, nRows);
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

// One symbol filling the screen: logo, ticker, a big price, the change. The
// text block is centred on cx; in landscape the logo sits left of it and the
// footer tucks in underneath the logo, because 172px of height cannot stack a
// logo, three text sizes and three footer lines in one column.
struct SoloLayout {
  int16_t logoX, logoY;
  int16_t cx;  // centre of the text block
  int16_t ySym, yPrice, yPct;
  int16_t yFoot0, footStep;
};

static const SoloLayout PORTRAIT_SOLO = {
    /*logo */ (172 - LOGO_PX) / 2, 22,
    /*cx   */ 86,
    /*text */ 130, 166, 210,
    /*foot */ 264, 15,
};

static const SoloLayout LANDSCAPE_SOLO = {
    /*logo */ 20, 38,
    /*cx   */ 222,
    /*text */ 40, 76, 120,
    /*foot */ 140, 11,
};

static const Layout *L = &PORTRAIT;
static const SoloLayout *S = &PORTRAIT_SOLO;
static void syncLayout() {
  L = uiLandscape() ? &LANDSCAPE : &PORTRAIT;
  S = uiLandscape() ? &LANDSCAPE_SOLO : &PORTRAIT_SOLO;
}

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
  return mins >= mktOpenMin && mins < mktCloseMin;
}

// Installed as ui.h's backlight hook, so uiTick() re-evaluates it every 30s
// and a change takes effect on its own rather than waiting for a button press.
static uint8_t tickerBacklight() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return blClosed;  // no clock yet: assume the quiet case
  if (t.tm_hour >= uiNightFrom || t.tm_hour < uiNightTo) return blNight;
  return marketOpen(t) ? blOpen : blClosed;
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
    if (cascade) delay(cascadeMs);
  }
}

static void turnPage(uint8_t to) {
  page = to;
  invalidateCache();
  if (!uiScreenOn()) return;
  drawChrome();
  drawRows(true);
}

// ── solo layout ──────────────────────────────────────────────────────────
// Logos are pre-converted to raw RGB565 by tools/make-logos.py and shipped on
// LittleFS, so the firmware carries no PNG decoder, opens no second endpoint
// and needs no decode buffer. A missing file is an ordinary case, not an
// error: the symbol is simply drawn large instead.
static uint16_t *logoBuf = nullptr;
static char logoFor[MAX_LABEL + 2] = "";
static bool logoOk = false;

static bool loadLogo(const char *label) {
  if (strcmp(label, logoFor) == 0) return logoOk;  // already in the buffer
  snprintf(logoFor, sizeof logoFor, "%s", label);
  logoOk = false;

  const size_t want = (size_t)LOGO_PX * LOGO_PX * 2;
  if (!logoBuf) {
    logoBuf = (uint16_t *)malloc(want);  // 18KB of ~305KB, allocated once
    if (!logoBuf) {
      Serial.println("logo: malloc failed");
      return false;
    }
  }
  char path[40];
  snprintf(path, sizeof path, "/logo/%s.565", label);
  File f = LittleFS.open(path, "r");
  if (!f) return false;  // no logo for this symbol; caller falls back to text
  // Check the length before trusting it: a truncated upload would otherwise
  // blit whatever was left in the buffer from the previous symbol.
  if (f.size() != want) {
    Serial.printf("logo %s: %u bytes, want %u -- rerun tools/make-logos.py\n", label,
                  (unsigned)f.size(), (unsigned)want);
    f.close();
    return false;
  }
  logoOk = f.read((uint8_t *)logoBuf, want) == want;
  f.close();
  return logoOk;
}

static void fieldCentre(int16_t cx, int16_t y, uint8_t maxChars, uint8_t size, uint16_t fg,
                        const char *s) {
  int16_t boxW = GW(size) * maxChars;
  gfx->fillRect(cx - boxW / 2, y, boxW, GH(size), uiTheme()->bg);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(cx - GW(size) * (int16_t)strlen(s) / 2, y);
  gfx->print(s);
}

static uint8_t soloIdx = 0;
static char cSolo[40];

static void drawSolo(bool full) {
  if (full) {
    gfx->fillScreen(uiTheme()->bg);
    cSolo[0] = '\0';
    logoFor[0] = '\0';  // force the blit; fillScreen just erased it
  }
  Row &r = rows[soloIdx];

  char price[12], pct[12], key[40];
  if (r.valid) {
    formatPrice(r.price, price, sizeof price);
    formatPct(r.pct, pct, sizeof pct);
  } else {
    snprintf(price, sizeof price, "--");
    pct[0] = '\0';
  }
  snprintf(key, sizeof key, "%s|%s|%s", r.label, price, pct);
  if (strcmp(key, cSolo) == 0) return;
  strcpy(cSolo, key);

  if (loadLogo(r.label))
    gfx->draw16bitRGBBitmap(S->logoX, S->logoY, logoBuf, LOGO_PX, LOGO_PX);
  else
    gfx->fillRect(S->logoX, S->logoY, LOGO_PX, LOGO_PX, uiTheme()->bg);

  // Symbol white, price and change green or red -- the only colours this app
  // uses, and the percent stays signed so colour is never the sole cue.
  uint16_t fg = !r.valid ? uiTheme()->dim : r.pct >= 0 ? uiTheme()->good : uiTheme()->bad;
  fieldCentre(S->cx, S->ySym, MAX_LABEL, 3, uiTheme()->fg, r.label);
  fieldCentre(S->cx, S->yPrice, 7, 4, fg, price);
  fieldCentre(S->cx, S->yPct, 7, 2, fg, pct);
}

static void drawHead(const struct tm *t, bool haveTime) {
  char buf[20];
  if (haveTime) strftime(buf, sizeof buf, "%H:%M", t);
  else snprintf(buf, sizeof buf, "--:--");
  if (strcmp(buf, cHead) == 0) return;
  strcpy(cHead, buf);
  fieldRight(L->w - 8, L->yHead, 5, 1, uiTheme()->muted, buf);
}

// y0/step are passed in rather than read from L, because the two layouts put
// the footer in different places -- solo has to tuck it under the logo.
static void drawFooter(const struct tm *t, bool haveTime, int16_t y0, int16_t step) {
  char buf[40];
  bool open = haveTime && marketOpen(*t);

  snprintf(buf, sizeof buf, "market %s", !haveTime ? "?" : open ? "open" : "closed");
  if (strcmp(buf, cFoot[0]) != 0) {
    strcpy(cFoot[0], buf);
    field(8, y0, 20, 1, open ? uiTheme()->good : uiTheme()->muted, buf);
  }
  // Say it is delayed rather than implying live prices.
  snprintf(buf, sizeof buf, "delayed, %lum ago",
           lastOk ? (unsigned long)((millis() - lastOk) / 60000) : 0UL);
  if (strcmp(buf, cFoot[1]) != 0) {
    strcpy(cFoot[1], buf);
    field(8, y0 + step, 20, 1, uiTheme()->dim, buf);
  }
  snprintf(buf, sizeof buf, "%.9s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  if (strcmp(buf, cFoot[2]) != 0) {
    strcpy(cFoot[2], buf);
    field(8, y0 + step * 2, 20, 1, uiTheme()->muted, buf);
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

// Solo puts a 96px logo and three text sizes on one screen, which is the
// layout most likely to overflow -- and the 172px-tall landscape case is the
// tight one. Every element is checked against the panel and against the
// element below it.
static void checkSolo(const SoloLayout *s, int16_t w, int16_t h) {
  assert(s->logoX >= 0 && s->logoX + LOGO_PX <= w);
  assert(s->logoY >= 0 && s->logoY + LOGO_PX <= h);

  // Widest string each field can hold, centred on cx, must stay on screen.
  assert(s->cx - GW(3) * MAX_LABEL / 2 >= 0 && s->cx + GW(3) * MAX_LABEL / 2 <= w);
  assert(s->cx - GW(4) * 7 / 2 >= 0 && s->cx + GW(4) * 7 / 2 <= w);
  assert(s->cx - GW(2) * 7 / 2 >= 0 && s->cx + GW(2) * 7 / 2 <= w);

  // Vertical stack: symbol, price, change, then the footer.
  assert(s->ySym + GH(3) <= s->yPrice);
  assert(s->yPrice + GH(4) <= s->yPct);
  assert(s->yPct + GH(2) <= s->yFoot0);
  assert(s->yFoot0 + s->footStep * 2 + GH(1) <= h);

  // The logo must not land on the text. In portrait it sits above it; in
  // landscape it sits to the left, so one of the two has to hold.
  bool above = s->logoY + LOGO_PX <= s->ySym;
  bool beside = s->logoX + LOGO_PX <= s->cx - GW(4) * 7 / 2;
  assert(above || beside);
  // ...and the footer goes under whichever it is.
  assert(s->yFoot0 >= s->logoY + LOGO_PX || above);
}

static void selfCheck() {
  cfgSelfCheck();  // the shared config accessors, including HH:MM parsing
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

  // Market hours. tm_wday: 0 = Sunday. These assertions read the live
  // mktOpenMin/mktCloseMin, so state plainly that selfCheck() runs before
  // loadConfig() and is therefore testing the compiled defaults -- if that
  // order ever changes, this assert fails rather than the test going vacuous.
  assert(mktOpenMin == 9 * 60 + 30 && mktCloseMin == 16 * 60);
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
  assert(nRows == 0);  // runs before loadConfig()
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
  checkSolo(&PORTRAIT_SOLO, 172, 320);
  checkSolo(&LANDSCAPE_SOLO, 320, 172);
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

  // One scheme only: black background, white symbols, green and red numbers.
  // uiBegin() restores whatever was last persisted, so pin it every boot --
  // persist=false keeps this from becoming an NVS write on every power-up.
  uiApply(uiRot(), 0, false);
  uiHoldSchemeLabel = "release: LAYOUT";

  cfg.begin("tickercfg", false);
  syncLayout();  // depends only on rotation, so it is valid before the settings

  // The hook reads blOpen/blClosed/blNight at call time, so installing it
  // before loadConfig() overrides them is fine. It cannot be *applied* yet
  // though: it needs the clock to know whether the market is open, so the
  // actual level is set at the end of setup(), after NTP.
  uiBacklightHook = tickerBacklight;

  // Must come before the mode and the timezone are used: this is what reads
  // every setting, including layoutDefault and tzString.
  if (!loadConfig()) {
    Serial.printf("config error: %s %s\n", cfgErr, cfgErrDetail);
    return;  // loop() draws the panel; nothing else can usefully run
  }

  cfgRelease();  // settings are copied out; the parsed tree is several KB

  // NVS wins over the file's `layout`, which is only a starting point: once the
  // button has been held, that choice is the user's and a config default must
  // not silently undo it on the next boot.
  mode = (Mode)(cfg.getUChar("mode", layoutDefault) % MODE_COUNT);
  Serial.printf("layout: %s (file default %s, %s)\n", modeName(),
                layoutDefault ? "solo" : "list",
                cfg.isKey("mode") ? "nvs override in effect" : "no nvs override");

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
    configTzTime(tzString, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    for (int i = 0; i < 40 && !getLocalTime(&t, 250); i++) {}
  }
#if POWER_SAVE
  // 80MHz is the floor, not a choice: the Wi-Fi stack requires at least this.
  setCpuFrequencyMhz(80);

  // Transmit power is set to 19.5dBm by netTune(), which exists because a weak
  // spot once failed to associate at all. This board sits at -50dBm, so most of
  // that is margin being spent as heat -- but only trim it when the measured
  // signal actually says there is margin, and leave it alone when there isn't.
  // Small next to the backlight, and not separately measurable with a die
  // thermometer, but it costs nothing and cannot hurt at this range.
  if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() > -65) {
    WiFi.setTxPower(WIFI_POWER_13dBm);
    Serial.printf("tx power -> 13dBm (rssi %ddBm, ample margin)\n", WiFi.RSSI());
  }
#endif

  // Now that NTP has run, tickerBacklight() can tell open from closed.
  uiBacklightApplied = uiBacklightNow();
  backlight(uiBacklightApplied);
  Serial.printf("backlight -> %u\n", uiBacklightApplied);
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
  // Claim the 2s hold for the layout before delegating -- ui.h's own comment
  // establishes this pattern for Setup. Everything else (tap to rotate, long
  // hold to blank) still belongs to uiHandle().
  UiPress p = uiPoll();
  if (p == UiPress::Scheme && uiScreenOn()) {
    mode = (Mode)(((uint8_t)mode + 1) % MODE_COUNT);
    cfg.putUChar("mode", (uint8_t)mode);
    soloIdx = 0;
    invalidateCache();
    state = State::Boot;
    Serial.printf("layout -> %s\n", modeName());
  } else if (uiHandle(p)) {
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
    if (uiScreenOn() && state == State::Running) {
      if (mode == Mode::List) drawChrome();
      else drawSolo(true);
    }
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
    if (state != State::Running) {
      drawFailPanel();
    } else if (mode == Mode::List) {
      drawHead(&t, haveTime);
      drawRows();
      drawFooter(&t, haveTime, L->yFoot0, L->footStep);
    } else {
      drawSolo(false);
      drawFooter(&t, haveTime, S->yFoot0, S->footStep);
    }
  }

  // Each layout advances on its own clock: the list turns a page, solo moves to
  // the next symbol.
  static uint32_t lastAdvance = 0;
  if (state == State::Running && uiScreenOn()) {
    if (mode == Mode::List && nPages > 1 && millis() - lastAdvance > pageMs) {
      lastAdvance = millis();
      turnPage((page + 1) % nPages);
    } else if (mode == Mode::Solo && nRows > 1 && millis() - lastAdvance > soloMs) {
      lastAdvance = millis();
      soloIdx = (soloIdx + 1) % nRows;
      // Not a full repaint: fieldCentre clears its own box and the logo
      // overwrites its own square, so a black flash every 5s is avoidable.
      drawSolo(false);
    }
  }

  // Crypto and stocks on separate clocks: one market closes, the other never
  // does, and polling a shut exchange every five minutes only risks the 429.
  // Nothing is fetched while the screen is blanked. Twelve TLS handshakes and
  // twelve HTTP GETs every few minutes for a panel nobody is looking at is the
  // clearest waste in the app, and it needs no catch-up logic: the interval
  // timers keep running, so the first loop after a wake is already overdue and
  // refetches immediately.
  // Stocks are swept ONE PER LOOP PASS, never in a burst.
  //
  // Measured: Yahoo served 22 sequential requests spaced 0.3s apart without a
  // single 429, so the old one-request-per-minute floor was far too cautious --
  // the original 429 was the missing User-Agent, not the rate. But 22 blocking
  // requests back to back freeze loop() for ~26s, and while the ISR does latch
  // a button press so nothing is lost, the clock stops and the press is acted
  // on up to half a minute late. One per pass keeps loop() alive, finishes the
  // sweep in ~30s anyway, and fills the rows in progressively.
  static bool sweeping = false;
  static uint8_t sweepIdx = 0;
  static uint32_t lastOne = 0;

  if (state != State::NoWifi && uiScreenOn()) {
    uint32_t stockEvery = (haveTime && marketOpen(t)) ? stockOpenMs : stockShutMs;
    // A guard against a pathological list rather than a rate limit: keep the
    // sweep under about a quarter of the interval. At 22 stocks this works out
    // to 110s, well inside the 5-minute interval, so it never actually bites.
    if (stockEvery < nStocks * 5000UL) stockEvery = nStocks * 5000UL;

    if (!sweeping && nStocks && (lastStock == 0 || millis() - lastStock > stockEvery)) {
      sweeping = true;
      sweepIdx = 0;
    }

    auto redraw = [&]() {
      if (state != State::Running) return;
      if (mode == Mode::List) drawRows();
      else drawSolo(false);
    };

    if (nCoins && (lastCoin == 0 || millis() - lastCoin > coinMs)) {
      lastCoin = millis();
      NetworkClientSecure client;
      client.setInsecure();  // public read-only quotes; pinning buys nothing here
      bool ok = fetchCoins(client);
      if (ok) {
        lastOk = millis();
        failures = 0;
        redraw();
      } else {
        failures++;
      }
      Serial.printf("coins %s (failures %u, heap %u)\n", ok ? "ok" : "FAILED", failures,
                    ESP.getFreeHeap());
    } else if (sweeping && millis() - lastOne > 250) {
      lastOne = millis();
      if (sweepIdx >= nStocks) {  // stocks are stored first, so this is the end
        sweeping = false;
        lastStock = millis();
        Serial.printf("sweep done (%u stocks, heap %u)\n", nStocks, ESP.getFreeHeap());
      } else {
        NetworkClientSecure client;
        client.setInsecure();
        if (fetchStock(client, rows[sweepIdx])) {
          lastOk = millis();
          failures = 0;
          redraw();
        } else {
          failures++;
        }
        sweepIdx++;
      }
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

  // 50ms rather than 20: the button is edge-captured in an ISR so nothing is
  // missed, the clock only changes once a minute, and delay() yields to the
  // idle task which parks the core. Small, but free.
  delay(50);
}
