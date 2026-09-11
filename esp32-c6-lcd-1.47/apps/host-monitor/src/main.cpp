// host-monitor: stats panel for any machine running one of the agents.
//
// Nothing here is OS-specific: it parses a fixed set of JSON keys, so the agent
// is the only part that knows what a "load average" means on a given platform.
// See agent/macos.py and agent/linux.py -- the JSON contract is the boundary.
//
// Read-only on purpose. The sleep/wake button described in the README is phase
// two: it needs a token, a bound interface and least privilege, and none of
// that has to exist for the panel to be useful. Also worth knowing before
// adding it -- BOOT's three gestures are already spent on rotation, colour
// scheme and blanking (see lib/board/ui.h), so a sleep action needs a fourth
// gesture or a different input.
//
// Follows APP-CHECKLIST.md: ui.h for scheme/rotation/gestures, a Layout struct
// per orientation, exact-box redraws with no fillScreen() in loop(), a
// self-check over the pure formatters and both layouts, and an offline screen
// that names the likely cause.
//
// Needs one of apps/host-monitor/agent/ running on the host being watched.
#include <board.h>
#include <netjoin.h>
#include <ui.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── config ───────────────────────────────────────────────────────────────
// Which host to watch. Override in secrets.h rather than editing here. Plain
// HTTP: it's a read-only LAN endpoint exposing no secrets, and skipping TLS
// saves ~40KB of heap.
#ifndef HOST_AGENT_URL
#define HOST_AGENT_URL "http://10.0.10.92:8787/stats"
#endif

// The compiled-in URL is only the default. Hold BOOT 6s to open a setup portal
// and change it at runtime; the choice lives in NVS. A `.local` hostname is
// resolved over mDNS, which survives DHCP moving the host.
static Preferences cfg;
static char agentUrl[96];
static char resolvedUrl[96];  // agentUrl with any .local name turned into an IP

static const char *PORTAL_SSID = "host-monitor-setup";
static const char *PORTAL_PASS = "setup-panel";  // >=8 chars, WPA2
static const uint32_t PORTAL_TIMEOUT_MS = 3UL * 60 * 1000;

static const uint32_t POLL_MS = 5UL * 1000;        // agent is cheap; 5s feels live
static const uint32_t POLL_RETRY_MS = 10UL * 1000; // back off a little when failing
static const uint32_t WIFI_RETRY_MS = 20UL * 1000; // re-begin() while disconnected
static const uint32_t TEMP_LOG_MS = 60UL * 1000;

#define POWER_SAVE 1

// Set to 1 to render a fixed set of plausible stats without an agent. Useful
// for checking layout in both orientations before the network path works --
// values are a real capture from an M4 mini.
#define DEMO_STATS 0

#define GW(size) (6 * (size))
#define GH(size) (8 * (size))

static Arduino_GFX *gfx;

// ── layout ───────────────────────────────────────────────────────────────
// Content lives in generic "slots" rather than named fields, because the two
// pages put different things in the same places. A slot is a small label with a
// large value under it, and optionally a bar under that.
//
// Two pages exist so the numbers can be big: less per screen means size 3 and 4
// glyphs instead of size 1 rows. They auto-cycle, since this board has one
// button and its three gestures are already spent.
struct Layout {
  bool landscape;
  int16_t w, h;
  int16_t xHost, yHost;
  int16_t xDots, yDots;
  int16_t slotX[4], slotY[4], slotW[4];
  int16_t xStatus, yStatus, yStale;
};

// 172x320: four slots stacked. 68px pitch fits label + size-4 value + bar.
static const Layout PORTRAIT = {
    false, 172, 320,
    /*host*/ 8, 6,
    /*dots*/ 150, 6,
    /*slotX*/ {8, 8, 8, 8},
    /*slotY*/ {26, 94, 162, 230},
    /*slotW*/ {156, 156, 156, 156},
    /*status*/ 8, 296, 308,
};

