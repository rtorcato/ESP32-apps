// SOCIAL -- a ticker of your own numbers, for the Waveshare ESP32-S3-Touch-LCD-7.
//
// A row per account: followers, stars, downloads or views, today's change,
// a sparkline of the last thirty days (kept by the board itself, one number
// a day in /history.json -- no service hands out history), and on the
// account's page the latest post with its likes. Only the services that
// answer without a key: Bluesky, Mastodon, GitHub, npm, YouTube (views).
// Built on lib/ui; the Wi-Fi network is the ticker's (NVS "ticker").
#include <appcfg.h>
#include <board.h>
#include <sleep.h>
#include <helv.h>
#include <netjoin.h>
#include <ui.h>

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);
static const int16_t Y_ROW0 = UI_Y_ROW0, ROW_H = 44, Y_HINT = UI_Y_HINT, X_RIGHT = 788;
static const uint8_t ROWS = (Y_HINT - 8 - Y_ROW0) / ROW_H;  // 9
static const char *UA = "Mozilla/5.0 (esp32-social)";

// ── the accounts ─────────────────────────────────────────────────────────
enum : uint8_t { S_BLUESKY, S_MASTODON, S_GITHUB, S_NPM, S_YOUTUBE, N_SERVICES };
static const char *const SVC_NAMES[] = {"bluesky", "mastodon", "github", "npm", "youtube"};
static const char *const SVC_TABS[] = {"BLUESKY", "MASTODON", "GITHUB", "NPM", "YOUTUBE"};
static const uint16_t SVC_COLOUR[] = {rgb(17, 133, 254), rgb(99, 100, 255), rgb(36, 41, 46), rgb(203, 56, 55), rgb(255, 0, 0)};
static const uint8_t MAX_ACC = 64, HIST_N = 31;
struct Account {
  uint8_t svc;
  bool repo, valid;
  char id[48], label[24], what[16];
  float count, second;      // followers / stars / downloads / views; and posts / forks / -
  char latest[140];         // the latest post's text, or the video's title
  float likes, reposts, replies;
  float hist[HIST_N];       // one a day, oldest first; hist[n-1] is today
  int32_t histDay[HIST_N];  // the day number of each
  uint8_t n;
  uint32_t at;
};
static Account *acc = nullptr;  // MAX_ACC, in PSRAM; the fetch task writes, the UI reads, under mux
static uint8_t nAcc = 0;
static uint32_t refreshMs = 600000, accVer = 0;
static char tzString[48] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static char wifiSsid[33] = "", wifiPass[65] = "";
static SemaphoreHandle_t mux;
static bool svcUsed[N_SERVICES];

