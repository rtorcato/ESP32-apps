// desk-clock: NTP clock + open-meteo weather.
//
// One button, three actions by hold duration (persisted in NVS):
//   tap          -- next rotation: 0 -> 90 -> 180 -> 270
//   hold 1.2s    -- toggle light/dark
//   hold 3s      -- blank the panel; any press brings it back
// While holding, an on-screen hint says what releasing will do.
//
// Four rotations, not two, because the USB-C socket is on a fixed edge: mounting
// the board with the cable exiting left, right, top or bottom needs all four.
// Rotation is runtime rather than a build flag, and the panel's (34, 0, 34, 0)
// offsets are correct in all four.
//
// All coordinates come from a Layout struct chosen at runtime, so every redraw
// still erases an exact box -- there is no fillScreen() in loop(), which is
// what stops the large digits from flickering.
#include <board.h>
#include <ui.h>
#include <secrets.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <assert.h>
#include <time.h>

// ── config ───────────────────────────────────────────────────────────────
// A real TZ string, not a UTC offset -- this is what makes DST automatic.
static const char *TZ_STRING = "EST5EDT,M3.2.0/2,M11.1.0/2";
static const float LAT = 43.6532f, LON = -79.3832f;
static const char *PLACE = "TORONTO";

static const uint32_t WX_PERIOD_MS = 15UL * 60 * 1000;  // open-meteo is free; don't hammer it
static const uint32_t WX_RETRY_MS = 60UL * 1000;        // but retry sooner while it's failing
// Night window and per-theme backlight levels live in lib/board/ui.h
// (uiNightFrom/uiNightTo, Theme::blDay/blNight) -- shared by every app.

// Power/heat. The board runs warm mainly because of the radio, the 160MHz clock
// and the LCD backlight, plus the 5V->3.3V LDO dissipating (5-3.3)*I. Set to 0
// to compare against the unthrottled baseline.
#define POWER_SAVE 1
static const uint32_t TEMP_LOG_MS = 20UL * 1000;  // report die temperature

// The built-in 6x8 font scales by integer size, so a glyph is exactly
// 6*size wide and 8*size tall. That exactness is why the dirty rects can be
// precomputed per layout.
#define GW(size) (6 * (size))
#define GH(size) (8 * (size))

static Arduino_GFX *gfx;

// ── layout ───────────────────────────────────────────────────────────────
// The only orientation-dependent data in the app. Everything below reads these
// fields, so nothing else has to know which way up the panel is.
//
// fcPitch is the horizontal step between forecast columns; fcChars is how many
// characters fit in one, which caps the labels wmoLabel() may return.
struct Layout {
  bool landscape;
  int16_t w, h;
  int16_t xTime, yTime;
  int16_t xSecs, ySecs;
  int16_t xDate, yDate;
  int16_t xStatus, yStatus, yStale;
  int16_t xCol2, yPlace, yTemp, yMeta;
  int16_t xFc0, yFcDay, yFcTmp, yFcCnd, fcPitch;
  uint8_t fcChars;
  int16_t rule1, rule2, rule3;  // portrait: three h-rules. landscape: unused.
};

// 172x320: three stacked blocks (time / current / forecast) plus a date footer.
static const Layout PORTRAIT = {
    /*landscape*/ false, /*w,h*/ 172, 320,
    /*time*/ (172 - GW(5) * 5) / 2, 24,
    /*secs*/ (172 - GW(2) * 2) / 2, 70,
    /*date*/ 8, 228,
    /*status*/ 8, 296, 308,
    /*col2*/ 8, 106, 120, 120,
    /*fc*/ 8, 172, 186, 200, 52,
    /*fcChars*/ 8,
    /*rules*/ 96, 162, 216,
};

// 320x172: clock and date on the left, weather on the right, split at x=170.
static const Layout LANDSCAPE = {
    /*landscape*/ true, /*w,h*/ 320, 172,
    /*time*/ 12, 18,
    /*secs*/ 12, 64,
    /*date*/ 12, 92,
    /*status*/ 12, 132, 148,
    /*col2*/ 182, 14, 28, 28,
    /*fc*/ 182, 96, 112, 128, 46,
    /*fcChars*/ 7,
    /*rules*/ 0, 0, 0,
};

