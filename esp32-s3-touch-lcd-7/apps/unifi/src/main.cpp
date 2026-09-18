// NETWORK -- a UniFi dashboard for the Waveshare ESP32-S3-Touch-LCD-7.
//
// Reads the UniFi Network application's local Integration API on the
// console (https://<console>/proxy/network/integration/v1, one X-API-Key
// header, self-signed TLS): the site, its devices with their live
// statistics, its clients. An overview of clients, devices and the WAN,
// with the gateway's throughput as a rolling chart; a devices page; a
// clients page. The key lives in data/config.local.json (gitignored):
//   { "unifi": { "host": "10.0.10.1", "apiKey": "..." } }
// made in UniFi Network > Settings > Control Plane > Integrations.
// Built on lib/ui; the Wi-Fi network is the ticker's (NVS "ticker").
#include <appcfg.h>
#include <board.h>
#include <helv.h>
#include <netjoin.h>
#include <ui.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);
static const int16_t Y_ROW0 = UI_Y_ROW0, ROW_H = 44, Y_HINT = UI_Y_HINT, X_RIGHT = 788;
static const uint8_t ROWS = (Y_HINT - 8 - Y_ROW0) / ROW_H;  // 9

// ── config ───────────────────────────────────────────────────────────────
static char host[64] = "unifi", apiKey[80] = "";
static uint32_t statsMs = 10000, devicesMs = 30000, clientsMs = 60000;
static char tzString[48] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static char wifiSsid[33] = "", wifiPass[65] = "";

// ── the site, its devices, its clients ───────────────────────────────────
struct Device {
  char id[40], name[24], model[12], ip[16];
  bool online, gateway;
  uint32_t uptime;
  float cpu, mem, rx, tx;  // percent, percent, bits per second
};
struct NetClient {
  char name[24], ip[16], uplink[40];
  bool wireless;
  time_t since;
};
static const uint8_t MAX_DEV = 32;
static const uint16_t MAX_CLI = 200;
static Device dev[MAX_DEV];
static NetClient *cli = nullptr;  // PSRAM
static uint8_t nDev = 0;
static uint16_t nCli = 0, nWireless = 0;
static char siteId[40] = "", siteName[24] = "";
static uint32_t devAt = 0, cliAt = 0, statsAt = 0, ver = 0;
static char lastErr[48] = "";
static SemaphoreHandle_t mux;
// The gateway's throughput, sampled every statsSeconds: 30 minutes at 10s.
static const uint16_t SAMPLES = 180;
static float rxHist[SAMPLES], txHist[SAMPLES];
static uint16_t nSamples = 0;