// 320x172: the same four slots as a 2x2 grid. 148px wide still fits 8 glyphs
// at size 3, which is what "196 kB/s" needs.
static const Layout LANDSCAPE = {
    true, 320, 172,
    /*host*/ 8, 4,
    /*dots*/ 298, 4,
    /*slotX*/ {8, 164, 8, 164},
    /*slotY*/ {22, 22, 90, 90},
    /*slotW*/ {148, 148, 148, 148},
    /*status*/ 8, 150, 160,
};

static const Layout *L = &PORTRAIT;
static void syncLayout() { L = uiLandscape() ? &LANDSCAPE : &PORTRAIT; }

// Page cycling. 6s each: long enough to read, short enough that you don't wait
// for the number you wanted.
static const uint32_t PAGE_MS = 6UL * 1000;
static const uint8_t PAGES = 2;
static uint8_t page = 0;

// ── stats ────────────────────────────────────────────────────────────────
struct Stats {
  char host[18];
  uint32_t uptime_s;
  float load1, load5, load15;
  int cpu_pct, mem_pct;
  float diskFreeGb, diskTotalGb;
  float downBps, upBps;
  char topName[16];
  float topPct;
  bool valid;
  uint32_t fetchedAt;
  uint16_t fails;
};
static Stats st = {};

// ── pure formatters (what selfCheck covers) ──────────────────────────────
// "4d 22:39" once past a day, else "22:39:12". Days matter for a machine that
// is meant to stay up; seconds matter when it hasn't.
//
// Must never exceed 8 characters: at size 3 that is 144px, and the widest slot
// is 156px. Past 10 days the minutes are dropped to stay inside that budget --
// nothing about a machine up that long needs minute precision. selfCheck()
// asserts the bound across the whole range.
static void formatUptime(uint32_t s, char *out, size_t n) {
  uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, sec = s % 60;
  if (d >= 1000) snprintf(out, n, "%lud", (unsigned long)d);
  else if (d >= 10) snprintf(out, n, "%lud %02luh", (unsigned long)d, (unsigned long)h);
  else if (d > 0) snprintf(out, n, "%lud %02lu:%02lu", (unsigned long)d, (unsigned long)h,
                           (unsigned long)m);
  else snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m,
                (unsigned long)sec);
}

// Rates are for glancing at, so one unit change and at most one decimal.
//
// Must never exceed 10 characters: the field is 13 wide and "dn " takes three.
// The GB/s tier exists because without it a gigabit link renders
// "1000.0 MB/s" at 11 characters -- selfCheck() caught that, not the panel.
static void formatRate(float bps, char *out, size_t n) {
  if (bps < 1000.0f) snprintf(out, n, "%.0f B/s", bps);
  else if (bps < 1.0e6f) snprintf(out, n, "%.0f kB/s", bps / 1000.0f);
  else if (bps < 1.0e9f) snprintf(out, n, "%.1f MB/s", bps / 1.0e6f);
  else snprintf(out, n, "%.1f GB/s", bps / 1.0e9f);
}

// ── drawing ──────────────────────────────────────────────────────────────
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg,
                  const char *s, bool center = false) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(x, y, w, GH(size), uiTheme()->bg);
  int16_t tx = center ? x + (w - (int16_t)(GW(size) * strlen(s))) / 2 : x;
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(tx, y);
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

// Green below 60%, amber to 85, red above. Thresholds do more for a glanceable
// panel than any amount of bar styling.
static uint16_t levelColor(int pct) {
  if (pct >= 85) return uiTheme()->bad;
  if (pct >= 60) return uiTheme()->warn;
  return uiTheme()->good;
}

static void bar(int16_t x, int16_t y, int16_t w, int pct, uint16_t fg) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  gfx->fillRect(x, y, w, 3, uiTheme()->rule);
  gfx->fillRect(x, y, (int16_t)((int32_t)w * pct / 100), 3, fg);
}

