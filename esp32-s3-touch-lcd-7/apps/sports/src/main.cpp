// sports -- a scores board for the Waveshare ESP32-S3-Touch-LCD-7.
//
// Every game today across the leagues in config.json, from ESPN's own site
// scoreboard (site.web.api.espn.com/apis/v2/scoreboard/header, keyless; the
// public api host refuses, this one answers): live first with the clock,
// then today's fixtures by start time, then finals; favourite teams first
// in each. A tab per league, a page per game. The Wi-Fi network is the one
// the ticker saved (NVS "ticker": ssid/pass), so one setup serves both.
//
// ponytail: the text, header, gesture and fetch helpers are the ticker's,
// copied; hoist them into lib/ui.h when the third app wants them.
#include <appcfg.h>
#include <board.h>
#include <helv.h>
#include <netjoin.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

// ── colours, faces ───────────────────────────────────────────────────────
static const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_GOOD = 0x07E0, C_BAD = 0xF800, C_DIM = 0x630C, C_MUTED = 0xA534,
                      C_RULE = 0x2104, C_GOLD = 0xFD40, C_WARN = 0xFFE0;
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
struct Face { const uint8_t *font; uint8_t cap, desc; };
static const Face FACES[] = {{u8g2_font_helvR14_tr, 14, 4}, {u8g2_font_helvB18_tr, 19, 5}, {u8g2_font_helvB24_tr, 25, 7},
                             {u8g2_font_logisoso50_tn, 50, 13}, {u8g2_font_logisoso58_tr, 58, 15}};
static const uint8_t GWS[] = {8, 11, 14, 28, 34};
#define GW(s) (GWS[(s) - 1])
#define GH(s) (FACES[(s) - 1].cap + FACES[(s) - 1].desc)
static Arduino_GFX *gfx;
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ── layout (landscape) ───────────────────────────────────────────────────
static const int16_t Y_ROW0 = 40, ROW_H = 52, Y_HINT = 452, X_RIGHT = 788;
static const uint8_t ROWS = (Y_HINT - 8 - Y_ROW0) / ROW_H;  // 7

// ── text ─────────────────────────────────────────────────────────────────
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
  gfx->fillRect(right - w, y, min<int16_t>(w + 6, LCD_W - (right - w)), GH(size), C_BG);
  textAt(right - textWidth(size, s), y, size, fg, s);
}
static void fieldCentre(int16_t cx, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(cx - w / 2, y, w, GH(size), C_BG);
  textAt(cx - textWidth(size, s) / 2, y, size, fg, s);
}
static void drawHint(const char *s, uint16_t c = C_DIM) { fieldCentre(LCD_W / 2, Y_HINT, 80, 1, c, s); }
static void backMark(int16_t left, int16_t cy, uint16_t c) {
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(left + 8 + d, cy - 8, left + d, cy, c);
    gfx->drawLine(left + d, cy, left + 8 + d, cy + 8, c);
  }
}