// ── history: one number a day per account, in /history.json ──────────────
static int32_t today() {
  time_t t = time(nullptr);
  if (t < 1000000000L) return 0;
  struct tm lt;
  localtime_r(&t, &lt);
  return (int32_t)lt.tm_year * 366 + lt.tm_yday;  // one number per local day, only ever compared
}
static void histKey(const Account &a, char *out, size_t n) { snprintf(out, n, "%s:%s", SVC_NAMES[a.svc], a.id); }
static void histLoad() {
  File f = LittleFS.open("/history.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  for (uint8_t i = 0; i < nAcc; i++) {
    char k[64];
    histKey(acc[i], k, sizeof k);
    acc[i].n = 0;
    for (JsonArray pair : doc[k].as<JsonArray>()) {
      if (acc[i].n >= HIST_N) break;
      acc[i].histDay[acc[i].n] = pair[0] | 0L;
      acc[i].hist[acc[i].n] = pair[1] | 0.0f;
      acc[i].n++;
    }
  }
  Serial.println("history: loaded");
}
static void histSave() {
  JsonDocument doc;
  for (uint8_t i = 0; i < nAcc; i++) {
    char k[64];
    histKey(acc[i], k, sizeof k);
    JsonArray arr = doc[k].to<JsonArray>();
    for (uint8_t j = 0; j < acc[i].n; j++) {
      JsonArray pair = arr.add<JsonArray>();
      pair.add(acc[i].histDay[j]);
      pair.add(acc[i].hist[j]);
    }
  }
  File f = LittleFS.open("/history.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}
// Today's value into the history: replace today's entry, or append and trim.
static void histRecord(Account &a, float v) {
  int32_t d = today();
  if (!d) return;
  if (a.n && a.histDay[a.n - 1] == d) {
    a.hist[a.n - 1] = v;
    return;
  }
  if (a.n == HIST_N) {
    memmove(a.hist, a.hist + 1, (HIST_N - 1) * sizeof(float));
    memmove(a.histDay, a.histDay + 1, (HIST_N - 1) * sizeof(int32_t));
    a.n--;
  }
  a.histDay[a.n] = d;
  a.hist[a.n] = v;
  a.n++;
}
// Today's change: against yesterday's number when there is one, else against
// the first number seen today (so a fresh board shows the change since boot).
static float deltaToday(const Account &a) {
  if (a.n >= 2) return a.count - a.hist[a.n - 2];
  return 0;
}

// ── fetching (core 0 task) ───────────────────────────────────────────────
static uint8_t *buf = nullptr;  // internal RAM, not PSRAM: parsing out of PSRAM starved the scan-out and the panel flickered
static const size_t CAP = 48 * 1024;
static volatile bool histDirty = false;
static void stripHtml(char *s) {  // Mastodon's content is HTML: tags out, a few entities back, </p> a space
  char *w = s;
  bool in = false;
  for (char *r = s; *r; r++) {
    if (*r == '<') { in = true; if (strncmp(r, "</p>", 4) == 0 || strncmp(r, "<br", 3) == 0) *w++ = ' '; continue; }
    if (*r == '>') { in = false; continue; }
    if (!in) *w++ = *r;
  }
  *w = '\0';
  static const char *const from[] = {"&amp;", "&#39;", "&quot;", "&lt;", "&gt;", "&nbsp;"};
  static const char *const to[] = {"&", "'", "\"", "<", ">", " "};
  for (uint8_t i = 0; i < 6; i++)
    for (char *p; (p = strstr(s, from[i]));) {
      size_t fl = strlen(from[i]), tl = strlen(to[i]);
      memcpy(p, to[i], tl);
      memmove(p + tl, p + fl, strlen(p + fl) + 1);
    }
}
static bool getJson(NetworkClientSecure &client, const char *url, JsonDocument &doc, const char *tag) {
  int len = fetchBytes(client, url, UA, buf, CAP - 1, tag);
  if (len <= 0) return false;
  DeserializationError je = deserializeJson(doc, (const char *)buf, (size_t)len, DeserializationOption::NestingLimit(24));
  if (je) Serial.printf("%s: json %s (%d bytes)\n", tag, je.c_str(), len);
  return !je;
}
static bool fetchAccount(Account &a) {
  NetworkClientSecure client;
  client.setInsecure();
  char url[200];
  JsonDocument doc;
  a.latest[0] = '\0';
  a.likes = a.reposts = a.replies = 0;
  if (a.svc == S_BLUESKY) {
    snprintf(url, sizeof url, "https://public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor=%s", a.id);
    if (!getJson(client, url, doc, a.label)) return false;
    a.count = doc["followersCount"] | 0.0f;
    a.second = doc["postsCount"] | 0.0f;
    strcpy(a.what, "followers");
    snprintf(url, sizeof url, "https://public.api.bsky.app/xrpc/app.bsky.feed.getAuthorFeed?actor=%s&limit=1&filter=posts_no_replies", a.id);
    JsonDocument feed;
    if (getJson(client, url, feed, a.label)) {
      JsonObject post = feed["feed"][0]["post"];
      snprintf(a.latest, sizeof a.latest, "%s", post["record"]["text"] | "");
      a.likes = post["likeCount"] | 0.0f;
      a.reposts = post["repostCount"] | 0.0f;
      a.replies = post["replyCount"] | 0.0f;
    }
  } else if (a.svc == S_MASTODON) {  // id "instance/user"
    char inst[40], user[40];
    const char *slash = strchr(a.id, '/');
    if (!slash) return false;
    snprintf(inst, sizeof inst, "%.*s", (int)(slash - a.id), a.id);
    snprintf(user, sizeof user, "%s", slash + 1);
    snprintf(url, sizeof url, "https://%s/api/v1/accounts/lookup?acct=%s", inst, user);
    if (!getJson(client, url, doc, a.label)) return false;
    a.count = doc["followers_count"] | 0.0f;
    a.second = doc["statuses_count"] | 0.0f;
    strcpy(a.what, "followers");
    const char *id = doc["id"] | "";
    snprintf(url, sizeof url, "https://%s/api/v1/accounts/%s/statuses?limit=1&exclude_replies=true&exclude_reblogs=true", inst, id);
    JsonDocument st;
    if (id[0] && getJson(client, url, st, a.label)) {
      JsonObject s0 = st[0];
      snprintf(a.latest, sizeof a.latest, "%s", s0["content"] | "");
      stripHtml(a.latest);
      a.likes = s0["favourites_count"] | 0.0f;
      a.reposts = s0["reblogs_count"] | 0.0f;
      a.replies = s0["replies_count"] | 0.0f;
    }
  } else if (a.svc == S_GITHUB) {
    snprintf(url, sizeof url, "https://api.github.com/%s/%s", a.repo ? "repos" : "users", a.id);
    if (!getJson(client, url, doc, a.label)) return false;
    if (a.repo) {
      a.count = doc["stargazers_count"] | 0.0f;
      a.second = doc["forks_count"] | 0.0f;
      strcpy(a.what, "stars");
      snprintf(a.latest, sizeof a.latest, "%s", doc["description"] | "");
      a.likes = doc["open_issues_count"] | 0.0f;
    } else {
      a.count = doc["followers"] | 0.0f;
      a.second = doc["public_repos"] | 0.0f;
      strcpy(a.what, "followers");
      snprintf(a.latest, sizeof a.latest, "%s", doc["bio"] | "");
    }
  } else if (a.svc == S_NPM) {
    snprintf(url, sizeof url, "https://api.npmjs.org/downloads/point/last-week/%s", a.id);
    if (!getJson(client, url, doc, a.label)) return false;
    a.count = doc["downloads"] | 0.0f;
    strcpy(a.what, "downloads/wk");
    snprintf(url, sizeof url, "https://api.npmjs.org/downloads/point/last-day/%s", a.id);
    JsonDocument day;
    if (getJson(client, url, day, a.label)) a.second = day["downloads"] | 0.0f;
  } else if (a.svc == S_YOUTUBE) {  // the channel feed: the latest video and its views
    snprintf(url, sizeof url, "https://www.youtube.com/feeds/videos.xml?channel_id=%s", a.id);
    int len = fetchBytes(client, url, UA, buf, CAP - 1, a.label);
    if (len <= 0) return false;
    buf[len] = '\0';
    const char *e = strstr((const char *)buf, "<entry>");
    if (!e) return false;
    const char *t = strstr(e, "<title>"), *v = strstr(e, "views=\"");
    if (t) {
      const char *te = strstr(t + 7, "</title>");
      snprintf(a.latest, sizeof a.latest, "%.*s", te ? (int)(te - t - 7) : 0, t + 7);
    }
    a.count = v ? (float)atof(v + 7) : 0;
    strcpy(a.what, "views, latest");
    const char *l = strstr(e, "<media:starRating count=\"");
    a.likes = l ? (float)atof(l + 25) : 0;
  }
  a.valid = true;
  return true;
}
static void fetchTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(3000));  // a breath between fetches: a burst of them starved the scan-out
    if (WiFi.status() != WL_CONNECTED || !buf) continue;
    for (uint8_t i = 0; i < nAcc; i++) {
      // GitHub allows sixty requests an hour without a token: its accounts go round hourly, whatever the config says
      uint32_t every = acc[i].svc == S_GITHUB ? max<uint32_t>(refreshMs, 3600000UL) : refreshMs;
      if (acc[i].at && millis() - acc[i].at < every) continue;
      Account a;
      xSemaphoreTake(mux, portMAX_DELAY);
      a = acc[i];
      xSemaphoreGive(mux);
      bool ok = fetchAccount(a);
      a.at = millis();
      if (ok) histRecord(a, a.count);
      xSemaphoreTake(mux, portMAX_DELAY);
      acc[i] = a;
      accVer++;
      xSemaphoreGive(mux);
      Serial.printf("%s %s: %s %.0f%s (heap %u)\n", SVC_NAMES[a.svc], a.label, ok ? a.what : "FAILED", a.count, ok ? "" : " -", ESP.getFreeHeap());
      if (ok) histDirty = true;
      break;  // one a pass
    }
    // The history file, at most once a minute: a flash write stalls the panel's
    // scan-out for a moment, and twenty-four of them in a row at boot flickered.
    static uint32_t savedAt = 0;
    if (histDirty && millis() - savedAt > 60000) {
      xSemaphoreTake(mux, portMAX_DELAY);
      histSave();
      xSemaphoreGive(mux);
      histDirty = false;
      savedAt = millis();
    }
  }
}