// Which layout is live. Rotations 0/2 are portrait, 1/3 landscape.
static const Layout *L = &PORTRAIT;
static void syncLayout() { L = uiLandscape() ? &LANDSCAPE : &PORTRAIT; }

// ── weather state ────────────────────────────────────────────────────────
struct Weather {
  float temp, feels;
  int humidity, code;
  int hi[3], lo[3], dayCode[3];
  char day[3][4];
  bool valid;
  uint32_t fetchedAt;
};
static Weather wx = {};

// WMO weather code -> a label that fits the panel. This is the only
// non-trivial pure logic in the app, so it's what selfCheck() covers.
static const char *wmoLabel(int code) {
  switch (code) {
    case 0:  return "clear";
    case 1:  case 2:  return "partly";
    case 3:  return "cloudy";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: case 66: case 67: return "sleet";
    case 61: case 63: case 65: return "rain";
    case 71: case 73: case 75: case 77: case 85: case 86: return "snow";
    case 80: case 81: case 82: return "showers";
    case 95: case 96: case 99: return "storm";
    default: return "?";
  }
}

static uint16_t wmoColor(int code) {
  switch (code) {
    case 0: case 1: return uiTheme()->sun;
    case 71: case 73: case 75: case 77: case 85: case 86: return uiTheme()->snow;
    case 61: case 63: case 65: case 80: case 81: case 82: return uiTheme()->rain;
    case 95: case 96: case 99: return uiTheme()->storm;
    default: return uiTheme()->neutral;
  }
}

// ── drawing helpers ──────────────────────────────────────────────────────
// Erase an exact box, then draw into it. `chars` sizes the box, so a shorter
// string never leaves stale pixels from a longer one behind.
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

static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size,
                       uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, w, GH(size), uiTheme()->bg);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(right - (int16_t)(GW(size) * strlen(s)), y);
  gfx->print(s);
}

// The 6x8 font has no reliable degree glyph, so draw it as a ring.
static void degree(int16_t x, int16_t y, uint16_t fg) { gfx->drawCircle(x, y, 2, fg); }

// Redraw caches. Held at file scope so a mode change can invalidate them in one
// place -- otherwise a toggle repaints the background and leaves the old text
// colour, or the old orientation's coordinates, behind.
static char cHM[6], cSS[3], cDate[18], cStatus[34], cStale[34], cStatusKey[48];

static void invalidateCache() {
  cHM[0] = cSS[0] = cDate[0] = cStatus[0] = cStale[0] = cStatusKey[0] = '\0';
}

static void drawChrome() {
  gfx->fillScreen(uiTheme()->bg);
  if (L->landscape) {
    gfx->drawFastVLine(170, 12, L->h - 24, uiTheme()->rule);
    gfx->drawFastHLine(L->xCol2, L->yFcDay - 10, L->w - L->xCol2 - 8, uiTheme()->rule);
  } else {
    gfx->drawFastHLine(12, L->rule1, L->w - 24, uiTheme()->rule);
    gfx->drawFastHLine(12, L->rule2, L->w - 24, uiTheme()->rule);
    gfx->drawFastHLine(12, L->rule3, L->w - 24, uiTheme()->rule);
  }
  field(L->xCol2, L->yPlace, 12, 1, uiTheme()->muted, PLACE);
}

// Only repaints what changed -- the minute block once a minute, seconds once a
// second. Repainting the whole screen here is what makes big digits flicker.
static void drawClock(const struct tm &t) {
  char hm[6], ss[3];
  strftime(hm, sizeof hm, "%H:%M", &t);
  strftime(ss, sizeof ss, "%S", &t);
  if (strcmp(hm, cHM) != 0) {
    field(L->xTime, L->yTime, 5, 5, uiTheme()->fg, hm);
    strcpy(cHM, hm);
  }
  if (strcmp(ss, cSS) != 0) {
    field(L->xSecs, L->ySecs, 2, 2, uiTheme()->dim, ss);
    strcpy(cSS, ss);
  }
}

static void drawDate(const struct tm &t) {
  char d[18];
  strftime(d, sizeof d, "%a %d %b", &t);
  if (strcmp(d, cDate) == 0) return;
  // 13 not 14: at size 2 a 14-char box is 168px, which overruns the 172px
  // portrait panel and clips the erase rect.
  field(L->xDate, L->yDate, 13, 2, uiTheme()->fg, d, true);
  strcpy(cDate, d);
}