// ── logos: /logo/32/ABBR.565 and /logo/128/ABBR.565, cached in PSRAM ──────
struct LogoCache { uint8_t size; char label[16]; uint16_t *px; };
static LogoCache logoCache[96];
static uint8_t nLogoCache = 0;
static uint16_t *logoLoad(uint8_t size, const char *label) {
  for (uint8_t i = 0; i < nLogoCache; i++)
    if (logoCache[i].size == size && strcmp(logoCache[i].label, label) == 0) return logoCache[i].px;
  if (nLogoCache >= 96) return nullptr;
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
// A team's mark at x,y: its logo (/logo/N/<league>_<ABBR>.565, made by
// tools/make-team-logos.py; TOR is a different team in the NHL and the NBA),
// or a tile with its letters.
static void teamMark(int16_t x, int16_t y, uint8_t size, const char *lg, const char *abbr, uint16_t c) {
  char key[20];
  snprintf(key, sizeof key, "%s_%s", lg, abbr);
  if (blitLogo(x, y, size, key)) return;
  gfx->fillRoundRect(x, y, size, size, size / 5, C_RULE);
  uint8_t f = size >= 96 ? 3 : 1;
  textAt(x + (size - textWidth(f, abbr)) / 2, y + (size - FACES[f - 1].cap) / 2, f, c, abbr);
}

// ── the leagues and the games ────────────────────────────────────────────
struct League { char id[10], sport[12], label[6]; };
static const uint8_t MAX_LEAGUES = 6, MAX_GAMES = 64;
static League leagues[MAX_LEAGUES];
static uint8_t nLeagues = 0;
static char favs[12][6];
static uint8_t nFavs = 0;
static uint32_t liveMs = 60000, idleMs = 600000;
static char tzString[48] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static char wifiSsid[33] = "", wifiPass[65] = "";

struct Game {
  uint8_t lg, state;  // state: 0 pre, 1 in, 2 post
  char away[6], home[6], awayName[24], homeName[24], aScore[6], hScore[6];
  char summary[28], clock[10], broadcast[20];
  uint8_t period;
  bool awayWin, homeWin, fav;
  time_t start;
};
static Game games[MAX_GAMES];  // all leagues, in order; the fetch task rebuilds, the UI reads, under mux
static uint8_t nGames = 0;
static uint32_t gamesAt[MAX_LEAGUES], gamesVer = 0;
static bool lgFailed[MAX_LEAGUES];
static SemaphoreHandle_t mux;

static bool isFav(const char *abbr) {
  for (uint8_t i = 0; i < nFavs; i++)
    if (strcmp(favs[i], abbr) == 0) return true;
  return false;
}
// Days since the epoch for a civil date (Howard Hinnant), for "2026-09-20T17:00:00Z".
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
static time_t parseIso(const char *s) {
  int y, mo, d, H, M, S = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &H, &M, &S) < 5) return 0;
  return (time_t)daysFromCivil(y, mo, d) * 86400 + H * 3600 + M * 60 + S;
}
// live first, then fixtures by start, then finals; favourites first in each
static int gameOrder(const Game &a, const Game &b) {
  static const uint8_t rank[3] = {1, 0, 2};
  if (rank[a.state] != rank[b.state]) return rank[a.state] - rank[b.state];
  if (a.fav != b.fav) return a.fav ? -1 : 1;
  if (a.start != b.start) return a.start < b.start ? -1 : 1;
  return 0;
}