// ── fetching (core 0 task) ───────────────────────────────────────────────
static uint8_t *buf = nullptr;
static const size_t CAP = 96 * 1024;  // 200 clients is ~60KB
static int apiGet(NetworkClientSecure &client, const char *path, uint8_t *out, size_t cap) {
  char url[200];
  snprintf(url, sizeof url, "https://%s/proxy/network/integration/v1%s", host, path);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return -1;
  http.addHeader("X-API-Key", apiKey);
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != 200) {
    snprintf(lastErr, sizeof lastErr, "%s: http %d", path, code);
    Serial.printf("unifi %s\n", lastErr);
    http.end();
    return -1;
  }
  size_t len = 0;
  NetworkClient *st = http.getStreamPtr();
  uint32_t last = millis();
  while (millis() - last < 6000 && len < cap) {
    int n = st->available();
    if (n > 0) {
      n = st->read(out + len, min((size_t)n, cap - len));
      if (n > 0) { len += n; last = millis(); }
    } else if (!st->connected()) {
      break;
    } else {
      delay(5);
    }
  }
  http.end();
  return (int)len;
}
static bool apiJson(NetworkClientSecure &client, const char *path, JsonDocument &doc, JsonDocument *filter = nullptr) {
  int len = apiGet(client, path, buf, CAP - 1);
  if (len <= 0) return false;
  DeserializationError je = filter ? deserializeJson(doc, (const char *)buf, (size_t)len, DeserializationOption::Filter(*filter))
                                   : deserializeJson(doc, (const char *)buf, (size_t)len);
  if (je) {
    snprintf(lastErr, sizeof lastErr, "%s: json %s", path, je.c_str());
    Serial.printf("unifi %s (%d bytes)\n", lastErr, len);
  }
  return !je;
}
static time_t parseIso(const char *s) {  // "2026-09-18T14:05:00Z"
  int y, mo, d, H, M, S = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &H, &M, &S) < 5) return 0;
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400), m = mo;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (time_t)(era * 146097 + (int32_t)doe - 719468) * 86400 + H * 3600 + M * 60 + S;
}
static bool isGatewayModel(const char *m) {
  static const char *const gw[] = {"UDM", "UDR", "UDW", "UCG", "UXG", "USG", "UNVR", "EFG"};
  for (const char *p : gw) if (strncmp(m, p, strlen(p)) == 0) return true;
  return false;
}
static bool fetchSite(NetworkClientSecure &client) {
  JsonDocument doc;
  if (!apiJson(client, "/sites", doc)) return false;
  JsonObject s = doc["data"][0];
  snprintf(siteId, sizeof siteId, "%s", s["id"] | "");
  snprintf(siteName, sizeof siteName, "%s", s["name"] | "");
  return siteId[0];
}
static void fetchDevices(NetworkClientSecure &client) {
  char path[80];
  snprintf(path, sizeof path, "/sites/%s/devices?limit=%u", siteId, MAX_DEV);
  JsonDocument doc, filter;
  JsonObject f = filter["data"][0].to<JsonObject>();
  for (const char *k : {"id", "name", "model", "ipAddress", "state", "features"}) f[k] = true;
  if (!apiJson(client, path, doc, &filter)) return;
  static Device got[MAX_DEV];
  uint8_t n = 0;
  for (JsonObject d : doc["data"].as<JsonArray>()) {
    if (n >= MAX_DEV) break;
    Device &g = got[n];
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "%s", d["id"] | "");
    snprintf(g.name, sizeof g.name, "%s", d["name"] | "");
    snprintf(g.model, sizeof g.model, "%s", d["model"] | "");
    snprintf(g.ip, sizeof g.ip, "%s", d["ipAddress"] | "");
    g.online = strcmp(d["state"] | "", "ONLINE") == 0;
    for (JsonVariant ft : d["features"].as<JsonArray>())
      if (strcmp(ft | "", "gateway") == 0) g.gateway = true;
    if (!g.gateway && isGatewayModel(g.model)) g.gateway = true;
    if (!g.id[0]) continue;
    for (uint8_t k = 0; k < nDev; k++)  // keep the stats the last round gathered
      if (strcmp(dev[k].id, g.id) == 0) { g.uptime = dev[k].uptime; g.cpu = dev[k].cpu; g.mem = dev[k].mem; g.rx = dev[k].rx; g.tx = dev[k].tx; }
    n++;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  memcpy(dev, got, sizeof(Device) * n);
  nDev = n;
  devAt = millis();
  ver++;
  xSemaphoreGive(mux);
  Serial.printf("unifi: %u devices (heap %u)\n", n, ESP.getFreeHeap());
}
static void fetchStats(NetworkClientSecure &client) {  // every device's latest, the gateway's uplink into the history
  float grx = 0, gtx = 0;
  bool gw = false;
  for (uint8_t i = 0; i < nDev; i++) {
    if (!dev[i].online) continue;
    char path[120];
    snprintf(path, sizeof path, "/sites/%s/devices/%s/statistics/latest", siteId, dev[i].id);
    JsonDocument doc;
    if (!apiJson(client, path, doc)) continue;
    xSemaphoreTake(mux, portMAX_DELAY);
    dev[i].uptime = doc["uptimeSec"] | 0UL;
    dev[i].cpu = doc["cpuUtilizationPct"] | 0.0f;
    dev[i].mem = doc["memoryUtilizationPct"] | 0.0f;
    dev[i].rx = doc["uplink"]["rxRateBps"] | 0.0f;
    dev[i].tx = doc["uplink"]["txRateBps"] | 0.0f;
    if (dev[i].gateway && !gw) { gw = true; grx = dev[i].rx; gtx = dev[i].tx; }
    xSemaphoreGive(mux);
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  if (gw) {
    if (nSamples == SAMPLES) {
      memmove(rxHist, rxHist + 1, (SAMPLES - 1) * sizeof(float));
      memmove(txHist, txHist + 1, (SAMPLES - 1) * sizeof(float));
      nSamples--;
    }
    rxHist[nSamples] = grx;
    txHist[nSamples] = gtx;
    nSamples++;
  }
  statsAt = millis();
  ver++;
  xSemaphoreGive(mux);
}
static void fetchClients(NetworkClientSecure &client) {
  char path[80];
  snprintf(path, sizeof path, "/sites/%s/clients?limit=%u", siteId, MAX_CLI);
  JsonDocument doc, filter;
  JsonObject f = filter["data"][0].to<JsonObject>();
  for (const char *k : {"name", "ipAddress", "type", "connectedAt", "uplinkDeviceId", "macAddress"}) f[k] = true;
  if (!apiJson(client, path, doc, &filter)) return;
  static NetClient *got = (NetClient *)heap_caps_calloc(MAX_CLI, sizeof(NetClient), MALLOC_CAP_SPIRAM);
  uint16_t n = 0, wl = 0;
  for (JsonObject c : doc["data"].as<JsonArray>()) {
    if (n >= MAX_CLI) break;
    NetClient &g = got[n];
    memset(&g, 0, sizeof g);
    const char *nm = c["name"] | "";
    snprintf(g.name, sizeof g.name, "%s", nm[0] ? nm : (c["macAddress"] | "?"));
    snprintf(g.ip, sizeof g.ip, "%s", c["ipAddress"] | "");
    snprintf(g.uplink, sizeof g.uplink, "%s", c["uplinkDeviceId"] | "");
    g.wireless = strcmp(c["type"] | "", "WIRELESS") == 0;
    g.since = parseIso(c["connectedAt"] | "");
    if (g.wireless) wl++;
    n++;
  }
  for (uint16_t i = 1; i < n; i++) {  // newest connection first
    NetClient k = got[i];
    int16_t j = i - 1;
    while (j >= 0 && got[j].since < k.since) { got[j + 1] = got[j]; j--; }
    got[j + 1] = k;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  memcpy(cli, got, sizeof(NetClient) * n);
  nCli = n;
  nWireless = wl;
  cliAt = millis();
  ver++;
  xSemaphoreGive(mux);
  Serial.printf("unifi: %u clients, %u wireless (heap %u)\n", n, wl, ESP.getFreeHeap());
}
static void fetchTask(void *) {
  NetworkClientSecure client;
  client.setInsecure();  // the console's certificate is its own
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (WiFi.status() != WL_CONNECTED || !buf || !apiKey[0]) continue;
    if (!siteId[0]) { if (!fetchSite(client)) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; } Serial.printf("unifi: site %s\n", siteName); }
    if (devAt == 0 || millis() - devAt > devicesMs) { fetchDevices(client); continue; }
    if (statsAt == 0 || millis() - statsAt > statsMs) { fetchStats(client); continue; }
    if (cliAt == 0 || millis() - cliAt > clientsMs) { fetchClients(client); continue; }
  }
}

