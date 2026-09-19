// GAME DAY -- a scores board for the Waveshare ESP32-S3-Touch-LCD-7.
//
// Every game today across the leagues in config.json, from ESPN's own site
// scoreboard (site.web.api.espn.com/apis/v2/scoreboard/header, keyless; the
// public api host refuses, this one answers): live first with the clock,
// then today's fixtures by start time, then finals; favourite teams first
// in each. A tab per sport, a page per game, the ticker's themes and sheets
// from lib/ui. The Wi-Fi network is the one the ticker saved (NVS "ticker":
// ssid/pass), so one setup serves both.
#include <appcfg.h>
#include <baseos.h>
#include <board.h>
#include <sleep.h>
#include <helv.h>
#include <netjoin.h>
#include <ui.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

// ── layout (landscape) ───────────────────────────────────────────────────
static const int16_t Y_ROW0 = UI_Y_ROW0, ROW_H = 52, Y_HINT = UI_Y_HINT, X_RIGHT = 788, LOGO_BIG = 96;
static const uint8_t ROWS = (Y_HINT - 8 - Y_ROW0) / ROW_H;  // 7

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
  gfx->draw16bitRGBBitmapWithTranColor(x, y, px, 0x0000, size, size);  // black is the mark's transparency: no square on a theme
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
struct League { char id[16], sport[12], label[8]; };
static const uint8_t MAX_LEAGUES = 14, MAX_GAMES = 160;  // a UEFA matchday alone is 75 games
// The tabs are sports, not leagues: every league with the same `sport` in
// config.json sits under one tab (soccer holds seven), the league named on the row.
static char sports[MAX_LEAGUES][12];
static uint8_t nSports = 0;
static League leagues[MAX_LEAGUES];
static uint8_t nLeagues = 0;
// Favourite teams: keys "nhl_TOR"; a bare "TOR" from config.json means every
// league's TOR. NVS "favs" (a comma list from the Teams page) replaces the
// config's list once it exists.
static const uint8_t MAX_FAVS = 32;
static char favs[MAX_FAVS][14];
static uint8_t nFavs = 0;
static bool favsFromNvs = false;
static uint8_t sClock = 0, sRet = 1, sSleep = 0;  // 12/24h; 15s/60s/never; never/night
static const char *const CLOCK_NAMES[] = {"12-hour", "24-hour"}, *const RET_NAMES[] = {"15s", "60s", "never"},
                  *const SLEEP_NAMES[] = {"never", "night (23-06)"};
static const uint32_t RET_MS[] = {15000, 60000, 0};
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
static Game *games = nullptr;  // MAX_GAMES, in PSRAM; all leagues in order; the fetch task rebuilds, the UI reads, under mux
static uint8_t nGames = 0;
static uint32_t gamesAt[MAX_LEAGUES], gamesVer = 0;
static bool lgFailed[MAX_LEAGUES];
static SemaphoreHandle_t mux;

static bool isFav(const char *lg, const char *abbr) {
  char key[20];
  snprintf(key, sizeof key, "%s_%s", lg, abbr);
  for (uint8_t i = 0; i < nFavs; i++)
    if (strcmp(favs[i], key) == 0 || (!favsFromNvs && strcmp(favs[i], abbr) == 0)) return true;
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
static const size_t BUF_CAP = 1024 * 1024;  // a league's header feed is 10-260KB, a UEFA matchday 725KB; PSRAM has room
static uint8_t *buf = nullptr;
static void fetchLeague(uint8_t li) {
  char url[160];
  snprintf(url, sizeof url, "https://site.web.api.espn.com/apis/v2/scoreboard/header?sport=%s&league=%s", leagues[li].sport,
           leagues[li].id);
  NetworkClientSecure client;
  client.setInsecure();
  int len = fetchBytes(client, url, UA, buf, BUF_CAP - 1, leagues[li].label);
  static Game *got = (Game *)heap_caps_calloc(MAX_GAMES, sizeof(Game), MALLOC_CAP_SPIRAM);
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
        g.fav = isFav(leagues[li].id, g.away) || isFav(leagues[li].id, g.home);
        n++;
      }
    }
  }
  // Rebuild the shared list: the other leagues' games kept, this one's replaced, then sorted.
  xSemaphoreTake(mux, portMAX_DELAY);
  uint8_t m = 0;
  static Game *merged = (Game *)heap_caps_calloc(MAX_GAMES, sizeof(Game), MALLOC_CAP_SPIRAM);
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