// One slot: small label, large value, optional bar. Everything is cached on
// "label|value|pct" -- drawPage() runs every loop iteration, and an unguarded
// field() erases and repaints at ~10Hz, which reads as flicker. That is exactly
// what the header did before it was cached.
static char cSlot[4][40], cHost[26], cStatus[34], cStale[34], cKey[48];
static int8_t cDots = -1;

static void invalidateCache() {
  for (int i = 0; i < 4; i++) cSlot[i][0] = '\0';
  cHost[0] = cStatus[0] = cStale[0] = cKey[0] = '\0';
  cDots = -1;
}

static void drawSlot(uint8_t i, const char *label, const char *value, uint8_t vs,
                     bool withBar, int pct, uint16_t fg) {
  int16_t x = L->slotX[i], y = L->slotY[i], w = L->slotW[i];
  char key[40];
  snprintf(key, sizeof key, "%s|%s|%d|%d", label, value, withBar ? pct : -1, vs);
  if (strcmp(key, cSlot[i]) == 0) return;
  strcpy(cSlot[i], key);

  field(x, y, w / GW(1), 1, uiTheme()->dim, label);
  field(x, y + 12, w / GW(vs), vs, fg, value);
  int16_t barY = y + 12 + GH(vs) + 5;
  if (withBar) {
    bar(x, barY, w, pct, levelColor(pct));
  } else {
    // Erase any bar the other page drew here, or it lingers under a value that
    // has nothing to do with it.
    gfx->fillRect(x, barY, w, 3, uiTheme()->bg);
  }
}

// Which page is showing, as dots rather than "1/2" -- less to read.
static void drawDots() {
  if (cDots == (int8_t)page) return;
  cDots = page;
  for (uint8_t i = 0; i < PAGES; i++) {
    gfx->fillCircle(L->xDots + i * 9, L->yDots + 3, 3,
                    i == page ? uiTheme()->fg : uiTheme()->rule);
  }
}

static void drawChrome() { gfx->fillScreen(uiTheme()->bg); }

static void drawPage() {
  char v[24], r[24];

  // Header: host plus a reachability dot.
  snprintf(v, sizeof v, "%c%s", st.valid ? '+' : '-', st.valid ? st.host : "no host");
  if (strcmp(v, cHost) != 0) {
    strcpy(cHost, v);
    field(L->xHost + 12, L->yHost, 14, 1, uiTheme()->fg, v + 1);
    gfx->fillCircle(L->xHost + 4, L->yHost + 3, 3,
                    st.valid ? uiTheme()->good : uiTheme()->bad);
  }
  drawDots();

  if (!st.valid) {
    drawSlot(0, "UPTIME", "--", 3, false, 0, uiTheme()->dim);
    drawSlot(1, "CPU", "--", 4, false, 0, uiTheme()->dim);
    drawSlot(2, "MEMORY", "--", 4, false, 0, uiTheme()->dim);
    drawSlot(3, "WAITING", "--", 3, false, 0, uiTheme()->dim);
    return;
  }

  if (page == 0) {
    formatUptime(st.uptime_s, v, sizeof v);
    drawSlot(0, "UPTIME", v, 3, false, 0, uiTheme()->fg);

    snprintf(v, sizeof v, "%d%%", st.cpu_pct);
    drawSlot(1, "CPU", v, 4, true, st.cpu_pct, uiTheme()->fg);

    snprintf(v, sizeof v, "%d%%", st.mem_pct);
    drawSlot(2, "MEMORY", v, 4, true, st.mem_pct, uiTheme()->fg);

    // Load is per-core, so scale against cores to make the bar mean something.
    // 10 cores assumed for an M4 mini; wrong only changes the bar, not the number.
    snprintf(v, sizeof v, "%.2f", st.load1);
    drawSlot(3, "LOAD 1MIN", v, 3, true, (int)(st.load1 * 100 / 10), uiTheme()->fg);
  } else {
    int diskPct = st.diskTotalGb > 0
                      ? (int)((st.diskTotalGb - st.diskFreeGb) * 100 / st.diskTotalGb)
                      : 0;
    snprintf(v, sizeof v, "%.0fG", st.diskFreeGb);
    drawSlot(0, "DISK FREE", v, 3, true, diskPct, uiTheme()->fg);

    formatRate(st.downBps, r, sizeof r);
    drawSlot(1, "DOWN", r, 3, false, 0, uiTheme()->rain);

    formatRate(st.upBps, r, sizeof r);
    drawSlot(2, "UP", r, 3, false, 0, uiTheme()->sun);

    snprintf(v, sizeof v, "%.12s", st.topName);
    drawSlot(3, "BUSIEST", v, 2, true, (int)st.topPct, uiTheme()->fg);
  }
}