static void drawWeather() {
  char buf[16];

  if (!wx.valid) {
    field(L->xCol2, L->yTemp, 6, 4, uiTheme()->dim, "--");
    fieldRight(L->w - 8, L->yMeta, 12, 1, uiTheme()->warn, "no weather");
    fieldRight(L->w - 8, L->yMeta + 14, 12, 1, uiTheme()->dim, "");
    fieldRight(L->w - 8, L->yMeta + 28, 12, 1, uiTheme()->dim, "");
    for (int i = 0; i < 3; i++) {
      int16_t x = L->xFc0 + i * L->fcPitch;
      field(x, L->yFcDay, L->fcChars, 1, uiTheme()->dim, "--", true);
      field(x, L->yFcTmp, L->fcChars, 1, uiTheme()->dim, "", true);
      field(x, L->yFcCnd, L->fcChars, 1, uiTheme()->dim, "", true);
    }
    return;
  }

  snprintf(buf, sizeof buf, "%d", (int)lroundf(wx.temp));
  field(L->xCol2, L->yTemp, 4, 4, wmoColor(wx.code), buf);
  degree(L->xCol2 + GW(4) * strlen(buf) + 5, L->yTemp + 5, wmoColor(wx.code));

  snprintf(buf, sizeof buf, "feels %d", (int)lroundf(wx.feels));
  fieldRight(L->w - 8, L->yMeta, 12, 1, uiTheme()->muted, buf);
  fieldRight(L->w - 8, L->yMeta + 14, 12, 1, wmoColor(wx.code), wmoLabel(wx.code));
  snprintf(buf, sizeof buf, "%d%% hum", wx.humidity);
  fieldRight(L->w - 8, L->yMeta + 28, 12, 1, uiTheme()->muted, buf);

  for (int i = 0; i < 3; i++) {
    int16_t x = L->xFc0 + i * L->fcPitch;
    field(x, L->yFcDay, L->fcChars, 1, uiTheme()->muted, wx.day[i], true);
    snprintf(buf, sizeof buf, "%d/%d", wx.hi[i], wx.lo[i]);
    field(x, L->yFcTmp, L->fcChars, 1, uiTheme()->fg, buf, true);
    field(x, L->yFcCnd, L->fcChars, 1, wmoColor(wx.dayCode[i]), wmoLabel(wx.dayCode[i]), true);
  }
}

// ── offline screen ───────────────────────────────────────────────────────
// A clock with no time is a blank panel, which reads as "broken" rather than
// "not connected". When there is no time at all this takes over the screen and
// says what is wrong and what to do about it. Once time exists we go back to
// the normal layout and only the weather block degrades.
static uint8_t lastDisconnectReason = 0;

// Reason codes are the whole diagnosis, and a bare number sends you to a
// header file. Only the ones that mean something different are named.
static const char *reasonName(uint8_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "AUTH_EXPIRE";
    case WIFI_REASON_ASSOC_LEAVE:            return "ASSOC_LEAVE (we left)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_TIMEOUT";
    case WIFI_REASON_MISSING_ACKS:           return "MISSING_ACKS (weak link)";
    case WIFI_REASON_STA_LEAVING:            return "STA_LEAVING (we left)";
    case WIFI_REASON_TIMEOUT:                return "TIMEOUT";
    case WIFI_REASON_AUTH_FAIL:              return "AUTH_FAIL";
    case WIFI_REASON_NO_AP_FOUND:            return "NO_AP_FOUND";
    case WIFI_REASON_ASSOC_FAIL:             return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:        return "CONNECTION_FAIL";
    default:                                 return "?";
  }
}

// Only these point at the credential. MISSING_ACKS, TIMEOUT and the LEAVE codes
// are RF or teardown -- blaming the password for those sent me looking in the
// wrong place twice. Even these are not conclusive: a correct PSK also times
// out when the AP has PMF required or 802.11r on, or simply needs a retry.
static bool reasonIsAuth(uint8_t r) {
  return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
}

static void panelLine(uint8_t row, uint16_t fg, const char *s) {
  field(8, 62 + row * 11, (L->w / GW(1)) - 2, 1, fg, s);
}