// ── fetching (core 0 task) ───────────────────────────────────────────────
static const char *UA = "Mozilla/5.0 (esp32-sports)";
static const size_t BUF_CAP = 640 * 1024;  // a league's header feed is 70-260KB; PSRAM has room
static uint8_t *buf = nullptr;
static int fetchBytes(NetworkClientSecure &client, const char *url, uint8_t *out, size_t cap, const char *tag) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return -1;
  http.addHeader("User-Agent", UA);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d\n", tag, code);
    http.end();
    return -1;
  }
  size_t len = 0;
  NetworkClient *st = http.getStreamPtr();
  uint32_t last = millis();
  while (millis() - last < 8000 && len < cap) {
    int n = st->available();
    if (n > 0) {
      n = st->read(out + len, min((size_t)n, cap - len));
      if (n > 0) {
        len += n;
        last = millis();
      }
    } else if (!st->connected()) {
      break;
    } else {
      delay(5);
    }
  }
  http.end();
  return (int)len;
}
static void fetchLeague(uint8_t li) {
  char url[160];
  snprintf(url, sizeof url, "https://site.web.api.espn.com/apis/v2/scoreboard/header?sport=%s&league=%s", leagues[li].sport,
           leagues[li].id);
  NetworkClientSecure client;
  client.setInsecure();
  int len = fetchBytes(client, url, buf, BUF_CAP - 1, leagues[li].label);
  static Game got[MAX_GAMES];
  uint8_t n = 0;
  bool ok = false;
  if (len > 0) {
    JsonDocument filter, doc;
    JsonObject ev = filter["sports"][0]["leagues"][0]["events"][0].to<JsonObject>();
    for (const char *k : {"status", "summary", "period", "clock", "date", "broadcast"}) ev[k] = true;
    JsonObject comp = ev["competitors"][0].to<JsonObject>();
    for (const char *k : {"abbreviation", "displayName", "score", "winner", "homeAway"}) comp[k] = true;
    DeserializationError je = deserializeJson(doc, (const char *)buf, (size_t)len, DeserializationOption::Filter(filter),
                                              DeserializationOption::NestingLimit(40));  // the feed nests past the default ten
    if (je) {
      Serial.printf("%s: json %s (%d bytes)\n", leagues[li].label, je.c_str(), len);
    } else {
      ok = true;
      for (JsonObject e : doc["sports"][0]["leagues"][0]["events"].as<JsonArray>()) {
        if (n >= MAX_GAMES) break;
        Game &g = got[n];
        memset(&g, 0, sizeof g);
        g.lg = li;
        const char *st = e["status"] | "pre";
        g.state = strcmp(st, "in") == 0 ? 1 : strcmp(st, "post") == 0 ? 2 : 0;
        snprintf(g.summary, sizeof g.summary, "%s", e["summary"] | "");
        snprintf(g.clock, sizeof g.clock, "%s", e["clock"] | "");
        snprintf(g.broadcast, sizeof g.broadcast, "%s", e["broadcast"] | "");
        g.period = e["period"] | 0;
        g.start = parseIso(e["date"] | "");
        for (JsonObject c : e["competitors"].as<JsonArray>()) {
          bool home = strcmp(c["homeAway"] | "", "home") == 0;
          char *ab = home ? g.home : g.away, *nm = home ? g.homeName : g.awayName, *sc = home ? g.hScore : g.aScore;
          snprintf(ab, 6, "%s", c["abbreviation"] | "?");
          snprintf(nm, 24, "%s", c["displayName"] | "");
          snprintf(sc, 6, "%s", c["score"] | "");
          if (home) g.homeWin = c["winner"] | false;
          else g.awayWin = c["winner"] | false;
        }
        if (!g.away[0] || !g.home[0]) continue;
        g.fav = isFav(g.away) || isFav(g.home);
        n++;
      }
    }
  }
  // Rebuild the shared list: the other leagues' games kept, this one's replaced, then sorted.
  xSemaphoreTake(mux, portMAX_DELAY);
  uint8_t m = 0;
  static Game merged[MAX_GAMES];
  for (uint8_t i = 0; i < nGames && m < MAX_GAMES; i++)
    if (games[i].lg != li) merged[m++] = games[i];
  for (uint8_t i = 0; i < n && m < MAX_GAMES; i++) merged[m++] = got[i];
  for (uint8_t i = 1; i < m; i++) {  // insertion sort by gameOrder
    Game k = merged[i];
    int8_t j = i - 1;
    while (j >= 0 && gameOrder(merged[j], k) > 0) { merged[j + 1] = merged[j]; j--; }
    merged[j + 1] = k;
  }
  memcpy(games, merged, sizeof(Game) * m);
  nGames = m;
  gamesAt[li] = millis();
  lgFailed[li] = !ok;
  gamesVer++;
  xSemaphoreGive(mux);
  Serial.printf("%s: %u games%s (%d bytes, heap %u)\n", leagues[li].label, n, ok ? "" : " FAILED", len, ESP.getFreeHeap());
}
static bool leagueLive(uint8_t li) {
  for (uint8_t i = 0; i < nGames; i++)
    if (games[i].lg == li && games[i].state == 1) return true;
  return false;
}
static void fetchTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (WiFi.status() != WL_CONNECTED || !buf) continue;
    for (uint8_t li = 0; li < nLeagues; li++) {  // the stalest league that is due
      uint32_t every = leagueLive(li) ? liveMs : idleMs;
      if (gamesAt[li] == 0 || millis() - gamesAt[li] > every) {
        fetchLeague(li);
        break;  // one a pass, so the UI stays live between
      }
    }
  }
}