// ── offline panel ────────────────────────────────────────────────────────
static void panelLine(uint8_t r, uint16_t fg, const char *s) {
  field(8, 62 + r * 11, (L->w / GW(1)) - 2, 1, fg, s);
}

static void drawPanel(const char *title, uint16_t tc, const char *const *lines, uint8_t n) {
  gfx->fillScreen(uiTheme()->bg);
  field(8, 24, (L->w / GW(1)) - 2, 3, tc, title);
  gfx->drawFastHLine(8, 54, L->w - 16, uiTheme()->rule);
  uint8_t maxRows = (L->h - 62 - 16) / 11;
  for (uint8_t i = 0; i < n && i < maxRows; i++) panelLine(i, uiTheme()->muted, lines[i]);
}

static void drawNoWifi() {
  char ssid[40];
  snprintf(ssid, sizeof ssid, "SSID %s", WIFI_SSID);
  const char *lines[] = {ssid, "", "Not associated.", "This board is 2.4GHz", "only.",
                         "", "retrying every 20s"};
  drawPanel("NO WIFI", uiTheme()->bad, lines, 7);
}

// The interesting failure: the board is on the IoT VLAN and the Mac is not, so
// UniFi blocks this by default. Saying so beats "connection failed".
static void drawNoAgent() {
  char ip[40], url[40], fails[40];
  snprintf(ip, sizeof ip, "board %s", WiFi.localIP().toString().c_str());
  snprintf(url, sizeof url, "%.32s", agentUrl);
  snprintf(fails, sizeof fails, "%u failed polls", st.fails);
  const char *lines[] = {"can't reach the agent",
                         url,
                         ip,
                         fails,
                         "",
                         "1. is an agent running",
                         "   on the host?",
                         "2. IoT VLAN -> LAN is",
                         "   blocked by default",
                         "   in UniFi. Allow it.",
                         "",
                         "retrying..."};
  drawPanel("NO AGENT", uiTheme()->warn, lines, 12);
}

// ── host URL ─────────────────────────────────────────────────────────────
// A `.local` name has to be resolved by mDNS: HTTPClient hands the hostname to
// normal DNS, which does not answer for .local, so the connect just fails.
// Resolving once per poll cycle is cheap and means a host that changes IP keeps
// working without a reflash.
static void resolveAgentUrl() {
  strncpy(resolvedUrl, agentUrl, sizeof resolvedUrl - 1);
  resolvedUrl[sizeof resolvedUrl - 1] = '\0';

  // Find the host portion: after "http://", up to ':' or '/'.
  const char *p = strstr(agentUrl, "://");
  if (!p) return;
  p += 3;
  const char *end = p;
  while (*end && *end != ':' && *end != '/') end++;

  char host[64];
  size_t len = (size_t)(end - p);
  if (len == 0 || len >= sizeof host) return;
  memcpy(host, p, len);
  host[len] = '\0';

  const char *dot = strstr(host, ".local");
  if (!dot || dot[6] != '\0') return;  // not a .local name; leave the URL alone

  char bare[64];
  size_t bl = (size_t)(dot - host);
  memcpy(bare, host, bl);
  bare[bl] = '\0';

  IPAddress ip = MDNS.queryHost(bare, 2000);
  if (ip == IPAddress((uint32_t)0)) {
    Serial.printf("mdns: %s did not resolve\n", host);
    return;
  }
  // Rebuild the URL with the address substituted for the name.
  snprintf(resolvedUrl, sizeof resolvedUrl, "%.*s%s%s", (int)(p - agentUrl), agentUrl,
           ip.toString().c_str(), end);
  Serial.printf("mdns: %s -> %s\n", host, ip.toString().c_str());
}