static void drawOfflinePanel(const char *title, uint16_t titleColor, const char *const *lines,
                             uint8_t n) {
  gfx->fillScreen(uiTheme()->bg);
  field(8, 24, (L->w / GW(1)) - 2, 3, titleColor, title);
  gfx->drawFastHLine(8, 54, L->w - 16, uiTheme()->rule);
  // Landscape is only 172px tall, so fewer rows fit than portrait. Clip rather
  // than drawing off the bottom edge.
  uint8_t maxRows = (L->h - 62 - 16) / 11;
  for (uint8_t i = 0; i < n && i < maxRows; i++) panelLine(i, uiTheme()->muted, lines[i]);
}

static void drawNoWifi() {
  uint8_t r = lastDisconnectReason;
  char ssid[40], why[40];
  snprintf(ssid, sizeof ssid, "SSID %s", WIFI_SSID);
  snprintf(why, sizeof why, "%u %s", r, reasonName(r));

  // The advice has to match the reason, or the screen sends you to the wrong
  // place -- which is exactly the mistake I made reading these by hand.
  const char *a1, *a2, *a3;
  if (r == WIFI_REASON_NO_AP_FOUND) {
    a1 = "Not on the air. Check";
    a2 = "the name -- this board";
    a3 = "is 2.4GHz only.";
  } else if (reasonIsAuth(r)) {
    a1 = "Handshake refused.";
    a2 = "Often just needs a";
    a3 = "retry. Else check PSK.";
  } else {
    a1 = "Link too weak or too";
    a2 = "noisy. Move closer,";
    a3 = "don't cup the antenna.";
  }

  const char *lines[] = {ssid, why, "", a1, a2, a3, "", "retrying..."};
  drawOfflinePanel("NO WIFI", uiTheme()->bad, lines, sizeof lines / sizeof lines[0]);
}

static void drawNoTime() {
  char ssid[40], ip[40], rssi[40];
  snprintf(ssid, sizeof ssid, "on %s", WiFi.SSID().c_str());
  snprintf(ip, sizeof ip, "ip %s", WiFi.localIP().toString().c_str());
  snprintf(rssi, sizeof rssi, "signal %d dBm", WiFi.RSSI());
  const char *lines[] = {ssid,
                         ip,
                         rssi,
                         "",
                         "Wi-Fi is up but NTP",
                         "is blocked. A guest",
                         "network's captive",
                         "portal does exactly",
                         "this. Use an IoT SSID",
                         "with no portal.",
                         "",
                         "retrying..."};
  drawOfflinePanel("NO CLOCK", uiTheme()->warn, lines, sizeof lines / sizeof lines[0]);
}

// ── network ──────────────────────────────────────────────────────────────
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    // Don't let our own teardown overwrite the meaningful reason: diagnoseWiFi()
    // calls WiFi.disconnect() before scanning, and the resulting ASSOC_LEAVE
    // masked the real HANDSHAKE_TIMEOUT that had just occurred.
    if (r != 0 && r != WIFI_REASON_ASSOC_LEAVE && r != WIFI_REASON_STA_LEAVING) {
      lastDisconnectReason = r;
    }
    const char *hint = "";
    if (r == WIFI_REASON_NO_AP_FOUND) {
      hint = " (not visible -- wrong SSID, or 5GHz-only: the C6 is 2.4GHz only)";
    } else if (reasonIsAuth(r)) {
      // Not only the password. With a correct PSK and a strong signal this also
      // happens on the first attempt and succeeds on the next, which is exactly
      // what a too-short connect window looks like.
      hint = " (retry, PSK, or AP PMF-required / 802.11r)";
    }
    // Auto-reconnect retries forever, so throttle by time. Deduping on "reason
    // changed" is not enough: a failing retry alternates between two codes
    // (201 then 36), so every line looks like a change.
    static uint32_t lastLog = 0;
    if (lastLog != 0 && millis() - lastLog < 10000) return;
    lastLog = millis();
    Serial.printf("wifi disconnect reason %u %s%s\n", r, reasonName(r), hint);
  }
}