// ── pages ────────────────────────────────────────────────────────────────
enum class View { Splash, List, Game, Settings, Themes, Teams, Info, Choice, Confirm };
static uint8_t chWhich = 0, chN = 0, chSel = 0;  // the open picker: 0 clock, 1 return, 2 sleep
static const char *const *chNames = nullptr;
static uint32_t pageOpenedAt = 0;
static View view = View::Splash;
static View confirmFrom = View::Settings;
static Preferences prefs;
static void saveSettings() {
  prefs.begin("sports", false);
  prefs.putUChar("bg", sTheme);
  prefs.putUChar("clk", sClock);
  prefs.putUChar("ret", sRet);
  prefs.putUChar("slp", sSleep);
  if (favsFromNvs) {
    char list[MAX_FAVS * 14] = "";
    for (uint8_t i = 0; i < nFavs; i++) {
      if (list[0]) strlcat(list, ",", sizeof list);
      strlcat(list, favs[i], sizeof list);
    }
    prefs.putString("favs", list);
  }
  prefs.end();
}
static uint8_t sect = 0;  // 0 ALL, 1 favourites, else sport index + 2
static uint8_t listTop = 0, gameIdx = 0;
static char cHead[48], cRows[ROWS][64];
static int16_t tabX[MAX_LEAGUES + 2], tabW[MAX_LEAGUES + 2];
static uint8_t order[MAX_GAMES], nShown = 0;  // (uint8_t: MAX_GAMES stays under 256)
static Game *shown = nullptr;  // MAX_GAMES, in PSRAM: the UI's copy, taken under mux when the version moves
static uint32_t shownVer = 0;