// ── network ──────────────────────────────────────────────────────────────
static uint8_t lastReason = 0;

static void onWiFiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r != 0 && r != WIFI_REASON_ASSOC_LEAVE && r != WIFI_REASON_STA_LEAVING) lastReason = r;
  static uint32_t lastLog = 0;
  if (lastLog != 0 && millis() - lastLog < 10000) return;
  lastLog = millis();
  Serial.printf("wifi disconnect reason %u\n", r);
}

static bool fetchStats() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, resolvedUrl)) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("agent http %d (%s)\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  // Read to a String so a parse failure can show what actually arrived.
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("agent json %s -- body[0..160]: %s\n", err.c_str(),
                  body.substring(0, 160).c_str());
    return false;
  }
  // Validate before trusting: a 200 from something that isn't the agent parses
  // fine and the `| default` operators would render it as plausible zeroes.
  if (!doc["uptime_s"].is<uint32_t>()) {
    Serial.printf("agent payload not ours -- body[0..160]: %s\n",
                  body.substring(0, 160).c_str());
    return false;
  }

  snprintf(st.host, sizeof st.host, "%s", doc["host"] | "mac");
  st.uptime_s = doc["uptime_s"] | 0;
  st.load1 = doc["load"][0] | 0.0f;
  st.load5 = doc["load"][1] | 0.0f;
  st.load15 = doc["load"][2] | 0.0f;
  st.cpu_pct = doc["cpu_pct"] | 0;
  st.mem_pct = doc["mem_pct"] | 0;
  st.diskFreeGb = doc["disk_free_gb"] | 0.0f;
  st.diskTotalGb = doc["disk_total_gb"] | 0.0f;
  st.downBps = doc["net_down_bps"] | 0.0f;
  st.upBps = doc["net_up_bps"] | 0.0f;
  snprintf(st.topName, sizeof st.topName, "%s", doc["top_name"] | "-");
  st.topPct = doc["top_pct"] | 0.0f;
  st.valid = true;
  st.fails = 0;
  st.fetchedAt = millis();
  return true;
}

// ── setup portal ─────────────────────────────────────────────────────────
// A brief SoftAP with one form field, so the host can be changed without a
// reflash. WPA2 rather than open: setting a poll URL is not much of a trust
// boundary on a read-only panel, but a nameless open AP is still worth
// avoiding, and the password costs one line. It closes itself after
// PORTAL_TIMEOUT_MS so it cannot be left running.
static void drawPortalScreen(const char *line1, const char *line2) {
  char ssid[40], pass[40], url[40];
  snprintf(ssid, sizeof ssid, "wifi: %s", PORTAL_SSID);
  snprintf(pass, sizeof pass, "pass: %s", PORTAL_PASS);
  snprintf(url, sizeof url, "open: http://%s", WiFi.softAPIP().toString().c_str());
  const char *lines[] = {"Join this network and", "open the page below.", "",
                         ssid, pass, url, "", line1, line2};
  drawPanel("SETUP", uiTheme()->good, lines, 9);
}