// HANDSHAKE_TIMEOUT is not only "wrong password" -- it is also what you get
// when the auth mode can't be negotiated (WPA3-only, enterprise, or a WPA2/WPA3
// transition AP with PMF required). So the scan reports the actual mode.
static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:                    return "open";
    case WIFI_AUTH_WEP:                     return "WEP";
    case WIFI_AUTH_WPA_PSK:                 return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:                return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:            return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:                return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:           return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WPA3_EXT_PSK:            return "WPA3-ext-PSK";
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "WPA3-ext-mixed";
    case WIFI_AUTH_OWE:                     return "OWE";
    case WIFI_AUTH_WAPI_PSK:                return "WAPI-PSK";
    case WIFI_AUTH_ENTERPRISE:              return "ENTERPRISE (802.1X!)";
    case WIFI_AUTH_WPA3_ENTERPRISE:         return "WPA3-ENTERPRISE (802.1X!)";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:    return "WPA2/WPA3-ENT (802.1X!)";
    case WIFI_AUTH_WPA3_ENT_192:            return "WPA3-ENT-192 (802.1X!)";
    default:                                return "?";
  }
}

// Run only when the connect attempt has already failed. Says whether the
// configured SSID is even on the air, which is the fork in the diagnosis.
static void diagnoseWiFi() {
  // Stop the retry loop first: a scan started while a connect is in flight
  // returns -2 (scan failed) instead of a list, which is a useless diagnostic.
  WiFi.disconnect(true);
  delay(300);

  Serial.println("scanning 2.4GHz (a 5GHz-only SSID cannot appear here):");
  int n = WiFi.scanNetworks();
  if (n < 0) {
    Serial.printf("scan failed (%d)\n", n);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = (WiFi.SSID(i) == WIFI_SSID);
    found |= match;
    Serial.printf("  %-24s ch%-3d %4d dBm %-26s%s\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
                  WiFi.RSSI(i), authName(WiFi.encryptionType(i)), match ? "  <-- target" : "");
  }
  // Don't over-claim: "found" only rules out the name. Whether it's the
  // credential or the link is the reason code's job, not the scan's.
  if (!found) {
    Serial.printf("target \"%s\": NOT FOUND -- wrong name, or 5GHz-only (%d APs)\n", WIFI_SSID, n);
  } else {
    Serial.printf("target \"%s\": on the air. last reason %u %s (%d APs)\n", WIFI_SSID,
                  lastDisconnectReason, reasonName(lastDisconnectReason), n);
  }
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // scanning tore the connection down
}