static void takeGames() {
  xSemaphoreTake(mux, portMAX_DELAY);
  memcpy(shown, games, sizeof(Game) * nGames);
  uint8_t n = nGames;
  shownVer = gamesVer;
  xSemaphoreGive(mux);
  nShown = 0;
  for (uint8_t i = 0; i < n; i++)
    if (sect == 0 || (sect == 1 && shown[i].fav) || (sect >= 2 && strcmp(leagues[shown[i].lg].sport, sports[sect - 2]) == 0))
      order[nShown++] = i;
}
// A sport's glyph, centred at cx,cy with half-size r, in c on C_BG: a laced
// football, a puck, a basketball with its seams, a baseball with stitches,
// a soccer ball with a pentagon. Anything else gets a plain ball.
static void sportIcon(const char *sport, int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  char key[20];  // the sport's picture (Twemoji, made by the tool with --sports) when there is one
  snprintf(key, sizeof key, "sport_%s", sport);
  if (blitLogo(cx - r, cy - r, (uint8_t)(2 * r), key)) return;
  if (strcmp(sport, "football") == 0) {
    gfx->fillEllipse(cx, cy, r + r / 4, r * 2 / 3, c);
    gfx->drawFastHLine(cx - r / 2, cy, r, C_BG);
    for (int8_t k = -1; k <= 1; k++) gfx->drawFastVLine(cx + k * (r / 3), cy - r / 5, r * 2 / 5 + 1, C_BG);
  } else if (strcmp(sport, "hockey") == 0) {
    int16_t ry = r / 2, d = r / 3;
    gfx->fillEllipse(cx, cy + d, r, ry, c);
    gfx->fillRect(cx - r, cy - d, 2 * r + 1, 2 * d, c);
    gfx->fillEllipse(cx, cy - d, r, ry, c);
    gfx->drawEllipse(cx, cy - d, r - 1, ry - 1, C_BG);
  } else if (strcmp(sport, "basketball") == 0) {
    gfx->fillCircle(cx, cy, r, c);
    gfx->drawFastVLine(cx, cy - r, 2 * r + 1, C_BG);
    gfx->drawFastHLine(cx - r, cy, 2 * r + 1, C_BG);
    for (int16_t i = -r; i <= r; i++) {  // the two curved seams
      int16_t y = (int16_t)lroundf(sqrtf(max(0.0f, (float)r * r * 1.9f - (float)(i + r * 1.35f) * (i + r * 1.35f))));
      if (abs(i) < r && abs(y) < r) gfx->drawPixel(cx + i, cy + y, C_BG), gfx->drawPixel(cx + i, cy - y, C_BG);
    }
  } else if (strcmp(sport, "baseball") == 0) {
    gfx->fillCircle(cx, cy, r, c);
    for (int8_t side = -1; side <= 1; side += 2) {  // two stitch arcs: circles centred outside, kept inside the ball
      float ox = cx + side * r * 1.6f, rr = r * 1.25f;
      for (float a = 0; a < 6.2832f; a += 0.05f) {
        int16_t x = (int16_t)lroundf(ox + rr * cosf(a)), y = (int16_t)lroundf(cy + rr * sinf(a));
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < (r - 1) * (r - 1)) gfx->drawPixel(x, y, C_BG);
      }
    }
  } else if (strcmp(sport, "soccer") == 0) {
    gfx->fillCircle(cx, cy, r, c);
    int16_t px[5], py[5];
    for (uint8_t k = 0; k < 5; k++) {
      float a = -1.5708f + k * 1.2566f;
      px[k] = cx + (int16_t)lroundf(r * 0.42f * cosf(a));
      py[k] = cy + (int16_t)lroundf(r * 0.42f * sinf(a));
      gfx->drawLine(px[k], py[k], cx + (int16_t)lroundf(r * cosf(a)), cy + (int16_t)lroundf(r * sinf(a)), C_BG);
    }
    for (uint8_t k = 1; k < 4; k++) gfx->fillTriangle(px[0], py[0], px[k], py[k], px[k + 1], py[k + 1], C_BG);
  } else {
    gfx->fillCircle(cx, cy, r, c);
  }
}
static void drawTabs() {
  gfx->fillRect(0, 0, 560, Y_ROW0 - 1, C_BG);
  int16_t x = 12;
  for (uint8_t i = 0; i <= nSports + 1; i++) {  // ALL as a word, a star for the favourites, a glyph per sport, 52px each
    bool on = i == sect;
    int16_t w = i == 0 ? textWidth(1, "ALL") + 26 : 52;
    tabX[i] = x;
    tabW[i] = w;
    if (i == 0) textAt(x + 13, 12, 1, on ? C_FG : C_DIM, "ALL");
    else if (i == 1) starGlyph(x + 26, 20, 12, on ? C_GOLD : C_DIM);
    else sportIcon(sports[i - 2], x + 26, 19, 14, on ? C_FG : C_DIM);  // 28px, in colour; the underline says which is open
    if (on) gfx->fillRect(x + 9, 34, w - 18, 3, C_FG);
    x += w;
  }
  settingsIcon(676, C_MUTED);  // by the clock
  gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
}
static int8_t hitTab(int16_t x) {
  for (uint8_t i = 0; i <= nSports + 1; i++)
    if (x >= tabX[i] && x < tabX[i] + tabW[i]) return i;
  return -1;
}
static void drawClock() {
  struct tm t;
  char clk[12] = "";
  if (getLocalTime(&t, 0)) clockStr(t, sClock, clk, sizeof clk);
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
  clockStr(t, sClock, hm, sizeof hm);
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
  textAt(480, y + 17, 1, C_DIM, leagues[g.lg].label);  // the league, always: a sport holds several
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
    snprintf(e, sizeof e, "%s", !any ? "fetching scores..." : sect == 1 ? "no games for your teams today" : failed ? "no scores (fetch failed)" : "no games today");
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
  teamMark(76, 88, LOGO_BIG, leagues[g.lg].id, g.away, ac);
  fieldCentre(124, 200, 12, 2, ac, g.away);
  fieldCentre(124, 230, 24, 1, C_MUTED, g.awayName);
  teamMark(628, 88, LOGO_BIG, leagues[g.lg].id, g.home, hc);
  fieldCentre(676, 200, 12, 2, hc, g.home);
  fieldCentre(676, 230, 24, 1, C_MUTED, g.homeName);
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

// ── the splash: GAME DAY, the sports, a scoreboard ────────────────────────
static void drawSplash(const char *status) {
  gfx->fillScreen(C_BG);
  const char *name = "GAME DAY";
  textAt((LCD_W - textWidth(5, name)) / 2, 44, 5, C_FG, name);
  // the sports as pictures, 64px, 120px apart, on a rule
  int16_t cy = 250, step = 120;
  int16_t x = LCD_W / 2 - (nSports - 1) * step / 2;
  for (uint8_t i = 0; i < nSports; i++, x += step) sportIcon(sports[i], x, cy, 32, C_MUTED);
  gfx->drawFastHLine(LCD_W / 8, 330, LCD_W * 3 / 4, C_RULE);
  const char *credit = "made by Richard Torcato";
  textAt((LCD_W - textWidth(1, credit)) / 2, 424, 1, C_MUTED, credit);
  drawHint(status);
}
// ── settings: Theme, Shutdown; the themes page; the shutdown sheet ───────
static void drawSettings() {
  pageHeader("SETTINGS");
  char n[8];
  snprintf(n, sizeof n, "%u", nFavs);
  settingRow(0, "Teams", n);
  settingRow(1, "Clock", CLOCK_NAMES[sClock]);
  settingRow(2, "Auto return", RET_NAMES[sRet]);
  settingRow(3, "Sleep", SLEEP_NAMES[sSleep]);
  settingRow(4, "Theme", THEMES[sTheme].name);
  settingRow(5, "Info", "");
  settingRow(6, "Wi-Fi", wifiSsid[0] ? wifiSsid : "not set");
  settingRow(7, "Shut down", "", C_BAD);
  drawHint("< scores        Wi-Fi is set up in Ticker Tape");
}
static void openChoice(uint8_t which) {
  chWhich = which;
  chNames = which == 0 ? CLOCK_NAMES : which == 1 ? RET_NAMES : SLEEP_NAMES;
  chN = which == 1 ? 3 : 2;
  chSel = which == 0 ? sClock : which == 1 ? sRet : sSleep;
  view = View::Choice;
  drawChoiceSheet(which == 0 ? "CLOCK" : which == 1 ? "AUTO RETURN" : "SLEEP", chNames, chN, chSel);
}
static void applyChoice(uint8_t i) {
  if (chWhich == 0) sClock = i;
  else if (chWhich == 1) sRet = i;
  else sSleep = i;
  saveSettings();
}
// The Teams page: every team in this week's games, by league, a star each.
struct TeamRow { uint8_t lg; char abbr[6], name[24]; };
static TeamRow *teamRows = nullptr;  // MAX_GAMES*2, PSRAM
static uint8_t nTeamRows = 0, teamTop = 0;
static const uint8_t TEAM_ROWS = (Y_HINT - 8 - UI_S_Y0) / 44;  // 8
static void buildTeamRows() {
  nTeamRows = 0;
  xSemaphoreTake(mux, portMAX_DELAY);
  for (uint8_t i = 0; i < nGames && nTeamRows < MAX_GAMES * 2 - 1; i++) {
    for (uint8_t side = 0; side < 2; side++) {
      const char *ab = side ? games[i].home : games[i].away, *nm = side ? games[i].homeName : games[i].awayName;
      bool seen = false;
      for (uint8_t k = 0; k < nTeamRows && !seen; k++) seen = teamRows[k].lg == games[i].lg && strcmp(teamRows[k].abbr, ab) == 0;
      if (seen) continue;
      TeamRow &r = teamRows[nTeamRows++];
      r.lg = games[i].lg;
      snprintf(r.abbr, sizeof r.abbr, "%s", ab);
      snprintf(r.name, sizeof r.name, "%s", nm);
    }
  }
  xSemaphoreGive(mux);
  for (uint8_t i = 1; i < nTeamRows; i++) {  // by league, then letters
    TeamRow k = teamRows[i];
    int8_t j = i - 1;
    while (j >= 0 && (teamRows[j].lg > k.lg || (teamRows[j].lg == k.lg && strcmp(teamRows[j].abbr, k.abbr) > 0))) { teamRows[j + 1] = teamRows[j]; j--; }
    teamRows[j + 1] = k;
  }
}
static void drawTeamRow(uint8_t slot) {
  int16_t y = UI_S_Y0 + slot * 44;
  gfx->fillRect(0, y, LCD_W, 44, C_BG);
  uint8_t i = teamTop + slot;
  if (i >= nTeamRows) return;
  const TeamRow &r = teamRows[i];
  bool fav = isFav(leagues[r.lg].id, r.abbr);
  textAt(20, y + 12, 1, C_DIM, leagues[r.lg].label);
  teamMark(90, y + 6, 32, leagues[r.lg].id, r.abbr, C_FG);
  textAt(132, y + 10, 2, fav ? C_FG : C_MUTED, r.abbr);
  textAt(210, y + 14, 1, C_MUTED, r.name);
  starGlyph(LCD_W - 40, y + 22, 13, fav ? C_GOLD : C_RULE);
  gfx->drawFastHLine(20, y + 43, LCD_W - 40, C_RULE);
}
static void drawTeams() {
  pageHeader("TEAMS", "star the ones you follow");
  for (uint8_t s = 0; s < TEAM_ROWS; s++) drawTeamRow(s);
  drawHint(nTeamRows ? "tap a team        drag for more        < settings" : "no teams yet: the scores have not landed");
}
// Toggling a team turns the config's bare list into a full one in NVS first.
static void toggleFav(const TeamRow &r) {
  if (!favsFromNvs) {
    char keep[MAX_FAVS][14];
    uint8_t n = 0;
    for (uint8_t i = 0; i < nTeamRows && n < MAX_FAVS; i++)
      if (isFav(leagues[teamRows[i].lg].id, teamRows[i].abbr)) snprintf(keep[n++], 14, "%s_%s", leagues[teamRows[i].lg].id, teamRows[i].abbr);
    memcpy(favs, keep, sizeof keep);
    nFavs = n;
    favsFromNvs = true;
  }
  char key[20];
  snprintf(key, sizeof key, "%s_%s", leagues[r.lg].id, r.abbr);
  for (uint8_t i = 0; i < nFavs; i++)
    if (strcmp(favs[i], key) == 0) {  // off
      for (uint8_t k = i + 1; k < nFavs; k++) strcpy(favs[k - 1], favs[k]);
      nFavs--;
      saveSettings();
      return;
    }
  if (nFavs < MAX_FAVS) snprintf(favs[nFavs++], 14, "%s", key);
  saveSettings();
}
static void refav() {  // the games carry a fav flag: recompute after a change
  xSemaphoreTake(mux, portMAX_DELAY);
  for (uint8_t i = 0; i < nGames; i++) games[i].fav = isFav(leagues[games[i].lg].id, games[i].away) || isFav(leagues[games[i].lg].id, games[i].home);
  gamesVer++;
  xSemaphoreGive(mux);
}
static void drawInfo() {
  pageHeader("INFO");
  char l[8][48];
  uint8_t n = 0;
  snprintf(l[n++], 48, "%u leagues in %u sports, %u games today", nLeagues, nSports, nGames);
  snprintf(l[n++], 48, "%u favourite teams", nFavs);
  snprintf(l[n++], 48, "refresh %lus with a game on, %lus otherwise", (unsigned long)(liveMs / 1000), (unsigned long)(idleMs / 1000));
  snprintf(l[n++], 48, "wifi %.16s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  snprintf(l[n++], 48, "heap %uk free, psram %uk free", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  snprintf(l[n++], 48, "up %luh %02lum", (unsigned long)(millis() / 3600000), (unsigned long)(millis() / 60000 % 60));
  snprintf(l[n++], 48, "built " __DATE__ " " __TIME__);
  for (uint8_t i = 0; i < n; i++) textAt(20, 60 + i * 26, 1, i == 0 ? C_FG : C_MUTED, l[i]);
  drawHint("< settings");
}
static void openConfirm() {
  confirmFrom = view;
  if (view == View::List) {
    gfx->fillScreen(C_BG);
    drawTabs();
  }
  view = View::Confirm;
  drawShutdownSheet();
}
static void closeConfirm() {
  sheetClose();
  view = confirmFrom;
  if (view == View::List) drawList(true);
}
static void shutDown() {
  drawHint("shutting down. BOOT button turns it on", C_WARN);
  delay(600);
  goToSleep(0);  // secs 0: the BOOT button alone, never a brush of the panel
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
  // The base: BOOT held goes back to it, a cold boot with no handoff goes
  // back to it, and otherwise we tell it the slot is ours. Must run before
  // the panel -- GPIO0 is BOOT and also the panel green bit 0.
  baseAppBoot("sports");
  mux = xSemaphoreCreateMutex();
  buf = (uint8_t *)heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM);
  games = (Game *)heap_caps_calloc(MAX_GAMES, sizeof(Game), MALLOC_CAP_SPIRAM);
  shown = (Game *)heap_caps_calloc(MAX_GAMES, sizeof(Game), MALLOC_CAP_SPIRAM);
  if (cfgLoad()) {
    for (JsonObject o : cfgArr("leagues")) {
      if (nLeagues >= MAX_LEAGUES) break;
      League &l = leagues[nLeagues];
      snprintf(l.id, sizeof l.id, "%s", o["id"] | "");
      snprintf(l.sport, sizeof l.sport, "%s", o["sport"] | "");
      snprintf(l.label, sizeof l.label, "%s", o["label"] | l.id);
      if (l.id[0] && l.sport[0]) nLeagues++;
    }
    for (uint8_t i = 0; i < nLeagues; i++) {  // the sports, in the order the leagues first name them
      bool seen = false;
      for (uint8_t j = 0; j < nSports && !seen; j++) seen = strcmp(sports[j], leagues[i].sport) == 0;
      if (!seen) snprintf(sports[nSports++], sizeof sports[0], "%s", leagues[i].sport);
    }
    for (JsonVariant v : cfgArr("teams")) {  // "TOR" (every league) or "nhl:TOR"
      const char *t = v.as<const char *>();
      if (!t || !*t || nFavs >= MAX_FAVS) continue;
      snprintf(favs[nFavs], 14, "%s", t);
      for (char *c = favs[nFavs]; *c; c++) if (*c == ':') *c = '_';
      nFavs++;
    }
    liveMs = (uint32_t)cfgInt("refresh.liveSeconds", 60, 15, 3600) * 1000UL;
    idleMs = (uint32_t)cfgInt("refresh.idleSeconds", 600, 60, 86400) * 1000UL;
    cfgStr("tz", tzString, sizeof tzString);
    cfgRelease();
  }
  Serial.printf("game day: %u leagues in %u sports, %u favourite teams, refresh %lus live / %lus idle\n", nLeagues, nSports, nFavs,
                (unsigned long)(liveMs / 1000), (unsigned long)(idleMs / 1000));
  // Credentials belong to the board, not to this app: Home owns the setup
  // portal and writes them to NVS "base". This used to read the TICKER's
  // namespace and tell you to go and run the ticker first.
  baseLoad();
  snprintf(wifiSsid, sizeof wifiSsid, "%s", baseCfg.ssid);
  snprintf(wifiPass, sizeof wifiPass, "%s", baseCfg.pass);
  if (!wifiSsid[0]) Serial.println("no network: Home runs setup -- hold BOOT at power-on to get there");
  setenv("TZ", tzString, 1);
  tzset();
  prefs.begin("sports", true);
  sTheme = prefs.getUChar("bg", 0) % N_THEMES;
  sClock = prefs.getUChar("clk", 0) % 2;
  sRet = prefs.getUChar("ret", 1) % 3;
  sSleep = prefs.getUChar("slp", 0) % 2;
  if (prefs.isKey("favs")) {  // the Teams page's list beats the config's
    char list[MAX_FAVS * 14];
    prefs.getString("favs", list, sizeof list);
    nFavs = 0;
    favsFromNvs = true;
    for (char *tok = strtok(list, ","); tok && nFavs < MAX_FAVS; tok = strtok(nullptr, ",")) snprintf(favs[nFavs++], 14, "%s", tok);
  }
  prefs.end();
  applyTheme();
  teamRows = (TeamRow *)heap_caps_calloc(MAX_GAMES * 2, sizeof(TeamRow), MALLOC_CAP_SPIRAM);
  gfx = boardDisplay();
  bool ok = gfx->begin();
  Serial.printf("board: expander %s, panel %s, psram %u free\n", xp ? "ok" : "NO ACK", ok ? "ok" : "FAILED", ESP.getFreePsram());
  boardSetRotation(0);
  gfx->setTextWrap(false);
  char st[48];
  snprintf(st, sizeof st, "connecting to %.24s", wifiSsid[0] ? wifiSsid : "(no network)");
  drawSplash(st);
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
  // A finger held on the header for two seconds: the shutdown sheet.
  static uint32_t headerHoldAt = 0;
  if (g == Gesture::LongPress && ty < Y_ROW0 && view != View::Confirm && view != View::Splash) headerHoldAt = millis();
  if (!touchHeld) headerHoldAt = 0;
  if (headerHoldAt && millis() - headerHoldAt > 1300) {
    headerHoldAt = 0;
    openConfirm();
    g = Gesture::None;
  }
  if (view == View::Splash) {  // until the first league lands, or a tap
    bool any = false;
    for (uint8_t li = 0; li < nLeagues; li++) any |= gamesAt[li] != 0;
    static bool said = false;
    if (WiFi.status() == WL_CONNECTED && !said) {
      said = true;
      drawHint("fetching scores...");
    }
    if (any || g == Gesture::TapUp) {
      view = View::List;
      drawList(true);
    }
  } else if (view == View::List) {
    if (g == Gesture::Drag && ddy) {  // a row per half a row of movement
      static int16_t acc = 0;
      acc += ddy;
      while (acc <= -ROW_H / 2 && listTop + ROWS < nShown) { acc += ROW_H / 2; listTop++; for (uint8_t s = 0; s < ROWS; s++) drawRow(s, false); }
      while (acc >= ROW_H / 2 && listTop > 0) { acc -= ROW_H / 2; listTop--; for (uint8_t s = 0; s < ROWS; s++) drawRow(s, false); }
      if (listTop == 0 && acc > 0) acc = 0;
      if (listTop + ROWS >= nShown && acc < 0) acc = 0;
    } else if (g == Gesture::TapUp) {
      if (ty < Y_ROW0) {
        if (tx >= 660 && tx < 724) {
          view = View::Settings;
          drawSettings();
        } else {
          int8_t t = hitTab(tx);
          if (t >= 0 && t != sect) {
            sect = (uint8_t)t;
            listTop = 0;
            shownVer = 0;
            drawList(true);
          }
        }
      } else {
        int16_t r = hitRow(ty);
        if (r >= 0) {
          gameIdx = (uint8_t)r;
          view = View::Game;
          pageOpenedAt = millis();
          drawGame(true);
        }
      }
    } else if (g == Gesture::SwipeLeft && nSports) {
      sect = (sect + 1) % (nSports + 2);
      listTop = 0;
      shownVer = 0;
      drawList(true);
    } else if (g == Gesture::SwipeRight && nSports) {
      sect = (sect + nSports + 1) % (nSports + 2);
      listTop = 0;
      shownVer = 0;
      drawList(true);
    }
    if (view == View::List) drawList(false);
  } else if (view == View::Game) {
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
    if (view == View::Game) {
      if (shownVer != gamesVer) {  // fresh scores: the same game, if it is still there
        const Game was = shown[order[gameIdx]];
        takeGames();
        for (uint8_t i = 0; i < nShown; i++)
          if (strcmp(shown[order[i]].away, was.away) == 0 && strcmp(shown[order[i]].home, was.home) == 0) gameIdx = i;
      }
      drawGame(false);
    }
  } else if (view == View::Settings) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) {
      view = View::List;
      drawList(true);
    } else if (g == Gesture::TapUp) {
      int8_t i = hitSettingRow(ty, 8);
      if (i == 0) {
        buildTeamRows();
        teamTop = 0;
        view = View::Teams;
        drawTeams();
      } else if (i == 1 || i == 2 || i == 3) {
        openChoice((uint8_t)(i - 1));
      } else if (i == 4) {
        view = View::Themes;
        drawThemesPage();
      } else if (i == 5) {
        view = View::Info;
        drawInfo();
      } else if (i == 7) {
        openConfirm();
      }
    }
  } else if (view == View::Teams) {
    if (g == Gesture::Drag && ddy) {
      static int16_t acc = 0;
      acc += ddy;
      while (acc <= -22 && teamTop + TEAM_ROWS < nTeamRows) { acc += 22; teamTop++; for (uint8_t s = 0; s < TEAM_ROWS; s++) drawTeamRow(s); }
      while (acc >= 22 && teamTop > 0) { acc -= 22; teamTop--; for (uint8_t s = 0; s < TEAM_ROWS; s++) drawTeamRow(s); }
      if (teamTop == 0 && acc > 0) acc = 0;
      if (teamTop + TEAM_ROWS >= nTeamRows && acc < 0) acc = 0;
    } else if (g == Gesture::SwipeLeft || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) {
      refav();
      view = View::Settings;
      drawSettings();
    } else if (g == Gesture::TapUp && ty >= UI_S_Y0) {
      uint8_t slot = (ty - UI_S_Y0) / 44;
      if (slot < TEAM_ROWS && teamTop + slot < nTeamRows) {
        toggleFav(teamRows[teamTop + slot]);
        drawTeamRow(slot);
      }
    }
  } else if (view == View::Info) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || g == Gesture::TapUp) {
      view = View::Settings;
      drawSettings();
    }
  } else if (view == View::Choice) {
    if (g == Gesture::TapUp || g == Gesture::Tap) {
      int8_t i = hitChoice(tx, ty, chN);
      if (i >= 0 && i != chSel) applyChoice((uint8_t)i);
      if (i >= 0 || !inSheet(tx, ty)) {
        sheetClose();
        view = View::Settings;
        drawSettings();
      }
    } else if (g == Gesture::SwipeDown || g == Gesture::SwipeLeft) {
      sheetClose();
      view = View::Settings;
    }
  } else if (view == View::Themes) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) {
      view = View::Settings;
      drawSettings();
    } else if (g == Gesture::TapUp) {
      int8_t i = hitTheme(tx, ty);
      if (i >= 0 && i != sTheme) {
        sTheme = (uint8_t)i;
        applyTheme();
        saveSettings();
        drawThemesPage();
      }
    }
  } else if (view == View::Confirm) {
    if (g == Gesture::TapUp || g == Gesture::Tap) {
      if (hitPill(tx, ty)) shutDown();
      else closeConfirm();
    } else if (g == Gesture::SwipeDown || g == Gesture::SwipeLeft) {
      closeConfirm();
    }
  }
  if (touchHeld) pageOpenedAt = millis();
  if (view == View::Game && RET_MS[sRet] && millis() - pageOpenedAt > RET_MS[sRet]) {  // back to the scores by itself
    view = View::List;
    drawList(true);
  }
  // Night: the panel dark from 23:00 to 06:00 when Sleep says so; a touch lights it for a minute.
  static bool dark = false;
  static uint32_t litAt = 0;
  struct tm t;
  bool night = sSleep == 1 && getLocalTime(&t, 0) && (t.tm_hour >= 23 || t.tm_hour < 6);
  if (touchHeld) litAt = millis();
  bool wantDark = night && millis() - litAt > 60000;
  if (wantDark != dark) {
    dark = wantDark;
    backlight(dark ? 0 : 255);
  }
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 60000) {
    lastLog = millis();
    Serial.printf("heap %u  psram %u  wifi %s  games %u shown %u\n", ESP.getFreeHeap(), ESP.getFreePsram(),
                  WiFi.status() == WL_CONNECTED ? "up" : "DOWN", nGames, nShown);
  }
  delay(20);
}