// ── touch ────────────────────────────────────────────────────────────────
static bool touchHeld = false;
enum class Gesture : uint8_t { None, Tap, TapUp, LongPress, Drag, SwipeRight, SwipeLeft, SwipeUp, SwipeDown };
static const uint32_t TAP_MS = 100, LONG_MS = 700;
static const int16_t AXIS_PX = 24, SWIPE_PX = 50;
static bool tapUpQuick = false;
static Gesture pollGesture(int16_t *x, int16_t *y, int16_t *dy) {
  static int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
  static uint32_t t0 = 0, lastTap = 0;
  static uint8_t axis = 0, gap = 0;
  static bool tapped = false, longed = false, dragging = false;
  int16_t cx, cy;
  bool contact = touchRead(&cx, &cy);
  if (contact) gap = 0;
  else if (touchHeld && ++gap < 3) return Gesture::None;
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
  } else if (touchHeld) {
    int16_t ddx = lx - x0, ddy = ly - y0;
    if (axis == 1) {
      if (ddx >= SWIPE_PX) g = Gesture::SwipeRight;
      else if (ddx <= -SWIPE_PX) g = Gesture::SwipeLeft;
    } else if (axis == 2) {
      if (ddy >= SWIPE_PX) g = Gesture::SwipeDown;
      else if (ddy <= -SWIPE_PX) g = Gesture::SwipeUp;
    } else {
      tapUpQuick = !tapped;
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::TapUp;
    }
  }
  touchHeld = now;
  return g;
}

// ── pages ────────────────────────────────────────────────────────────────
enum class View { List, Game };
static View view = View::List;
static uint8_t sect = 0;  // 0 ALL, else league index + 1
static uint8_t listTop = 0, gameIdx = 0;
static char cHead[48], cRows[ROWS][64];
static int16_t tabX[MAX_LEAGUES + 1], tabW[MAX_LEAGUES + 1];
static uint8_t order[MAX_GAMES], nShown = 0;
static Game shown[MAX_GAMES];  // the UI's copy, taken under mux when the version moves
static uint32_t shownVer = 0;