// ── pages ────────────────────────────────────────────────────────────────
enum class View { Splash, Home, List, Detail, Settings, Themes, Info, Choice, Confirm };
static View view = View::Splash, confirmFrom = View::Settings;
static uint8_t sect = 0, listTop = 0, detailIdx = 0, sClock = 0, chN = 0, chSel = 0;
static const char *const CLOCK_NAMES[] = {"12-hour", "24-hour"};
static uint8_t order[MAX_ACC], nShown = 0;
static Account *shown = nullptr;  // MAX_ACC, in PSRAM
static uint32_t shownVer = 0;
static char cHead[16], cRows[ROWS][80];
static int16_t tabX[N_SERVICES + 1], tabW[N_SERVICES + 1];
static uint8_t tabSvc[N_SERVICES + 1], nTabs = 0;
static Preferences prefs;
static void saveSettings() {
  prefs.begin("social", false);
  prefs.putUChar("bg", sTheme);
  prefs.putUChar("clk", sClock);
  prefs.end();
}
static void fmtCount(float v, char *out, size_t n) {  // 1,284  12.4K  1.2M
  if (v >= 1e6f) snprintf(out, n, "%.1fM", v / 1e6f);
  else if (v >= 1e4f) snprintf(out, n, "%.1fK", v / 1e3f);
  else if (v >= 1e3f) snprintf(out, n, "%d,%03d", (int)v / 1000, (int)v % 1000);
  else snprintf(out, n, "%.0f", v);
}
static void takeAccounts() {
  xSemaphoreTake(mux, portMAX_DELAY);
  memcpy(shown, acc, sizeof(Account) * nAcc);
  shownVer = accVer;
  xSemaphoreGive(mux);
  nShown = 0;
  for (uint8_t i = 0; i < nAcc; i++)
    if (sect == 0 || shown[i].svc == tabSvc[sect]) order[nShown++] = i;
}
static void drawTabs() {
  gfx->fillRect(0, 0, 640, Y_ROW0 - 1, C_BG);
  backMark(14, 22, C_MUTED);  // home
  int16_t x = 36;
  for (uint8_t i = 0; i < nTabs; i++) {
    const char *name = i == 0 ? "ALL" : SVC_TABS[tabSvc[i]];
    int16_t w = textWidth(1, name) + 26;
    tabX[i] = x;
    tabW[i] = w;
    bool on = i == sect;
    textAt(x + 13, 12, 1, on ? C_FG : C_DIM, name);
    if (on) gfx->fillRect(x + 9, 34, w - 18, 3, C_FG);
    x += w;
  }
  settingsIcon(676, C_MUTED);
  gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
}
static int8_t hitTab(int16_t x) {
  for (uint8_t i = 0; i < nTabs; i++)
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
// Logos: /logo/N/svc_<service>.565 (tools/make-service-logos.py), cached in PSRAM.
struct LogoCache { uint8_t size; char label[16]; uint16_t *px; };
static LogoCache logoCache[24];
static uint8_t nLogoCache = 0;
static uint16_t *logoLoad(uint8_t size, const char *label) {
  for (uint8_t i = 0; i < nLogoCache; i++)
    if (logoCache[i].size == size && strcmp(logoCache[i].label, label) == 0) return logoCache[i].px;
  if (nLogoCache >= 24) return nullptr;
  char path[40];
  snprintf(path, sizeof path, "/logo/%u/%s.565", size, label);
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  const size_t want = (size_t)size * size * 2;
  uint16_t *px = f.size() == want ? (uint16_t *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM) : nullptr;
  bool ok = px && f.read((uint8_t *)px, want) == want;
  f.close();
  if (!ok) { if (px) free(px); return nullptr; }
  LogoCache &c = logoCache[nLogoCache++];
  c.size = size;
  snprintf(c.label, sizeof c.label, "%s", label);
  c.px = px;
  return px;
}
// An app icon: a rounded plate in the brand's colour with a thin border,
// the white mark on it at two thirds the size -- or the letter, without a file.
static void svcMark(int16_t x, int16_t y, uint8_t size, uint8_t svc) {
  gfx->fillRoundRect(x, y, size, size, size / 5, SVC_COLOUR[svc]);
  gfx->drawRoundRect(x, y, size, size, size / 5, towardsWhite(SVC_COLOUR[svc], 35));
  char key[20];
  uint8_t inner = size * 2 / 3;
  snprintf(key, sizeof key, "svc_%s", SVC_NAMES[svc]);
  uint16_t *px = logoLoad(inner, key);
  if (px) { gfx->draw16bitRGBBitmapWithTranColor(x + (size - inner) / 2, y + (size - inner) / 2, px, 0x0000, inner, inner); return; }
  char c[2] = {(char)toupper(SVC_NAMES[svc][0]), 0};
  uint8_t f = size >= 64 ? 3 : 2;
  textAt(x + (size - textWidth(f, c)) / 2, y + (size - FACES[f - 1].cap) / 2, f, svc == S_GITHUB ? 0x0000 : C_FG, c);
}
static void sparkline(int16_t x, int16_t y, int16_t w, int16_t h, const Account &a) {
  gfx->fillRect(x, y, w, h, C_BG);
  if (a.n < 2) {
    for (int16_t i = 0; i < w; i += 6) gfx->drawFastHLine(x + i, y + h / 2, 3, C_DIM);
    return;
  }
  float lo = a.hist[0], hi = a.hist[0];
  for (uint8_t i = 1; i < a.n; i++) { lo = min(lo, a.hist[i]); hi = max(hi, a.hist[i]); }
  uint16_t c = a.hist[a.n - 1] >= a.hist[0] ? C_GOOD : C_BAD;
  int16_t px = x, py = hi > lo ? y + (int16_t)((1 - (a.hist[0] - lo) / (hi - lo)) * (h - 1)) : y + h / 2;
  for (uint8_t i = 1; i < a.n; i++) {
    int16_t nx = x + (int32_t)i * (w - 1) / (a.n - 1);
    int16_t ny = hi > lo ? y + (int16_t)((1 - (a.hist[i] - lo) / (hi - lo)) * (h - 1)) : y + h / 2;
    gfx->drawLine(px, py, nx, ny, c);
    px = nx;
    py = ny;
  }
}
static void drawRow(uint8_t slot) {
  int16_t y = Y_ROW0 + slot * ROW_H;
  uint8_t i = listTop + slot;
  char key[80] = "";
  if (i < nShown) {
    const Account &a = shown[order[i]];
    snprintf(key, sizeof key, "%s|%d|%.0f|%.0f|%u", a.label, a.valid, a.count, deltaToday(a), a.n);
  }
  if (strcmp(key, cRows[slot]) == 0) return;
  strcpy(cRows[slot], key);
  gfx->fillRect(0, y, LCD_W, ROW_H, C_BG);
  if (i >= nShown) return;
  const Account &a = shown[order[i]];
  svcMark(12, y + 6, 32, a.svc);
  textAt(56, y + 12, 2, C_FG, a.label);
  textAt(56 + textWidth(2, a.label) + 10, y + 15, 1, C_DIM, a.valid ? a.what : "fetching...");
  sparkline(300, y + 6, 330, 32, a);
  if (a.valid) {
    char b[16];
    fmtCount(a.count, b, sizeof b);
    textAt(692 - textWidth(2, b), y + 12, 2, C_FG, b);
    float d = deltaToday(a);
    if (a.n >= 2) {
      snprintf(b, sizeof b, "%+.0f", d);
      textAt(X_RIGHT - textWidth(2, b), y + 12, 2, d > 0 ? C_GOOD : d < 0 ? C_BAD : C_MUTED, b);
    } else {
      textAt(X_RIGHT - textWidth(1, "new"), y + 15, 1, C_DIM, "new");
    }
  }
  gfx->drawFastHLine(20, y + ROW_H - 1, LCD_W - 40, C_RULE);
}
static void drawList(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    drawTabs();
    cHead[0] = '\0';
    for (uint8_t s = 0; s < ROWS; s++) cRows[s][0] = '\0';
    drawHint(nAcc ? "drag to scroll        tap an account        the change is since yesterday" : "config.json has no accounts");
  }
  drawClock();
  if (shownVer != accVer) takeAccounts();
  if (listTop + ROWS > nShown) listTop = nShown > ROWS ? nShown - ROWS : 0;
  for (uint8_t s = 0; s < ROWS; s++) drawRow(s);
}
static int16_t hitRow(int16_t y) {
  if (y < Y_ROW0 || y >= Y_ROW0 + ROWS * ROW_H) return -1;
  uint8_t s = (y - Y_ROW0) / ROW_H;
  return listTop + s < nShown ? listTop + s : -1;
}
static uint8_t wrapText(const char *s, int16_t w, char out[][64], uint8_t lines) {
  uint8_t n = 0;
  char line[64] = "";
  const char *p = s;
  while (*p && n < lines) {
    const char *e = p;
    while (*e && *e != ' ') e++;
    char cand[64];
    snprintf(cand, sizeof cand, "%s%s%.*s", line, line[0] ? " " : "", (int)(e - p), p);
    if (textWidth(1, cand) <= w || !line[0]) strcpy(line, cand);
    else { strcpy(out[n++], line); line[0] = '\0'; continue; }
    p = *e ? e + 1 : e;
  }
  if (line[0] && n < lines) strcpy(out[n++], line);
  if (*p && n == lines) { size_t l = strlen(out[n - 1]); if (l > 3) strcpy(out[n - 1] + l - 3, "..."); }
  return n;
}
static void drawDetail(bool full) {
  static char kDetail[96];
  if (detailIdx >= nShown) { view = View::List; drawList(true); return; }
  const Account &a = shown[order[detailIdx]];
  if (full) {
    pageHeader(a.label, SVC_TABS[a.svc]);
    drawHint("< next        v list        prev >");
    kDetail[0] = '\0';
  }
  drawClock();
  char key[96];
  snprintf(key, sizeof key, "%s|%d|%.0f|%.0f|%.0f|%u", a.label, a.valid, a.count, a.likes, deltaToday(a), a.n);
  if (strcmp(key, kDetail) == 0) return;
  strcpy(kDetail, key);
  gfx->fillRect(0, Y_ROW0, LCD_W, Y_HINT - 8 - Y_ROW0, C_BG);
  svcMark(20, 56, 96, a.svc);
  char b[24];
  if (!a.valid) { textAt(140, 90, 2, C_DIM, "fetching..."); return; }
  fmtCount(a.count, b, sizeof b);
  textAt(140, 52, 4, C_FG, b);
  textAt(140 + textWidth(4, b) + 14, 90, 2, C_MUTED, a.what);
  float d = deltaToday(a);
  if (a.n >= 2) {
    snprintf(b, sizeof b, "%+.0f today", d);
    textAt(140, 122, 2, d > 0 ? C_GOOD : d < 0 ? C_BAD : C_MUTED, b);
  } else {
    textAt(140, 122, 2, C_DIM, "first day: the change shows tomorrow");
  }
  if (a.n >= 8) {
    snprintf(b, sizeof b, "%+.0f this week", a.count - a.hist[a.n - 8]);
    textAt(340, 122, 2, C_MUTED, b);
  }
  const char *second = a.svc == S_GITHUB ? (a.repo ? "forks" : "repos") : a.svc == S_NPM ? "downloads/day" : a.svc == S_YOUTUBE ? nullptr : "posts";
  if (second) {
    fmtCount(a.second, b, sizeof b);
    char l[32];
    snprintf(l, sizeof l, "%s %s", b, second);
    textAt(140, 156, 1, C_DIM, l);
  }
  // the chart: the history, large
  gfx->drawRect(19, 189, LCD_W - 38, 102, C_RULE);
  sparkline(24, 194, LCD_W - 48, 92, a);
  textAt(24, 194, 1, C_DIM, a.n >= 2 ? "30 days" : "the board keeps one number a day; the line grows from here");
  // the latest post, or the video, with its numbers
  if (a.latest[0]) {
    const char *head = a.svc == S_YOUTUBE ? "latest video" : a.svc == S_GITHUB ? (a.repo ? "about" : "bio") : "latest post";
    textAt(20, 306, 1, C_DIM, head);
    char lines[3][64];
    uint8_t n = wrapText(a.latest, LCD_W - 40, lines, 3);
    for (uint8_t i = 0; i < n; i++) textAt(20, 326 + i * 20, 1, i == 0 ? C_FG : C_MUTED, lines[i]);
    char nums[48] = "";
    if (a.svc == S_BLUESKY || a.svc == S_MASTODON) snprintf(nums, sizeof nums, "%.0f likes   %.0f reposts   %.0f replies", a.likes, a.reposts, a.replies);
    else if (a.svc == S_YOUTUBE && a.likes > 0) snprintf(nums, sizeof nums, "%.0f likes", a.likes);
    else if (a.svc == S_GITHUB && a.repo) snprintf(nums, sizeof nums, "%.0f open issues", a.likes);
    if (nums[0]) textAt(20, 326 + n * 20 + 6, 2, C_GOLD, nums);
  }
}
// The home: a tile per service in use -- its logo, its name, the first
// account's number -- and a tap opens it: the account when there is one,
// the list of them when there are more.
static char kHome[N_SERVICES][32];
static void homeTile(uint8_t i, bool force) {
  uint8_t svc = tabSvc[i + 1];
  int16_t x, y, w, h;
  tileRect(i, nTabs - 1, nTabs - 1 > 3 ? 3 : nTabs - 1, &x, &y, &w, &h);
  const Account *first = nullptr;
  uint8_t count = 0;
  for (uint8_t k = 0; k < nAcc; k++)
    if (shown[k].svc == svc) { if (!first) first = &shown[k]; count++; }
  char key[32];
  snprintf(key, sizeof key, "%d|%.0f|%u", first && first->valid, first ? first->count : 0, count);
  if (!force && strcmp(key, kHome[i]) == 0) return;
  strcpy(kHome[i], key);
  gfx->fillRoundRect(x, y, w, h, 14, towardsWhite(C_BG, 6));
  gfx->drawRoundRect(x, y, w, h, 14, C_RULE);
  svcMark(x + (w - 96) / 2, y + 16, 96, svc);
  textAt(x + (w - textWidth(2, SVC_TABS[svc])) / 2, y + 124, 2, C_FG, SVC_TABS[svc]);
  char b[40] = "fetching...";
  if (first && first->valid) {
    char n[16];
    fmtCount(first->count, n, sizeof n);
    snprintf(b, sizeof b, "%s %s", n, first->what);
  }
  fieldCentre(x + w / 2, y + 152, 22, 1, C_MUTED, b);
  if (count > 1) {
    snprintf(b, sizeof b, "%u accounts", count);
    fieldCentre(x + w / 2, y + 172, 12, 1, C_DIM, b);
  }
}
static void drawHome(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    textAt(20, 10, 2, C_FG, "SOCIAL");
    settingsIcon(676, C_MUTED);
    gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
    cHead[0] = '\0';
    drawHint("tap a service");
  }
  drawClock();
  if (shownVer != accVer) takeAccounts();
  for (uint8_t i = 0; i + 1 < nTabs; i++) homeTile(i, full);
}
static int8_t hitHome(int16_t tx, int16_t ty) {
  for (uint8_t i = 0; i + 1 < nTabs; i++) {
    int16_t x, y, w, h;
    tileRect(i, nTabs - 1, nTabs - 1 > 3 ? 3 : nTabs - 1, &x, &y, &w, &h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return i;
  }
  return -1;
}
static void openService(uint8_t tab) {  // tab: index into tabSvc, 1..
  sect = tab;
  listTop = 0;
  shownVer = 0;
  takeAccounts();
  if (nShown == 1) { detailIdx = 0; view = View::Detail; drawDetail(true); }
  else { view = View::List; drawList(true); }
}
static void drawSplash(const char *status) {
  gfx->fillScreen(C_BG);
  const char *name = "SOCIAL";  // the name alone, centred; nothing else to look at
  textAt((LCD_W - textWidth(5, name)) / 2, (LCD_H - FACES[4].cap) / 2 - 20, 5, C_FG, name);
  gfx->fillRect(LCD_W / 2 - 40, (LCD_H - FACES[4].cap) / 2 - 20 + FACES[4].cap + 22, 80, 3, C_GOLD);
  const char *credit = "made by Richard Torcato";
  textAt((LCD_W - textWidth(1, credit)) / 2, 424, 1, C_MUTED, credit);
  drawHint(status);
}
static void drawSettings() {
  pageHeader("SETTINGS");
  settingRow(0, "Clock", CLOCK_NAMES[sClock]);
  settingRow(1, "Theme", THEMES[sTheme].name);
  settingRow(2, "Info", "");
  settingRow(3, "Wi-Fi", wifiSsid[0] ? wifiSsid : "not set");
  settingRow(4, "Shut down", "", C_BAD);
  drawHint("< home        Wi-Fi is set up in Ticker Tape");
}
static void drawInfo() {
  pageHeader("INFO");
  char l[6][48];
  uint8_t n = 0;
  snprintf(l[n++], 48, "%u accounts, refreshed every %lu min", nAcc, (unsigned long)(refreshMs / 60000));
  snprintf(l[n++], 48, "history: one number a day per account, /history.json");
  snprintf(l[n++], 48, "wifi %.16s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  snprintf(l[n++], 48, "heap %uk free, psram %uk free", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  snprintf(l[n++], 48, "built " __DATE__ " " __TIME__);
  for (uint8_t i = 0; i < n; i++) textAt(20, 60 + i * 26, 1, i == 0 ? C_FG : C_MUTED, l[i]);
  drawHint("< settings");
}
static void openConfirm() {
  confirmFrom = view;
  if (view == View::List || view == View::Home) { gfx->fillScreen(C_BG); if (view == View::List) drawTabs(); }
  view = View::Confirm;
  drawShutdownSheet();
}
static void closeConfirm() {
  sheetClose();
  view = confirmFrom;
  if (view == View::List) drawList(true);
  else if (view == View::Home) drawHome(true);
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
  if (up) { wifiRetries = 0; lastRetry = 0; }
  else if (millis() - joinStarted > 20000 && (lastRetry == 0 || millis() - lastRetry > netRetryDelay(wifiRetries, 20000))) {
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
  buf = (uint8_t *)heap_caps_malloc(CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // malloc over 16KB lands in PSRAM by itself
  acc = (Account *)heap_caps_calloc(MAX_ACC, sizeof(Account), MALLOC_CAP_SPIRAM);
  shown = (Account *)heap_caps_calloc(MAX_ACC, sizeof(Account), MALLOC_CAP_SPIRAM);
  // config.json (committed, a demo) and then config.local.json (yours, gitignored), the same shape
  for (const char *path : {"/config.json", "/config.local.json"}) {
    if (!cfgLoad(path)) continue;
    for (JsonObject o : cfgArr("accounts")) {
      if (nAcc >= MAX_ACC) break;
      const char *svc = o["service"] | "";
      int8_t s = -1;
      for (uint8_t k = 0; k < N_SERVICES; k++) if (strcmp(svc, SVC_NAMES[k]) == 0) s = k;
      if (s < 0) { Serial.printf("skipped account: service '%s' unknown\n", svc); continue; }
      Account &a = acc[nAcc];
      memset(&a, 0, sizeof a);
      a.svc = (uint8_t)s;
      a.repo = o["repo"] | false;
      snprintf(a.id, sizeof a.id, "%s", o["id"] | "");
      snprintf(a.label, sizeof a.label, "%s", o["label"] | a.id);
      bool dup = false;  // the same account in both files: once
      for (uint8_t k = 0; k < nAcc && !dup; k++) dup = acc[k].svc == a.svc && strcmp(acc[k].id, a.id) == 0;
      if (a.id[0] && !dup) { svcUsed[s] = true; nAcc++; }
    }
    refreshMs = (uint32_t)cfgInt("refreshMinutes", refreshMs / 60000, 1, 1440) * 60000UL;
    cfgStr("tz", tzString, sizeof tzString);
    cfgRelease();
  }
  nTabs = 0;
  tabSvc[nTabs++] = 0;  // ALL
  for (uint8_t s = 0; s < N_SERVICES; s++) if (svcUsed[s]) tabSvc[nTabs++] = s;
  Serial.printf("social: %u accounts, refresh %lu min\n", nAcc, (unsigned long)(refreshMs / 60000));
  histLoad();
  prefs.begin("ticker", true);
  prefs.getString("ssid", wifiSsid, sizeof wifiSsid);
  prefs.getString("pass", wifiPass, sizeof wifiPass);
  prefs.end();
  prefs.begin("social", true);
  sTheme = prefs.getUChar("bg", 0) % N_THEMES;
  sClock = prefs.getUChar("clk", 0) % 2;
  prefs.end();
  applyTheme();
  setenv("TZ", tzString, 1);
  tzset();
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
  static uint32_t headerHoldAt = 0;  // two seconds on the header: the shutdown sheet
  if (g == Gesture::LongPress && ty < Y_ROW0 && view != View::Confirm && view != View::Splash) headerHoldAt = millis();
  if (!touchHeld) headerHoldAt = 0;
  if (headerHoldAt && millis() - headerHoldAt > 1300) { headerHoldAt = 0; openConfirm(); g = Gesture::None; }
  if (view == View::Splash) {
    static bool said = false;
    if (WiFi.status() == WL_CONNECTED && !said) { said = true; drawHint("fetching..."); }
    if (accVer || g == Gesture::TapUp) { view = View::Home; drawHome(true); }
  } else if (view == View::Home) {
    if (g == Gesture::TapUp) {
      if (ty < Y_ROW0 && tx >= 660 && tx < 724) { view = View::Settings; drawSettings(); }
      else { int8_t i = hitHome(tx, ty); if (i >= 0) openService((uint8_t)(i + 1)); }
    }
    if (view == View::Home) drawHome(false);
  } else if (view == View::List) {
    if (g == Gesture::Drag && ddy) {
      static int16_t acc_ = 0;
      acc_ += ddy;
      while (acc_ <= -ROW_H / 2 && listTop + ROWS < nShown) { acc_ += ROW_H / 2; listTop++; for (uint8_t s = 0; s < ROWS; s++) drawRow(s); }
      while (acc_ >= ROW_H / 2 && listTop > 0) { acc_ -= ROW_H / 2; listTop--; for (uint8_t s = 0; s < ROWS; s++) drawRow(s); }
      if (listTop == 0 && acc_ > 0) acc_ = 0;
      if (listTop + ROWS >= nShown && acc_ < 0) acc_ = 0;
    } else if (g == Gesture::TapUp) {
      if (ty < Y_ROW0) {
        if (tx < 36) { view = View::Home; drawHome(true); }
        else if (tx >= 660 && tx < 724) { view = View::Settings; drawSettings(); }
        else { int8_t t = hitTab(tx); if (t >= 0 && t != sect) { sect = (uint8_t)t; listTop = 0; shownVer = 0; drawList(true); } }
      } else {
        int16_t r = hitRow(ty);
        if (r >= 0) { detailIdx = (uint8_t)r; view = View::Detail; drawDetail(true); }
      }
    } else if (g == Gesture::SwipeLeft && nTabs > 1) { sect = (sect + 1) % nTabs; listTop = 0; shownVer = 0; drawList(true); }
    else if (g == Gesture::SwipeRight && nTabs > 1) { sect = (sect + nTabs - 1) % nTabs; listTop = 0; shownVer = 0; drawList(true); }
    if (view == View::List) drawList(false);
  } else if (view == View::Detail) {
    if (g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) { view = View::List; drawList(true); }
    else if (g == Gesture::SwipeLeft && nShown) { detailIdx = (detailIdx + 1) % nShown; drawDetail(true); }
    else if (g == Gesture::SwipeRight && nShown) { detailIdx = (detailIdx + nShown - 1) % nShown; drawDetail(true); }
    if (view == View::Detail) { if (shownVer != accVer) takeAccounts(); drawDetail(false); }
  } else if (view == View::Settings) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) { view = View::Home; drawHome(true); }
    else if (g == Gesture::TapUp) {
      int8_t i = hitSettingRow(ty, 5);
      if (i == 0) { chN = 2; chSel = sClock; view = View::Choice; drawChoiceSheet("CLOCK", CLOCK_NAMES, chN, chSel); }
      else if (i == 1) { view = View::Themes; drawThemesPage(); }
      else if (i == 2) { view = View::Info; drawInfo(); }
      else if (i == 4) openConfirm();
    }
  } else if (view == View::Themes) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) { view = View::Settings; drawSettings(); }
    else if (g == Gesture::TapUp) { int8_t i = hitTheme(tx, ty); if (i >= 0 && i != sTheme) { sTheme = (uint8_t)i; applyTheme(); saveSettings(); drawThemesPage(); } }
  } else if (view == View::Info) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || g == Gesture::TapUp) { view = View::Settings; drawSettings(); }
  } else if (view == View::Choice) {
    if (g == Gesture::TapUp || g == Gesture::Tap) {
      int8_t i = hitChoice(tx, ty, chN);
      if (i >= 0 && i != chSel) { sClock = (uint8_t)i; saveSettings(); }
      if (i >= 0 || !inSheet(tx, ty)) { sheetClose(); view = View::Settings; drawSettings(); }
    } else if (g == Gesture::SwipeDown || g == Gesture::SwipeLeft) { sheetClose(); view = View::Settings; }
  } else if (view == View::Confirm) {
    if (g == Gesture::TapUp || g == Gesture::Tap) { if (hitPill(tx, ty)) shutDown(); else closeConfirm(); }
    else if (g == Gesture::SwipeDown || g == Gesture::SwipeLeft) closeConfirm();
  }
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 60000) {
    lastLog = millis();
    Serial.printf("heap %u  psram %u  wifi %s\n", ESP.getFreeHeap(), ESP.getFreePsram(), WiFi.status() == WL_CONNECTED ? "up" : "DOWN");
  }
  delay(20);
}