// Associated but nothing works: separate "no DNS" from "traffic blocked" from
// "TLS problem". A guest VLAN with a portal or a restrictive firewall hands out
// a DHCP lease and then drops everything, which looks identical to a broken
// app until you test the layers separately.
//
// ponytail: lives here rather than lib/board/ because desk-clock is the only
// app using it today. Promote it to lib/board/netdiag.h the moment a second
// networked app needs it -- unifi-status and notifier both will.
static void diagnoseNet() {
  Serial.printf("ip %s  gw %s  dns %s  rssi %d\n", WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
                WiFi.RSSI());

  IPAddress addr;
  bool dns = WiFi.hostByName("api.open-meteo.com", addr);
  Serial.printf("dns  api.open-meteo.com: %s %s\n", dns ? "ok" : "FAILED",
                dns ? addr.toString().c_str() : "");

  WiFiClient c;
  bool gw = c.connect(WiFi.gatewayIP(), 53, 3000);
  Serial.printf("tcp  gateway:53 -> %s\n", gw ? "ok" : "refused/blocked");
  c.stop();

  if (dns) {
    WiFiClient t;
    bool tcp = t.connect(addr, 443, 5000);
    Serial.printf("tcp  %s:443 -> %s\n", addr.toString().c_str(),
                  tcp ? "ok" : "BLOCKED or no route out");
    t.stop();

    // The decisive test. A captive portal accepts TCP to *any* destination so
    // it can serve a redirect, then closes TLS because it cannot MITM it --
    // which looks exactly like "TCP ok, TLS EOF". On plain HTTP it shows itself.
    WiFiClient h;
    if (h.connect(addr, 80, 5000)) {
      h.print("GET / HTTP/1.1\r\nHost: api.open-meteo.com\r\nConnection: close\r\n\r\n");
      uint32_t t0 = millis();
      while (!h.available() && millis() - t0 < 5000) delay(10);
      Serial.printf("http status: %s\n", h.readStringUntil('\n').c_str());
      while (h.available()) {
        String l = h.readStringUntil('\n');
        if (l.length() < 2) break;
        if (l.startsWith("Location:") || l.startsWith("location:")) {
          Serial.printf("redirected to: %s   <-- captive portal\n", l.c_str());
          break;
        }
      }
    }
    h.stop();
  }

  // Rules out the other cause of a failed handshake: TLS wants ~40KB of
  // contiguous heap, and a fragmented heap fails in a way that looks similar.
  Serial.printf("heap free %u  largest block %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

static bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  NetworkClientSecure client;
  // ponytail: a public read-only API over TLS on a 512KB chip. Pinning a cert
  // costs heap and buys nothing here -- there is no secret in this request.
  client.setInsecure();

  char url[320];
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=4",
           LAT, LON);

  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != 200) {
    Serial.printf("wx http %d\n", status);
    http.end();
    return false;
  }

  // Read into a String rather than deserializing straight from the stream: the
  // response is well under 2KB, and holding it means a parse failure can show
  // what actually arrived instead of leaving you guessing.
  String body = http.getString();
  http.end();

  // The filter is mandatory, not an optimization: the full open-meteo document
  // will not fit in this heap alongside the display buffers.
  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["apparent_temperature"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("wx json %s -- body[0..200]: %s\n", err.c_str(), body.substring(0, 200).c_str());
    return false;
  }

  // Validate before trusting it. A 200 with a body that isn't open-meteo (a
  // portal page, a proxy error) deserializes fine and filters down to nothing,
  // and the `| default` operators below then happily produce "0.0C ?" as if it
  // were real weather. Seen exactly that once.
  if (!doc["current"]["weather_code"].is<int>() ||
      !doc["current"]["temperature_2m"].is<float>()) {
    Serial.printf("wx payload has no usable 'current' -- body[0..240]: %s\n",
                  body.substring(0, 240).c_str());
    return false;
  }

  wx.temp = doc["current"]["temperature_2m"] | 0.0f;
  wx.feels = doc["current"]["apparent_temperature"] | 0.0f;
  wx.humidity = doc["current"]["relative_humidity_2m"] | 0;
  wx.code = doc["current"]["weather_code"] | -1;

  // Index 0 of the daily arrays is today; the forecast columns want 1..3.
  for (int i = 0; i < 3; i++) {
    wx.dayCode[i] = doc["daily"]["weather_code"][i + 1] | -1;
    wx.hi[i] = lroundf(doc["daily"]["temperature_2m_max"][i + 1] | 0.0f);
    wx.lo[i] = lroundf(doc["daily"]["temperature_2m_min"][i + 1] | 0.0f);

    time_t d = time(nullptr) + (time_t)(i + 1) * 86400;
    struct tm dt;
    localtime_r(&d, &dt);
    strftime(wx.day[i], sizeof wx.day[i], "%a", &dt);
  }

  wx.valid = true;
  wx.fetchedAt = millis();
  Serial.printf("wx ok %.1fC %s\n", wx.temp, wmoLabel(wx.code));
  return true;
}

// ponytail: three bands, not a gradient -- a gradient is more code carrying the
// same information. Values are deliberately dim; this LED is bright.
static void ledByTemp(float c) {
  if (c <= 0.0f)      led(0, 0, 20);
  else if (c < 18.0f) led(0, 16, 6);
  else                led(22, 5, 0);
}

// ── mode + button ────────────────────────────────────────────────────────
// ── self-check ───────────────────────────────────────────────────────────
// Both layouts are asserted, not just the one in use -- that is the point of
// making orientation runtime, and it is stronger coverage than before.
static void checkLayout(const Layout *l) {
  assert(l->xTime >= 0 && l->xTime + GW(5) * 5 <= l->w);  // "14:32"
  assert(l->xDate + GW(2) * 13 <= l->w);                  // date footer
  assert(l->xStatus + GW(1) * 26 <= l->w);                // status strip
  assert(l->xFc0 + 3 * l->fcPitch <= l->w);               // three forecast columns
  assert(l->yStale + GH(1) <= l->h);                      // footer on-screen
  assert(l->yFcCnd + GH(1) <= l->h);
  assert(l->fcChars >= 7);                                // "drizzle"/"showers"
  assert((l->w / GW(1)) - 2 >= 22);                        // offline panel lines
  assert(8 + GW(3) * 8 <= l->w);                           // "NO CLOCK" at size 3
  assert((l->h - 62 - 16) / 11 >= 6);                      // enough offline rows
  // The hold hint is centred, so the longest one must fit this orientation.
  assert(GW(1) * 18 + 10 <= l->w);                         // "release: SCREEN OFF"
}