static void runPortal() {
  Serial.println("portal: starting");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PORTAL_SSID, PORTAL_PASS);
  delay(300);
  drawPortalScreen("waiting...", "");

  WebServer server(80);
  bool saved = false;

  server.on("/", HTTP_GET, [&server]() {
    String page = F("<!doctype html><meta name=viewport content='width=device-width'>"
                    "<title>host-monitor setup</title>"
                    "<style>body{font:16px system-ui;margin:2rem;max-width:30rem}"
                    "input{width:100%;padding:.6rem;font:inherit}"
                    "button{padding:.6rem 1rem;font:inherit;margin-top:1rem}"
                    "code{background:#eee;padding:.1rem .3rem}</style>"
                    "<h2>host-monitor</h2><form method=POST action=/save>"
                    "<label>Agent URL<input name=url value='");
    page += agentUrl;
    page += F("'></label><button>Save &amp; reboot</button></form>"
              "<p>Run an agent on the host first: <code>python3 macos.py</code> or "
              "<code>python3 linux.py</code>.</p>"
              "<p>A <code>.local</code> name is resolved over mDNS, so it survives the "
              "host changing IP.</p>");
    server.send(200, "text/html", page);
  });

  server.on("/save", HTTP_POST, [&server, &saved]() {
    String u = server.arg("url");
    u.trim();
    if (u.length() < 8 || !u.startsWith("http")) {
      server.send(400, "text/plain", "URL must start with http");
      return;
    }
    u.toCharArray(agentUrl, sizeof agentUrl);
    cfg.putString("url", agentUrl);
    Serial.printf("portal: saved %s\n", agentUrl);
    server.send(200, "text/html", "<p>Saved. Rebooting.</p>");
    saved = true;
  });

  server.begin();
  uint32_t start = millis();
  while (millis() - start < PORTAL_TIMEOUT_MS && !saved) {
    server.handleClient();
    // Any button press cancels, so you are never stuck in the portal.
    if (uiPoll() != UiPress::None) {
      Serial.println("portal: cancelled");
      break;
    }
    delay(5);
  }
  server.stop();
  WiFi.softAPdisconnect(true);
  Serial.println("portal: closing, rebooting");
  delay(200);
  ESP.restart();  // simplest way back to a clean Wi-Fi client state
}

// ── self-check ───────────────────────────────────────────────────────────
static void checkLayout(const Layout *l) {
  for (int i = 0; i < 4; i++) {
    assert(l->slotX[i] >= 0 && l->slotX[i] + l->slotW[i] <= l->w);
    // Label, the tallest value used (size 4), and a bar must all fit above the
    // status strip -- a slot that overruns draws silently off the panel.
    assert(l->slotY[i] + 12 + GH(4) + 5 + 3 <= l->yStatus);
    assert(l->slotW[i] / GW(4) >= 4);  // "100%"
    assert(l->slotW[i] / GW(3) >= 8);  // "196 kB/s", "4d 22:39"
    assert(l->slotW[i] / GW(2) >= 12); // "WindowServer"
  }
  assert(l->xDots + (PAGES - 1) * 9 + 3 <= l->w);
  assert(l->yStale + GH(1) <= l->h);
  assert((l->w / GW(1)) - 2 >= 22);   // offline panel lines
  assert((l->h - 62 - 16) / 11 >= 7); // NO WIFI needs 7 rows
}