// ── pages ────────────────────────────────────────────────────────────────
enum class View { Overview, Devices, Clients, Settings, Themes, Info, Choice, Confirm, Setup, Kb };
static uint8_t kbField_ = 0;  // 0 the console, 1 the key
static char editBuf[80];
static View view = View::Overview, confirmFrom = View::Settings;
static uint8_t sect = 0, sClock = 0, chN = 0, chSel = 0;
static uint16_t listTop = 0;
static const char *const TABS[] = {"OVERVIEW", "DEVICES", "CLIENTS"}, *const CLOCK_NAMES[] = {"12-hour", "24-hour"};
static int16_t tabX[3], tabW[3];
static char cHead[16], cRows[ROWS][96];
static uint32_t shownVer = 0;
static Preferences prefs;
static void saveSettings() {
  prefs.begin("unifi", false);
  prefs.putUChar("bg", sTheme);
  prefs.putUChar("clk", sClock);
  prefs.putString("host", host);
  prefs.putString("key", apiKey);
  prefs.end();
}
// The setup page: where the key comes from, the two fields, Connect.
// Shown at boot until there is a key; Settings > Console opens it again.
static void drawSetup() {
  pageHeader("SET UP", "the console's API key");
  const char *steps[] = {"1. In UniFi OS (not the Network app) open Settings > Control Plane > Integrations.",
                         "2. Create API Key, name it for this board, copy the key: it shows only once.",
                         "3. Tap the key row below and type it in, then Connect."};
  for (uint8_t i = 0; i < 3; i++) textAt(20, 56 + i * 24, 1, i == 2 ? C_FG : C_MUTED, steps[i]);
  char masked[24] = "not set";
  size_t l = strlen(apiKey);
  if (l) snprintf(masked, sizeof masked, "%s%.4s", l > 4 ? "**** " : "", apiKey + (l > 4 ? l - 4 : 0));  // the last four, the rest hidden
  settingRow(2, "Console", host);
  settingRow(3, "API key", masked);
  gfx->fillRoundRect(LCD_W / 2 - 120, 270, 240, 48, 24, apiKey[0] ? C_GOLD : C_RULE);
  textAt(LCD_W / 2 - textWidth(2, "Connect") / 2, 270 + (48 - FACES[1].cap) / 2, 2, apiKey[0] ? 0x0000 : C_DIM, "Connect");
  textAt(20, 340, 1, C_DIM, "Or put it in data/config.local.json and push it: the README says how.");
  if (lastErr[0]) textAt(20, 364, 1, C_BAD, lastErr);
  drawHint(apiKey[0] ? "< overview" : "no key yet");
}
static bool hitConnect(int16_t x, int16_t y) { return y >= 270 && y < 318 && x >= LCD_W / 2 - 120 && x < LCD_W / 2 + 120; }
static void fmtRate(float bps, char *out, size_t n) {  // 12.3 Mbps
  if (bps >= 1e9f) snprintf(out, n, "%.2f Gbps", bps / 1e9f);
  else if (bps >= 1e6f) snprintf(out, n, "%.1f Mbps", bps / 1e6f);
  else if (bps >= 1e3f) snprintf(out, n, "%.0f kbps", bps / 1e3f);
  else snprintf(out, n, "%.0f bps", bps);
}
static void fmtUptime(uint32_t s, char *out, size_t n) {
  if (s >= 86400) snprintf(out, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24));
  else if (s >= 3600) snprintf(out, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
  else snprintf(out, n, "%lum", (unsigned long)(s / 60));
}
static void fmtAgo(time_t since, char *out, size_t n) {
  time_t now = time(nullptr);
  if (!since || now < 1000000000L || now < since) { out[0] = '\0'; return; }
  fmtUptime((uint32_t)(now - since), out, n);
}
static void drawHeader() {
  gfx->fillRect(0, 0, 640, Y_ROW0 - 1, C_BG);
  int16_t x = 12;
  for (uint8_t i = 0; i < 3; i++) {
    int16_t w = textWidth(1, TABS[i]) + 26;
    tabX[i] = x;
    tabW[i] = w;
    bool on = i == sect;
    textAt(x + 13, 12, 1, on ? C_FG : C_DIM, TABS[i]);
    if (on) gfx->fillRect(x + 9, 34, w - 18, 3, C_FG);
    x += w;
  }
  if (siteName[0]) textAt(x + 20, 14, 1, C_DIM, siteName);
  settingsIcon(676, C_MUTED);
  gfx->drawFastHLine(0, Y_ROW0 - 1, LCD_W, C_RULE);
}
static int8_t hitTab(int16_t x) {
  for (uint8_t i = 0; i < 3; i++)
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
static void tile(uint8_t i, const char *label, const char *big, const char *small, uint16_t bigColour) {
  int16_t x = 20 + i * 256, y = 52, w = 240, h = 132;
  gfx->fillRoundRect(x, y, w, h, 14, towardsWhite(C_BG, 6));
  gfx->drawRoundRect(x, y, w, h, 14, C_RULE);
  textAt(x + 18, y + 14, 1, C_MUTED, label);
  textAt(x + 18, y + 40, 3, bigColour, big);
  textAt(x + 18, y + 96, 1, C_DIM, small);
}
static void drawChart() {  // the gateway's throughput: rx in green, tx in the accent, the last half hour
  int16_t x = 20, y = 208, w = LCD_W - 40, h = Y_HINT - 12 - y;
  gfx->fillRect(x - 1, y - 1, w + 2, h + 2, C_BG);
  gfx->drawRect(x - 1, y - 1, w + 2, h + 2, C_RULE);
  if (nSamples < 2) {
    textAt(x + 10, y + h / 2 - 8, 1, C_DIM, apiKey[0] ? "the gateway's throughput, once the stats land" : "no API key: see the README");
    return;
  }
  float hi = 1;
  for (uint16_t i = 0; i < nSamples; i++) hi = max(hi, max(rxHist[i], txHist[i]));
  for (uint8_t which = 0; which < 2; which++) {
    const float *v = which ? txHist : rxHist;
    uint16_t c = which ? C_GOLD : C_GOOD;
    int16_t px = x, py = y + h - 1 - (int16_t)(v[0] / hi * (h - 12));
    for (uint16_t i = 1; i < nSamples; i++) {
      int16_t nx = x + (int32_t)i * (w - 1) / (SAMPLES - 1);
      int16_t ny = y + h - 1 - (int16_t)(v[i] / hi * (h - 12));
      gfx->drawLine(px, py, nx, ny, c);
      px = nx;
      py = ny;
    }
  }
  char b[24], l[48];
  fmtRate(hi, b, sizeof b);
  snprintf(l, sizeof l, "peak %s", b);
  textAt(x + 8, y + 6, 1, C_DIM, l);
  fmtRate(rxHist[nSamples - 1], b, sizeof b);
  snprintf(l, sizeof l, "down %s", b);
  textAt(x + w - 8 - textWidth(1, l), y + 6, 1, C_GOOD, l);
  fmtRate(txHist[nSamples - 1], b, sizeof b);
  snprintf(l, sizeof l, "up %s", b);
  textAt(x + w - 8 - textWidth(1, l), y + 26, 1, C_GOLD, l);
  textAt(x + 8, y + h - 20, 1, C_DIM, "30 min");
}
static void drawOverview(bool full) {
  static char kOv[96];
  if (full) {
    gfx->fillScreen(C_BG);
    drawHeader();
    cHead[0] = '\0';
    kOv[0] = '\0';
    drawHint(apiKey[0] ? "tap a tab        hold the header to shut down" : "put the console's API key in data/config.local.json");
  }
  drawClock();
  uint8_t online = 0;
  const Device *gw = nullptr;
  for (uint8_t i = 0; i < nDev; i++) { if (dev[i].online) online++; if (dev[i].gateway && !gw) gw = &dev[i]; }
  char key[96];
  snprintf(key, sizeof key, "%u|%u|%u|%u|%.0f|%.0f|%lu|%lu|%s", nCli, nWireless, online, nDev, gw ? gw->rx : 0.0f, gw ? gw->tx : 0.0f,
           (unsigned long)(gw ? gw->uptime / 60 : 0), (unsigned long)nSamples, lastErr);
  if (strcmp(key, kOv) == 0) return;
  strcpy(kOv, key);
  char big[24], small[48];
  snprintf(big, sizeof big, "%u", nCli);
  snprintf(small, sizeof small, "%u wired   %u wireless", nCli - nWireless, nWireless);
  tile(0, "CLIENTS", cliAt ? big : "--", cliAt ? small : "waiting for the console", C_FG);
  snprintf(big, sizeof big, "%u / %u", online, nDev);
  snprintf(small, sizeof small, "online / all");
  tile(1, "DEVICES", devAt ? big : "--", devAt ? small : lastErr[0] ? lastErr : "waiting for the console", online == nDev ? C_GOOD : C_WARN);
  if (gw) {
    char r[16], t[16], up[16];
    fmtRate(gw->rx, r, sizeof r);
    fmtRate(gw->tx, t, sizeof t);
    fmtUptime(gw->uptime, up, sizeof up);
    snprintf(big, sizeof big, "%s", r);
    snprintf(small, sizeof small, "up %s   %s   cpu %.0f%%", t, up, gw->cpu);
    tile(2, "WAN, DOWN", big, small, C_GOOD);
  } else {
    tile(2, "GATEWAY", "--", "no gateway among the devices", C_DIM);
  }
  drawChart();
}
static void drawDevRow(uint8_t slot) {
  int16_t y = Y_ROW0 + slot * ROW_H;
  uint16_t i = listTop + slot;
  char key[96] = "";
  if (i < nDev) {
    const Device &d = dev[i];
    snprintf(key, sizeof key, "%s|%d|%lu|%.0f|%.0f|%s", d.name, d.online, (unsigned long)(d.uptime / 60), d.cpu, d.rx, d.ip);
  }
  if (strcmp(key, cRows[slot]) == 0) return;
  strcpy(cRows[slot], key);
  gfx->fillRect(0, y, LCD_W, ROW_H, C_BG);
  if (i >= nDev) return;
  const Device &d = dev[i];
  gfx->fillCircle(28, y + 22, 6, d.online ? C_GOOD : C_BAD);
  textAt(50, y + 12, 2, d.online ? C_FG : C_MUTED, d.name[0] ? d.name : d.model);
  textAt(300, y + 15, 1, C_MUTED, d.model);
  textAt(400, y + 15, 1, C_DIM, d.ip);
  char b[24];
  if (d.online) {
    fmtUptime(d.uptime, b, sizeof b);
    textAt(540, y + 15, 1, C_MUTED, b);
    snprintf(b, sizeof b, "cpu %.0f%%  mem %.0f%%", d.cpu, d.mem);
    textAt(X_RIGHT - textWidth(1, b), y + 15, 1, C_DIM, b);
  } else {
    textAt(X_RIGHT - textWidth(1, "offline"), y + 15, 1, C_BAD, "offline");
  }
  gfx->drawFastHLine(20, y + ROW_H - 1, LCD_W - 40, C_RULE);
}
static const char *devName(const char *id) {
  for (uint8_t i = 0; i < nDev; i++) if (strcmp(dev[i].id, id) == 0) return dev[i].name;
  return "";
}
static void drawCliRow(uint8_t slot) {
  int16_t y = Y_ROW0 + slot * ROW_H;
  uint16_t i = listTop + slot;
  char key[96] = "";
  if (i < nCli) {
    const NetClient &c = cli[i];
    snprintf(key, sizeof key, "%s|%s|%d|%ld", c.name, c.ip, c.wireless, (long)(c.since / 60));
  }
  if (strcmp(key, cRows[slot]) == 0) return;
  strcpy(cRows[slot], key);
  gfx->fillRect(0, y, LCD_W, ROW_H, C_BG);
  if (i >= nCli) return;
  const NetClient &c = cli[i];
  if (c.wireless) {  // three arcs
    for (int8_t r = 6; r <= 14; r += 4) for (float a = -0.8f; a <= 0.8f; a += 0.04f) gfx->drawPixel(28 + (int16_t)lroundf(r * sinf(a)), y + 30 - (int16_t)lroundf(r * cosf(a)), C_MUTED);
    gfx->fillCircle(28, y + 30, 2, C_MUTED);
  } else {
    gfx->drawRect(20, y + 16, 16, 12, C_MUTED);
    gfx->drawFastVLine(28, y + 28, 6, C_MUTED);
  }
  textAt(50, y + 12, 2, C_FG, c.name);
  textAt(340, y + 15, 1, C_DIM, c.ip);
  textAt(480, y + 15, 1, C_MUTED, devName(c.uplink));
  char b[16];
  fmtAgo(c.since, b, sizeof b);
  textAt(X_RIGHT - textWidth(1, b), y + 15, 1, C_DIM, b);
  gfx->drawFastHLine(20, y + ROW_H - 1, LCD_W - 40, C_RULE);
}
static void drawList(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    drawHeader();
    cHead[0] = '\0';
    for (uint8_t s = 0; s < ROWS; s++) cRows[s][0] = '\0';
    drawHint(sect == 1 ? "drag for more        online, model, address, uptime, load" : "drag for more        newest connection first");
  }
  drawClock();
  uint16_t n = sect == 1 ? nDev : nCli;
  if (listTop + ROWS > n) listTop = n > ROWS ? n - ROWS : 0;
  for (uint8_t s = 0; s < ROWS; s++) sect == 1 ? drawDevRow(s) : drawCliRow(s);
}
static void drawSettings() {
  pageHeader("SETTINGS");
  settingRow(0, "Clock", CLOCK_NAMES[sClock]);
  settingRow(1, "Theme", THEMES[sTheme].name);
  settingRow(2, "Info", "");
  settingRow(3, "Console", host);
  settingRow(4, "Shut down", "", C_BAD);
  drawHint("< overview");
}
static void drawInfo() {
  pageHeader("INFO");
  char l[7][60];
  uint8_t n = 0;
  snprintf(l[n++], 60, "console %s, site %s", host, siteName[0] ? siteName : "-");
  snprintf(l[n++], 60, "%u devices, %u clients", nDev, nCli);
  snprintf(l[n++], 60, "stats every %lus, devices %lus, clients %lus", (unsigned long)(statsMs / 1000), (unsigned long)(devicesMs / 1000), (unsigned long)(clientsMs / 1000));
  snprintf(l[n++], 60, "last error: %s", lastErr[0] ? lastErr : "none");
  snprintf(l[n++], 60, "wifi %.16s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  snprintf(l[n++], 60, "heap %uk free, psram %uk free", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  snprintf(l[n++], 60, "built " __DATE__ " " __TIME__);
  for (uint8_t i = 0; i < n; i++) textAt(20, 60 + i * 26, 1, i == 0 ? C_FG : C_MUTED, l[i]);
  drawHint("< settings");
}
static void drawCurrent(bool full) {
  if (view == View::Overview) drawOverview(full);
  else drawList(full);
}
static void openConfirm() {
  confirmFrom = view;
  gfx->fillScreen(C_BG);
  drawHeader();
  view = View::Confirm;
  drawShutdownSheet();
}
static void closeConfirm() {
  sheetClose();
  view = confirmFrom;
  drawCurrent(true);
}
static void shutDown() {
  drawHint("shutting down. BOOT button turns it on", C_WARN);
  delay(600);
  backlight(0);
  WiFi.disconnect(true);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
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
  buf = (uint8_t *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);  // 96KB: internal RAM could not spare it (7.9KB of heap left, Wi-Fi would not join)
  cli = (NetClient *)heap_caps_calloc(MAX_CLI, sizeof(NetClient), MALLOC_CAP_SPIRAM);
  for (const char *path : {"/config.json", "/config.local.json"}) {  // the committed one, then yours (gitignored) on top
    if (!cfgLoad(path)) continue;
    cfgStr("unifi.host", host, sizeof host);
    cfgStr("unifi.apiKey", apiKey, sizeof apiKey);
    statsMs = (uint32_t)cfgInt("refresh.statsSeconds", statsMs / 1000, 5, 600) * 1000UL;
    devicesMs = (uint32_t)cfgInt("refresh.devicesSeconds", devicesMs / 1000, 10, 3600) * 1000UL;
    clientsMs = (uint32_t)cfgInt("refresh.clientsSeconds", clientsMs / 1000, 10, 3600) * 1000UL;
    cfgStr("tz", tzString, sizeof tzString);
    cfgRelease();
  }
  Serial.printf("network: console %s, key %s, stats %lus\n", host, apiKey[0] ? "set" : "MISSING", (unsigned long)(statsMs / 1000));
  prefs.begin("ticker", true);
  prefs.getString("ssid", wifiSsid, sizeof wifiSsid);
  prefs.getString("pass", wifiPass, sizeof wifiPass);
  prefs.end();
  prefs.begin("unifi", true);
  sTheme = prefs.getUChar("bg", 0) % N_THEMES;
  sClock = prefs.getUChar("clk", 0) % 2;
  if (prefs.isKey("key")) {  // typed on the board: beats the files
    prefs.getString("host", host, sizeof host);
    prefs.getString("key", apiKey, sizeof apiKey);
  }
  prefs.end();
  applyTheme();
  setenv("TZ", tzString, 1);
  tzset();
  gfx = boardDisplay();
  bool ok = gfx->begin();
  Serial.printf("board: expander %s, panel %s, psram %u free\n", xp ? "ok" : "NO ACK", ok ? "ok" : "FAILED", ESP.getFreePsram());
  boardSetRotation(0);
  gfx->setTextWrap(false);
  if (apiKey[0]) drawOverview(true);
  else { view = View::Setup; drawSetup(); }
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
  static uint32_t headerHoldAt = 0;
  if (g == Gesture::LongPress && ty < Y_ROW0 && view != View::Confirm) headerHoldAt = millis();
  if (!touchHeld) headerHoldAt = 0;
  if (headerHoldAt && millis() - headerHoldAt > 1300) { headerHoldAt = 0; openConfirm(); g = Gesture::None; }
  if (view == View::Overview || view == View::Devices || view == View::Clients) {
    uint16_t n = sect == 1 ? nDev : nCli;
    if (view != View::Overview && g == Gesture::Drag && ddy) {
      static int16_t acc = 0;
      acc += ddy;
      while (acc <= -ROW_H / 2 && listTop + ROWS < n) { acc += ROW_H / 2; listTop++; drawList(false); }
      while (acc >= ROW_H / 2 && listTop > 0) { acc -= ROW_H / 2; listTop--; drawList(false); }
      if (listTop == 0 && acc > 0) acc = 0;
      if (listTop + ROWS >= n && acc < 0) acc = 0;
    } else if (g == Gesture::TapUp && ty < Y_ROW0) {
      if (tx >= 660 && tx < 724) { view = View::Settings; drawSettings(); }
      else {
        int8_t t = hitTab(tx);
        if (t >= 0 && t != sect) { sect = (uint8_t)t; listTop = 0; view = t == 0 ? View::Overview : t == 1 ? View::Devices : View::Clients; drawCurrent(true); }
      }
    } else if (g == Gesture::SwipeLeft || g == Gesture::SwipeRight) {
      sect = (sect + (g == Gesture::SwipeLeft ? 1 : 2)) % 3;
      listTop = 0;
      view = sect == 0 ? View::Overview : sect == 1 ? View::Devices : View::Clients;
      drawCurrent(true);
    }
    if (view == View::Overview || view == View::Devices || view == View::Clients) {
      if (shownVer != ver) { shownVer = ver; drawCurrent(false); }
      else drawClock();
    }
  } else if (view == View::Setup) {
    if (g == Gesture::TapUp) {
      int8_t i = hitSettingRow(ty, 4);
      if (ty < Y_ROW0 && tx < 140 && apiKey[0]) { view = View::Overview; sect = 0; drawCurrent(true); }
      else if (i == 2 || i == 3) {
        kbField_ = i == 2 ? 0 : 1;
        snprintf(editBuf, sizeof editBuf, "%s", kbField_ ? apiKey : host);
        kbOpen(kbField_ ? "API KEY" : "CONSOLE", editBuf, kbField_ ? sizeof apiKey : sizeof host);
        view = View::Kb;
        kbDraw(kbField_ ? "the key, exactly as shown; it is case sensitive" : "address or name, 10.0.10.1 or unifi");
      } else if (hitConnect(tx, ty) && apiKey[0]) {
        saveSettings();
        siteId[0] = '\0';
        lastErr[0] = '\0';
        devAt = cliAt = statsAt = 0;
        view = View::Overview;
        sect = 0;
        drawCurrent(true);
      }
    }
  } else if (view == View::Kb) {
    if (g == Gesture::TapUp || g == Gesture::Tap) {
      uint8_t r = kbTap(tx, ty);
      if (r == 2) {
        if (kbField_) snprintf(apiKey, sizeof apiKey, "%s", editBuf);
        else snprintf(host, sizeof host, "%s", editBuf);
        view = View::Setup;
        drawSetup();
      } else if (r == 3) {
        view = View::Setup;
        drawSetup();
      }
    }
  } else if (view == View::Settings) {
    if (g == Gesture::SwipeLeft || g == Gesture::SwipeDown || (g == Gesture::TapUp && ty < Y_ROW0 && tx < 140)) { view = sect == 0 ? View::Overview : sect == 1 ? View::Devices : View::Clients; drawCurrent(true); }
    else if (g == Gesture::TapUp) {
      int8_t i = hitSettingRow(ty, 5);
      if (i == 0) { chN = 2; chSel = sClock; view = View::Choice; drawChoiceSheet("CLOCK", CLOCK_NAMES, chN, chSel); }
      else if (i == 1) { view = View::Themes; drawThemesPage(); }
      else if (i == 2) { view = View::Info; drawInfo(); }
      else if (i == 3) { view = View::Setup; drawSetup(); }
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
    Serial.printf("heap %u  psram %u  wifi %s  devices %u clients %u samples %u\n", ESP.getFreeHeap(), ESP.getFreePsram(), WiFi.status() == WL_CONNECTED ? "up" : "DOWN", nDev, nCli, nSamples);
  }
  delay(20);
}