static void selfCheck() {
  assert(strcmp(wmoLabel(0), "clear") == 0);
  assert(strcmp(wmoLabel(2), "partly") == 0);
  assert(strcmp(wmoLabel(3), "cloudy") == 0);
  assert(strcmp(wmoLabel(65), "rain") == 0);
  assert(strcmp(wmoLabel(73), "snow") == 0);
  assert(strcmp(wmoLabel(86), "snow") == 0);
  assert(strcmp(wmoLabel(99), "storm") == 0);
  assert(strcmp(wmoLabel(-1), "?") == 0);
  assert(strcmp(wmoLabel(1234), "?") == 0);
  assert(strlen(wmoLabel(51)) <= 7);
  assert(strlen(wmoLabel(80)) <= 7);

  checkLayout(&PORTRAIT);
  checkLayout(&LANDSCAPE);
  assert(PORTRAIT.w == LCD_NATIVE_W && PORTRAIT.h == LCD_NATIVE_H);
  assert(LANDSCAPE.w == LCD_NATIVE_H && LANDSCAPE.h == LCD_NATIVE_W);
}

// ── main ─────────────────────────────────────────────────────────────────
enum class State { Boot, NoWifi, NoTime, Running };
static State state = State::Boot;

void setup() {
  Serial.begin(115200);
  // Native USB CDC: anything printed in the first few hundred ms is lost while
  // the host is still enumerating the port. Don't block on Serial being open --
  // this has to boot headless too.
  delay(300);
  selfCheck();

  pinMode(BTN_BOOT, INPUT_PULLUP);

  gfx = boardDisplay();
  gfx->begin();
  uiBegin(gfx, "deskclock");  // loads rotation/theme from NVS and applies them
  syncLayout();

  const char *boot[] = {"connecting to wifi", WIFI_SSID, "", "BOOT cycles modes"};
  drawOfflinePanel("STARTING", uiTheme()->muted, boot, 4);

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWiFiEvent);
  // The core defaults to _persistent = true, which writes the SSID and PSK into
  // NVS as well as having them compiled into the app partition. This keeps the
  // credential out of NVS (WIFI_STORAGE_RAM) -- one copy instead of two. It does
  // not hide the compiled-in copy; see SECURITY.md for what actually helps.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
#if POWER_SAVE
  // MAX_MODEM parks the radio between beacon intervals. Costs a little latency
  // on inbound packets, which a clock polling every 15 minutes never notices.
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  // 20s, not 10: the WPA2 handshake can time out once (204) and succeed on the
  // next attempt. A 10s window reported a failure that would have connected,
  // and I misread that as a wrong password twice.
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s %ddBm ip %s\n", WiFi.SSID().c_str(), WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
    configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    bool synced = false;
    for (int i = 0; i < 40 && !(synced = getLocalTime(&t, 250)); i++) {}
    Serial.printf("ntp %s\n", synced ? "ok" : "FAILED");
    // Associated but no time means traffic is being dropped, not that the app
    // is broken. Find out where before blaming the code.
    if (!synced) diagnoseNet();
  } else {
    Serial.println("wifi FAILED - check lib/board/secrets.h");
    diagnoseWiFi();
  }

  // Logged here rather than inside selfCheck(): USB CDC needs ~2s to enumerate,
  // so anything printed at the top of setup() is never seen. The asserts still
  // run first, where they can abort before anything is drawn.
  Serial.println("selfcheck ok");

  // Build-freshness check. PlatformIO caches objects, and a stale build that
  // still holds an old credential is indistinguishable from a wrong password.
  // FNV-1a is a non-reversible checksum -- it proves the binary matches the
  // file on disk without putting the secret anywhere.
  uint32_t h = 2166136261u;
  for (const char *p = WIFI_PASS; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  Serial.printf("cred check: ssid=\"%s\" passlen=%u fnv1a=%08x\n", WIFI_SSID,
                (unsigned)strlen(WIFI_PASS), h);

#if POWER_SAVE
  // 80MHz is the floor that still supports Wi-Fi on this chip. Nothing here is
  // compute-bound -- the panel is SPI-limited and the clock ticks once a second
  // -- so halving the clock costs nothing observable.
  setCpuFrequencyMhz(80);
#endif
  Serial.printf("power: cpu %uMHz, wifi sleep %s, die %.1fC\n", getCpuFrequencyMhz(),
                POWER_SAVE ? "MAX_MODEM" : "default", temperatureRead());
}