static void selfCheck() {
  char b[24];
  formatUptime(0, b, sizeof b);          assert(strcmp(b, "00:00:00") == 0);
  formatUptime(59, b, sizeof b);         assert(strcmp(b, "00:00:59") == 0);
  formatUptime(3661, b, sizeof b);       assert(strcmp(b, "01:01:01") == 0);
  formatUptime(86400, b, sizeof b);      assert(strcmp(b, "1d 00:00") == 0);
  formatUptime(427174, b, sizeof b);     assert(strcmp(b, "4d 22:39") == 0);
  formatUptime(9u * 86400 + 23 * 3600 + 59 * 60, b, sizeof b);
  assert(strcmp(b, "9d 23:59") == 0);
  formatUptime(10u * 86400 + 3600, b, sizeof b);
  assert(strcmp(b, "10d 01h") == 0);
  // Must stay inside the 9-char box the layout reserves, at any uptime. This
  // caught a real 10-char overflow at 999 days -- it panicked at boot instead
  // of clipping on screen, which is the whole point of the check.
  for (uint32_t days : {0u, 1u, 9u, 10u, 99u, 100u, 999u, 4000u}) {
    formatUptime(days * 86400 + 23 * 3600 + 59 * 60 + 59, b, sizeof b);
    assert(strlen(b) <= 8);  // size 3 in a 156px slot
  }

  formatRate(0, b, sizeof b);            assert(strcmp(b, "0 B/s") == 0);
  formatRate(999, b, sizeof b);          assert(strcmp(b, "999 B/s") == 0);
  formatRate(196413, b, sizeof b);       assert(strcmp(b, "196 kB/s") == 0);
  formatRate(1.5e6f, b, sizeof b);       assert(strcmp(b, "1.5 MB/s") == 0);
  formatRate(1.0e9f, b, sizeof b);       assert(strcmp(b, "1.0 GB/s") == 0);
  // "dn " takes 3 of the 13-char field, so the rate itself must fit in 10.
  for (float r : {0.0f, 1.0f, 999.0f, 1000.0f, 999999.0f, 1.0e6f, 9.99e8f, 1.0e9f, 2.5e9f}) {
    formatRate(r, b, sizeof b);
    assert(strlen(b) <= 10);
  }

  assert(levelColor(0) == uiTheme()->good);
  assert(levelColor(70) == uiTheme()->warn);
  assert(levelColor(95) == uiTheme()->bad);

  checkLayout(&PORTRAIT);
  checkLayout(&LANDSCAPE);
  assert(PORTRAIT.w == LCD_NATIVE_W && LANDSCAPE.w == LCD_NATIVE_H);
  assert(PAGES >= 2);
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoWifi, NoAgent, Running };
static State state = State::Boot;

void setup() {
  Serial.begin(115200);
  delay(300);  // USB CDC needs a moment; early prints are lost regardless
  selfCheck();

  gfx = boardDisplay();
  gfx->begin();
  uiBegin(gfx, "hostmon");  // its own NVS namespace
  syncLayout();

  // NVS overrides the compiled-in default, so the portal's choice sticks.
  cfg.begin("hostmon-cfg", false);
  String stored = cfg.getString("url", HOST_AGENT_URL);
  stored.toCharArray(agentUrl, sizeof agentUrl);
  strncpy(resolvedUrl, agentUrl, sizeof resolvedUrl - 1);

  const char *boot[] = {"connecting to wifi", WIFI_SSID, "", "BOOT cycles modes"};
  drawPanel("STARTING", uiTheme()->muted, boot, 4);

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();  // full TX power, MIN_MODEM -- see lib/board/netjoin.h
  netJoinBest(WIFI_SSID, WIFI_PASS);
  // 20s: the WPA2 handshake can time out once and succeed on retry.
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s %ddBm ip %s\n", WiFi.SSID().c_str(), WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
    // Needed before queryHost() can answer, and lets the board be found as
    // host-monitor.local too.
    MDNS.begin("host-monitor");
    resolveAgentUrl();
  } else {
    Serial.printf("wifi FAILED, last reason %u\n", lastReason);
  }

#if DEMO_STATS
  st = {"RT-Mac-Mini-M4", 427174, 2.07f, 2.10f, 2.06f, 19, 74, 475.1f, 994.7f,
        196413.3f, 4499.1f, "WindowServer", 41.0f, true, 0, 0};
  Serial.println("DEMO_STATS: rendering a fixed capture, not polling");
#endif
  Serial.printf("agent url %s (resolved %s)\n", agentUrl, resolvedUrl);
  Serial.println("hold BOOT 6s for setup");
  Serial.println("selfcheck ok");

#if POWER_SAVE
  setCpuFrequencyMhz(80);  // 80MHz is the floor that still supports Wi-Fi
#endif
}