static void takeGames() {
  xSemaphoreTake(mux, portMAX_DELAY);
  memcpy(shown, games, sizeof(Game) * nGames);
  uint8_t n = nGames;
  shownVer = gamesVer;
  xSemaphoreGive(mux);
  nShown = 0;
  for (uint8_t i = 0; i < n; i++)
    if (sect == 0 || shown[i].lg == sect - 1) order[nShown++] = i;
}
static void drawTabs() {
  gfx->fillRect(0, 0, 560, Y_ROW0 - 1, C_BG);
  int16_t x = 12;
  for (uint8_t i = 0; i <= nLeagues; i++) {
    const char *name = i == 0 ? "ALL" : leagues[i - 1].label;
    int16_t w = textWidth(1, name) + 26;
    tabX[i] = x;
    tabW[i] = w;
    bool on = i == sect;
    textAt(x + 13, 12, 1, on ? C_FG : C_DIM, name);
    if (on) gfx->fillRect(x + 9, 34, w - 18, 3, C_FG);
    x += w;
  }
  gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
}
static int8_t hitTab(int16_t x) {
  for (uint8_t i = 0; i <= nLeagues; i++)
    if (x >= tabX[i] && x < tabX[i] + tabW[i]) return i;
  return -1;
}
static void drawClock() {
  struct tm t;
  char clk[12] = "";
  if (getLocalTime(&t, 0)) snprintf(clk, sizeof clk, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  if (strcmp(clk, cHead) == 0) return;
  strcpy(cHead, clk);
  fieldRight(X_RIGHT, 12, 8, 1, C_MUTED, clk);
}
// When a fixture starts, in the local zone: "7:00 PM" today, else "Sat 1:00 PM".
static void whenStr(time_t start, char *out, size_t n) {
  if (!start) { out[0] = '\0'; return; }
  struct tm t, now;
  localtime_r(&start, &t);
  time_t nt = time(nullptr);
  localtime_r(&nt, &now);
  bool today = nt > 1000000000L && t.tm_yday == now.tm_yday && t.tm_year == now.tm_year;
  char hm[12];
  snprintf(hm, sizeof hm, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  if (today) snprintf(out, n, "%s", hm);
  else {
    char day[8];
    strftime(day, sizeof day, "%a", &t);
    snprintf(out, n, "%s %s", day, hm);
  }
}
// One row: away mark, letters and score; home the same; the state on the right.
static void drawRow(uint8_t slot, bool force) {
  int16_t y = Y_ROW0 + slot * ROW_H;
  uint8_t i = listTop + slot;
  char key[64] = "";
  if (i < nShown) {
    const Game &g = shown[order[i]];
    snprintf(key, sizeof key, "%s|%s|%s|%s|%s|%u|%d", g.away, g.aScore, g.home, g.hScore, g.summary, g.state, g.fav);
  }
  if (!force && strcmp(key, cRows[slot]) == 0) return;
  strcpy(cRows[slot], key);
  gfx->fillRect(0, y, LCD_W, ROW_H, C_BG);
  if (i >= nShown) return;
  const Game &g = shown[order[i]];
  bool live = g.state == 1, done = g.state == 2;
  if (g.fav) gfx->fillRect(6, y + 10, 4, 32, C_GOLD);
  uint16_t ac = done && !g.awayWin ? C_MUTED : C_FG, hc = done && !g.homeWin ? C_MUTED : C_FG;
  teamMark(20, y + 10, 32, leagues[g.lg].id, g.away, ac);
  textAt(62, y + 15, 2, ac, g.away);
  if (g.aScore[0]) textAt(200 - textWidth(2, g.aScore), y + 15, 2, ac, g.aScore);
  textAt(228, y + 17, 1, C_DIM, "@");
  teamMark(260, y + 10, 32, leagues[g.lg].id, g.home, hc);
  textAt(302, y + 15, 2, hc, g.home);
  if (g.hScore[0]) textAt(440 - textWidth(2, g.hScore), y + 15, 2, hc, g.hScore);
  if (sect == 0) textAt(480, y + 17, 1, C_DIM, leagues[g.lg].label);
  char st[28];
  if (live) snprintf(st, sizeof st, "%s", g.summary[0] ? g.summary : "live");
  else if (done) snprintf(st, sizeof st, "%s", g.summary[0] ? g.summary : "Final");
  else whenStr(g.start, st, sizeof st);
  textAt(X_RIGHT - textWidth(2, st), y + 15, 2, live ? C_GOOD : C_MUTED, st);
  if (g.broadcast[0] && !done) textAt(X_RIGHT - textWidth(1, g.broadcast), y + 36, 1, C_DIM, g.broadcast);
  gfx->drawFastHLine(20, y + ROW_H - 1, LCD_W - 40, C_RULE);
}
static void drawList(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    drawTabs();
    cHead[0] = '\0';
    for (uint8_t s = 0; s < ROWS; s++) cRows[s][0] = '\0';
    drawHint(nLeagues ? "drag for more        tap a game" : "config.json has no leagues");
  }
  drawClock();
  if (shownVer != gamesVer) takeGames();
  if (listTop + ROWS > nShown) listTop = nShown > ROWS ? nShown - ROWS : 0;
  for (uint8_t s = 0; s < ROWS; s++) drawRow(s, false);
  static char cEmpty[32];
  char e[32] = "";
  if (!nShown) {
    bool any = false, failed = false;
    for (uint8_t li = 0; li < nLeagues; li++) { any |= gamesAt[li] != 0; failed |= lgFailed[li]; }
    snprintf(e, sizeof e, "%s", !any ? "fetching scores..." : failed ? "no scores (fetch failed)" : "no games today");
  }
  if (strcmp(e, cEmpty) != 0) {
    strcpy(cEmpty, e);
    fieldCentre(LCD_W / 2, 220, 30, 2, C_DIM, e);
  }
}
static int16_t hitRow(int16_t y) {
  if (y < Y_ROW0 || y >= Y_ROW0 + ROWS * ROW_H) return -1;
  uint8_t s = (y - Y_ROW0) / ROW_H;
  return listTop + s < nShown ? listTop + s : -1;
}
// The game's page: the two marks large, the scores in the tall digits, the state between.
static void drawGame(bool full) {
  static char kGame[96];
  if (gameIdx >= nShown) { view = View::List; drawList(true); return; }
  const Game &g = shown[order[gameIdx]];
  if (full) {
    gfx->fillScreen(C_BG);
    backMark(14, 22, C_MUTED);
    textAt(36, 10, 2, C_FG, leagues[g.lg].label);
    char when[24];
    whenStr(g.start, when, sizeof when);
    textAt(36 + textWidth(2, leagues[g.lg].label) + 14, 14, 1, C_MUTED, when);
    gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
    drawHint("< next        v scores        prev >");
    kGame[0] = '\0';
  }
  drawClock();
  char key[96];
  snprintf(key, sizeof key, "%s|%s|%s|%s|%s|%u", g.away, g.aScore, g.home, g.hScore, g.summary, g.state);
  if (strcmp(key, kGame) == 0) return;
  strcpy(kGame, key);
  gfx->fillRect(0, Y_ROW0, LCD_W, Y_HINT - 8 - Y_ROW0, C_BG);
  bool live = g.state == 1, done = g.state == 2;
  uint16_t ac = done && !g.awayWin ? C_MUTED : C_FG, hc = done && !g.homeWin ? C_MUTED : C_FG;
  // away on the left, home on the right, the state in the middle
  teamMark(60, 80, 128, leagues[g.lg].id, g.away, ac);
  fieldCentre(124, 222, 12, 2, ac, g.away);
  fieldCentre(124, 252, 24, 1, C_MUTED, g.awayName);
  teamMark(612, 80, 128, leagues[g.lg].id, g.home, hc);
  fieldCentre(676, 222, 12, 2, hc, g.home);
  fieldCentre(676, 252, 24, 1, C_MUTED, g.homeName);
  if (g.aScore[0] || g.hScore[0]) {
    textAt(300 - textWidth(4, g.aScore[0] ? g.aScore : "0"), 96, 4, ac, g.aScore[0] ? g.aScore : "0");
    textAt(500, 96, 4, hc, g.hScore[0] ? g.hScore : "0");
    textAt(400 - textWidth(3, "-") / 2, 108, 3, C_DIM, "-");
  } else {
    fieldCentre(400, 110, 6, 3, C_DIM, "vs");
  }
  char st[28];
  if (live) snprintf(st, sizeof st, "%s", g.summary[0] ? g.summary : "live");
  else if (done) snprintf(st, sizeof st, "%s", g.summary[0] ? g.summary : "Final");
  else whenStr(g.start, st, sizeof st);
  fieldCentre(400, 196, 20, 2, live ? C_GOOD : C_MUTED, st);
  if (g.broadcast[0]) fieldCentre(400, 232, 20, 1, C_DIM, g.broadcast);
  if (g.fav) fieldCentre(400, 300, 20, 1, C_GOLD, "your team");
}

// ── wifi ─────────────────────────────────────────────────────────────────
static uint32_t joinStarted = 0;
static uint16_t wifiRetries = 0;
static void netTick() {
  static bool scanning = true, clockStarted = false;
  if (scanning && netJoinTick(wifiSsid, wifiPass)) scanning = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !clockStarted) {
    clockStarted = true;
    Serial.printf("wifi ok %s %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
    configTzTime(tzString, "pool.ntp.org", "time.nist.gov");
  }
  static uint32_t lastRetry = 0;
  if (up) {
    wifiRetries = 0;
    lastRetry = 0;
  } else if (millis() - joinStarted > 20000 && (lastRetry == 0 || millis() - lastRetry > netRetryDelay(wifiRetries, 20000))) {
    lastRetry = millis();
    Serial.printf("wifi retry #%u\n", ++wifiRetries);
    WiFi.disconnect();
    netJoinStart();
    scanning = true;
  }
}

// ── main ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  bool xp = boardBegin();
  mux = xSemaphoreCreateMutex();
  buf = (uint8_t *)heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM);
  if (cfgLoad()) {
    for (JsonObject o : cfgArr("leagues")) {
      if (nLeagues >= MAX_LEAGUES) break;
      League &l = leagues[nLeagues];
      snprintf(l.id, sizeof l.id, "%s", o["id"] | "");
      snprintf(l.sport, sizeof l.sport, "%s", o["sport"] | "");
      snprintf(l.label, sizeof l.label, "%s", o["label"] | l.id);
      if (l.id[0] && l.sport[0]) nLeagues++;
    }
    for (JsonVariant v : cfgArr("teams")) {
      const char *t = v.as<const char *>();
      if (t && *t && nFavs < 12) snprintf(favs[nFavs++], 6, "%s", t);
    }
    liveMs = (uint32_t)cfgInt("refresh.liveSeconds", 60, 15, 3600) * 1000UL;
    idleMs = (uint32_t)cfgInt("refresh.idleSeconds", 600, 60, 86400) * 1000UL;
    cfgStr("tz", tzString, sizeof tzString);
    cfgRelease();
  }
  Serial.printf("sports: %u leagues, %u favourite teams, refresh %lus live / %lus idle\n", nLeagues, nFavs,
                (unsigned long)(liveMs / 1000), (unsigned long)(idleMs / 1000));
  Preferences prefs;  // the ticker's network: one setup on the board serves both apps
  prefs.begin("ticker", true);
  prefs.getString("ssid", wifiSsid, sizeof wifiSsid);
  prefs.getString("pass", wifiPass, sizeof wifiPass);
  prefs.end();
  if (!wifiSsid[0]) Serial.println("no network in NVS: run the ticker once and set it up there");
  setenv("TZ", tzString, 1);
  tzset();
  gfx = boardDisplay();
  bool ok = gfx->begin();
  Serial.printf("board: expander %s, panel %s, psram %u free\n", xp ? "ok" : "NO ACK", ok ? "ok" : "FAILED", ESP.getFreePsram());
  boardSetRotation(0);
  gfx->setTextWrap(false);
  gfx->fillScreen(C_BG);
  drawList(true);
  backlight(255);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinStart();
  joinStarted = millis();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  netTick();
  int16_t tx, ty, ddy = 0;
  Gesture g = pollGesture(&tx, &ty, &ddy);
  if (view == View::List) {
    if (g == Gesture::Drag && ddy) {  // a row per 26px
      static int16_t acc = 0;
      acc += ddy;
      while (acc <= -ROW_H / 2 && listTop + ROWS < nShown) { acc += ROW_H / 2; listTop++; for (uint8_t s = 0; s < ROWS; s++) drawRow(s, false); }
      while (acc >= ROW_H / 2 && listTop > 0) { acc -= ROW_H / 2; listTop--; for (uint8_t s = 0; s < ROWS; s++) drawRow(s, false); }
      if (listTop == 0 && acc > 0) acc = 0;
      if (listTop + ROWS >= nShown && acc < 0) acc = 0;
    } else if (g == Gesture::TapUp) {
      if (ty < Y_ROW0) {
        int8_t t = hitTab(tx);
        if (t >= 0 && t != sect) {
          sect = (uint8_t)t;
          listTop = 0;
          shownVer = 0;  // retake with the new section
          drawList(true);
        }
      } else {
        int16_t r = hitRow(ty);
        if (r >= 0) {
          gameIdx = (uint8_t)r;
          view = View::Game;
          drawGame(true);
        }
      }
    } else if (g == Gesture::SwipeLeft && nLeagues) {
      sect = (sect + 1) % (nLeagues + 1);
      listTop = 0;
      shownVer = 0;
      drawList(true);
    } else if (g == Gesture::SwipeRight && nLeagues) {
      sect = (sect + nLeagues) % (nLeagues + 1);
      listTop = 0;
      shownVer = 0;
      drawList(true);
    }
    drawList(false);
  } else {
    if (g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) {
      view = View::List;
      drawList(true);
    } else if (g == Gesture::SwipeLeft && nShown) {
      gameIdx = (gameIdx + 1) % nShown;
      drawGame(true);
    } else if (g == Gesture::SwipeRight && nShown) {
      gameIdx = (gameIdx + nShown - 1) % nShown;
      drawGame(true);
    }
    if (shownVer != gamesVer) {  // fresh scores: the same game, if it is still there
      const Game was = shown[order[gameIdx]];
      takeGames();
      for (uint8_t i = 0; i < nShown; i++)
        if (strcmp(shown[order[i]].away, was.away) == 0 && strcmp(shown[order[i]].home, was.home) == 0) gameIdx = i;
    }
    drawGame(false);
  }
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 60000) {
    lastLog = millis();
    Serial.printf("heap %u  psram %u  wifi %s  games %u shown %u\n", ESP.getFreeHeap(), ESP.getFreePsram(),
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN", nGames, nShown);
  }
  delay(20);
}