void loop() {
  static uint32_t lastWx = 0;
  static int lastMinute = -1;

  // uiHandle() owns blank/wake/theme/rotate and says whether we must repaint.
  if (uiHandle(uiPoll())) {
    syncLayout();
    invalidateCache();
    state = State::Boot;  // force the full redraw below
  }

  struct tm t;
  bool haveTime = getLocalTime(&t, 100);
  State want = (WiFi.status() != WL_CONNECTED) ? State::NoWifi
               : !haveTime                     ? State::NoTime
                                               : State::Running;

  if (want != state) {
    state = want;
    invalidateCache();
    if (uiScreenOn() && state == State::Running) {
      drawChrome();
      drawWeather();
    }
  }

  if (!uiScreenOn()) {
    // Deliberately keep fetching below: the point of blanking rather than
    // sleeping is that the clock is already right when it comes back.
  } else if (state == State::Running) {
    drawClock(t);
    drawDate(t);

    if (t.tm_min != lastMinute) {
      lastMinute = t.tm_min;
      backlight(uiBacklightNow());  // theme-specific day/night level
    }

    // Staleness has to be visible: a frozen panel showing nice weather is worse
    // than one that admits it lost the network.
    char stale[34];
    if (wx.valid) {
      snprintf(stale, sizeof stale, "wx %lum ago", (millis() - wx.fetchedAt) / 60000);
    } else {
      snprintf(stale, sizeof stale, "no weather - see serial");
    }
    if (strcmp(stale, cStale) != 0) {
      field(L->xStatus, L->yStale, 26, 1, wx.valid ? uiTheme()->dim : uiTheme()->warn, stale);
      strcpy(cStale, stale);
    }

    char net[34];
    snprintf(net, sizeof net, "%s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
    if (strcmp(net, cStatus) != 0) {
      field(L->xStatus, L->yStatus, 26, 1, uiTheme()->muted, net);
      strcpy(cStatus, net);
    }
  } else {
    // Offline panels: repaint only when their content actually changes, then
    // tick one line at the bottom so it's visibly alive rather than frozen.
    char key[48];
    snprintf(key, sizeof key, "%d-%u-%d-%u", (int)state, lastDisconnectReason,
             state == State::NoTime ? WiFi.RSSI() / 5 : 0, (unsigned)(uiRot() * 8 + uiScheme()));
    if (strcmp(key, cStatusKey) != 0) {
      strcpy(cStatusKey, key);
      if (state == State::NoWifi) {
        drawNoWifi();
      } else if (state == State::NoTime) {
        drawNoTime();
      }
    }
    char up[34];
    snprintf(up, sizeof up, "for %lus  BOOT=mode", millis() / 1000);
    if (strcmp(up, cStale) != 0) {
      field(8, L->h - 12, (L->w / GW(1)) - 2, 1, uiTheme()->dim, up);
      strcpy(cStale, up);
    }
    led(0, 0, 0);
  }

  // ponytail: this GET blocks for a second or two on a single-core chip, but
  // the clock reads the RTC rather than counting ticks, so the seconds just
  // jump and self-correct. Not worth a task for a 15-minute poll.
  uint32_t period = wx.valid ? WX_PERIOD_MS : WX_RETRY_MS;
  if (state != State::NoWifi && (lastWx == 0 || millis() - lastWx > period)) {
    lastWx = millis();
    if (fetchWeather()) {
      if (uiScreenOn() && state == State::Running) drawWeather();
      if (uiScreenOn()) ledByTemp(wx.temp);
    }
  }

  static uint32_t lastTemp = 0;
  if (millis() - lastTemp > TEMP_LOG_MS) {
    lastTemp = millis();
    Serial.printf("die %.1fC  cpu %uMHz  bl %u  heap %u\n", temperatureRead(),
                  getCpuFrequencyMhz(), uiBacklightApplied, ESP.getFreeHeap());
  }

  delay(100);
}