void loop() {
  uiTick();  // deferred NVS write, never in the press path

  UiPress press = uiPoll();
  if (press == UiPress::Setup) {
    runPortal();  // does not return: reboots
  } else if (uiHandle(press)) {
    syncLayout();
    invalidateCache();
    state = State::Boot;
  }

  State want = WiFi.status() != WL_CONNECTED ? State::NoWifi
               : !st.valid                   ? State::NoAgent
                                             : State::Running;

  if (want != state) {
    state = want;
    invalidateCache();
    if (uiScreenOn() && state == State::Running) {
      drawChrome();
      drawPage();
    }
  }

  // setAutoReconnect() alone did not recover from a cold-boot association
  // failure, so re-issue begin() while disconnected.
  static uint32_t lastRetry = 0;
  static uint16_t retries = 0;
  if (state == State::NoWifi) {
    if (lastRetry == 0 || millis() - lastRetry > netRetryDelay(retries, WIFI_RETRY_MS)) {
      lastRetry = millis();
      Serial.printf("wifi retry #%u\n", ++retries);
      WiFi.disconnect();
      netJoinBest(WIFI_SSID, WIFI_PASS);  // re-scan, so a better AP wins
    }
  } else {
    retries = 0;
    lastRetry = 0;
  }

  // Auto-cycle the pages. Only the slots change, so this is four cached
  // redraws, not a full repaint.
  static uint32_t lastPage = 0;
  if (state == State::Running && millis() - lastPage > PAGE_MS) {
    lastPage = millis();
    page = (page + 1) % PAGES;
  }

  if (uiScreenOn()) {
    if (state == State::Running) {
      drawPage();
      char s[34];
      snprintf(s, sizeof s, "%s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
      if (strcmp(s, cStatus) != 0) {
        field(L->xStatus, L->yStatus, 26, 1, uiTheme()->muted, s);
        strcpy(cStatus, s);
      }
      snprintf(s, sizeof s, "polled %lus ago", (millis() - st.fetchedAt) / 1000);
      if (strcmp(s, cStale) != 0) {
        field(L->xStatus, L->yStale, 26, 1, uiTheme()->dim, s);
        strcpy(cStale, s);
      }
    } else {
      char key[48];
      snprintf(key, sizeof key, "%d-%u-%u-%u", (int)state, lastReason, st.fails, uiScheme());
      if (strcmp(key, cKey) != 0) {
        strcpy(cKey, key);
        if (state == State::NoWifi) drawNoWifi();
        else drawNoAgent();
      }
      char up[34];
      snprintf(up, sizeof up, "%lus, retry %u", millis() / 1000, retries);
      if (strcmp(up, cStale) != 0) {
        field(8, L->h - 12, (L->w / GW(1)) - 2, 1, uiTheme()->dim, up);
        strcpy(cStale, up);
      }
    }
  }

  static uint32_t lastPoll = 0;
  uint32_t period = st.valid ? POLL_MS : POLL_RETRY_MS;
  if (!DEMO_STATS && state != State::NoWifi &&
      (lastPoll == 0 || millis() - lastPoll > period)) {
    lastPoll = millis();
    if (!st.valid) resolveAgentUrl();  // a moved host needs re-resolving
    if (fetchStats()) {
      if (uiScreenOn() && state == State::Running) drawPage();
      // Green when healthy, amber when something is loaded, dim red when the
      // Mac is unreachable -- readable from across the room without reading.
      int worst = max(st.cpu_pct, st.mem_pct);
      if (worst >= 85) led(20, 0, 0);
      else if (worst >= 60) led(16, 8, 0);
      else led(0, 14, 4);
    } else {
      st.fails++;
      if (st.fails >= 3) st.valid = false;  // tolerate a blip before blanking
      led(12, 0, 0);
    }
  }

  static uint32_t lastTemp = 0;
  if (millis() - lastTemp > TEMP_LOG_MS) {
    lastTemp = millis();
    Serial.printf("die %.1fC  bl %u  heap %u  fails %u\n", temperatureRead(),
                  uiBacklightApplied, ESP.getFreeHeap(), st.fails);
  }

  delay(100);
}
