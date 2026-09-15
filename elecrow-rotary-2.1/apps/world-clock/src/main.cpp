// World clock: one city per screen, the knob spins the globe.
//
// Turn to move through the zones in config.json; the ring of dots around the
// edge shows every zone at its UTC offset relative to the one on screen, so
// the whole ring rotates as you turn. Press to jump home (zone 0). Time comes
// from NTP once and is kept by the RTC; each zone is rendered by switching the
// libc TZ, so daylight saving is handled by the POSIX rule string, not by us.
#include <HTTPClient.h>
#include <appcfg.h>
#include <board.h>
#include <netjoin.h>
#include <secrets.h>
#include <time.h>

#include "globe.h"

static Arduino_GFX *gfx;

// ── config ────────────────────────────────────────────────────────────────
#define MAX_ZONES 16
#define NAME_CHARS 12  // width of the city box at size 4: 12 * 24 = 288px
struct Zone {
  char name[NAME_CHARS + 1];
  char tz[64];
  float lat, lon;  // where the city dot goes on the globe; NAN if the config gave none
};
static Zone zones[MAX_ZONES];
static uint8_t nZones = 0;
static bool hour24 = true;
static uint8_t blDay = 204, blNight = 60, nightFrom = 23, nightTo = 7;
static float globeTilt = 30.0f;
static bool fahrenheit = false;

// Current weather per zone from open-meteo, fetched when a zone has been
// selected for a moment (so spinning past it costs nothing) and kept 15 min.
struct Wx {
  uint32_t at = 0;  // millis() of the last fetch, 0 = never
  float temp = 0;
  int code = -1;
};
static Wx wx[MAX_ZONES];
static const uint32_t WX_KEEP_MS = 15UL * 60 * 1000, WX_RETRY_MS = 2UL * 60 * 1000, WX_SETTLE_MS = 1500;

static void defaultZones() {
  static const Zone d[] = {
      {"TORONTO", "EST5EDT,M3.2.0/2,M11.1.0/2", 43.65f, -79.38f},
      {"VANCOUVER", "PST8PDT,M3.2.0/2,M11.1.0/2", 49.28f, -123.12f},
      {"LONDON", "GMT0BST,M3.5.0/1,M10.5.0", 51.51f, -0.13f},
      {"PARIS", "CET-1CEST,M3.5.0,M10.5.0/3", 48.86f, 2.35f},
      {"DUBAI", "<+04>-4", 25.20f, 55.27f},
      {"MUMBAI", "IST-5:30", 19.08f, 72.88f},
      {"TOKYO", "JST-9", 35.68f, 139.69f},
      {"SYDNEY", "AEST-10AEDT,M10.1.0,M4.1.0/3", -33.87f, 151.21f},
  };
  nZones = sizeof d / sizeof d[0];
  memcpy(zones, d, sizeof d);
}

// ── time maths ────────────────────────────────────────────────────────────
// Local wall time in a zone. Switching TZ per call is cheap (tzset parses a
// short string) and means DST rules live in the config, not in code.
static void zoneLocal(const char *tz, time_t t, struct tm *out) {
  setenv("TZ", tz, 1);
  tzset();
  localtime_r(&t, out);
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// UTC offset of a zone at instant t, in minutes. Derived from the wall clock
// rather than tm_gmtoff, which newlib does not reliably provide.
static int offsetMin(const char *tz, time_t t) {
  struct tm lt;
  zoneLocal(tz, t, &lt);
  int64_t local = (int64_t)daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 +
                  lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  return (int)((local - (int64_t)t) / 60);
}

// Longitude the globe turns to for a zone: the city's if the config gave one,
// else the meridian its UTC offset implies. Keeps the globe turning for a
// zone list with no coordinates, which is exactly what a first config looks
// like.
static float zoneLon(uint8_t i, time_t now) {
  return isnan(zones[i].lon) ? offsetMin(zones[i].tz, now) / 4.0f : zones[i].lon;
}

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

// Where a zone sits on the ring when `sel` is on screen (top): 15 degrees per
// hour of offset difference, clockwise for zones ahead of the selected one.
static float ringAngle(int offMin, int selOffMin) { return (offMin - selOffMin) * (360.0f / 1440.0f); }

// Pads s to exactly n chars, centred, so a fixed-width field erases cleanly.
static void centre(char *out, size_t n, const char *s) {
  size_t len = strlen(s);
  if (len > n) len = n;
  size_t left = (n - len) / 2;
  memset(out, ' ', n);
  memcpy(out + left, s, len);
  out[n] = '\0';
}

static void selfCheck() {
  // 2026-01-15 12:00 UTC and 2026-07-15 12:00 UTC: one side of DST each.
  const time_t jan = 1768478400, jul = 1784116800;
  struct tm t;
  zoneLocal("EST5EDT,M3.2.0/2,M11.1.0/2", jan, &t); assert(t.tm_hour == 7);
  zoneLocal("EST5EDT,M3.2.0/2,M11.1.0/2", jul, &t); assert(t.tm_hour == 8);
  zoneLocal("GMT0BST,M3.5.0/1,M10.5.0", jan, &t);   assert(t.tm_hour == 12);
  zoneLocal("GMT0BST,M3.5.0/1,M10.5.0", jul, &t);   assert(t.tm_hour == 13);
  zoneLocal("JST-9", jan, &t);                      assert(t.tm_hour == 21);
  zoneLocal("IST-5:30", jan, &t);                   assert(t.tm_hour == 17 && t.tm_min == 30);
  zoneLocal("AEST-10AEDT,M10.1.0,M4.1.0/3", jan, &t); assert(t.tm_hour == 23);
  zoneLocal("<+04>-4", jan, &t);                    assert(t.tm_hour == 16);

  assert(daysFromCivil(1970, 1, 1) == 0);
  assert(daysFromCivil(2000, 3, 1) == 11017);
  assert(daysFromCivil(2026, 1, 15) == jan / 86400);
  assert(offsetMin("EST5EDT,M3.2.0/2,M11.1.0/2", jan) == -300);
  assert(offsetMin("EST5EDT,M3.2.0/2,M11.1.0/2", jul) == -240);
  assert(offsetMin("IST-5:30", jan) == 330);
  assert(offsetMin("<+04>-4", jan) == 240);

  assert(!strcmp(wmoLabel(0), "clear") && !strcmp(wmoLabel(63), "rain") && !strcmp(wmoLabel(42), "?"));

  assert(ringAngle(540, -300) == 210.0f);   // Tokyo seen from Toronto in winter
  assert(ringAngle(-300, -300) == 0.0f);
  assert(ringAngle(-480, -300) == -45.0f);  // Vancouver: 3h behind, anticlockwise

  char b[NAME_CHARS + 1];
  centre(b, NAME_CHARS, "TOKYO");  assert(!strcmp(b, "   TOKYO    "));
  centre(b, NAME_CHARS, "");       assert(!strcmp(b, "            "));
  centre(b, 4, "TOOLONGNAME");     assert(!strcmp(b, "TOOL"));
}

// ── drawing ───────────────────────────────────────────────────────────────
// Built-in font: 6*size x 8*size per glyph, so each field is an exact box.
// Erasing means putting the globe back, not painting black.
static void field(int16_t x, int16_t y, uint8_t size, uint8_t chars, uint16_t fg, const char *s) {
  globe::restore(gfx, x, y, 6 * size * chars, 8 * size);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static const int16_t CX = 240, CY = 240, RING_R = 222, DOT_R = 6;
static uint8_t sel = 0;
static int dotOff[MAX_ZONES];

// City dots on the globe: selected in yellow, home in cyan, each with a dark
// outline so they read on land and sea alike. Drawn last, over the text.
static void drawCities(time_t now) {
  int16_t x, y;
  float lon0 = zoneLon(sel, now);
  if (sel != 0 && !isnan(zones[0].lat) && globe::project(zones[0].lat, zones[0].lon, lon0, &x, &y)) {
    gfx->fillCircle(x, y, 6, RGB565_BLACK);
    gfx->fillCircle(x, y, 4, RGB565_CYAN);
  }
  if (!isnan(zones[sel].lat) && globe::project(zones[sel].lat, zones[sel].lon, lon0, &x, &y)) {
    gfx->fillCircle(x, y, 7, RGB565_BLACK);
    gfx->fillCircle(x, y, 5, RGB565_YELLOW);
  }
}

static void dotAt(float deg, int16_t r, uint16_t c) {
  float a = (deg - 90.0f) * (float)M_PI / 180.0f;
  gfx->fillCircle(CX + (int16_t)lroundf(RING_R * cosf(a)), CY + (int16_t)lroundf(RING_R * sinf(a)), r, c);
}

static void drawRing(time_t now) {
  int selOff = offsetMin(zones[sel].tz, now);
  gfx->drawCircle(CX, CY, RING_R, RGB565_DARKGREY);
  for (uint8_t i = 0; i < nZones; i++) dotOff[i] = offsetMin(zones[i].tz, now);
  for (uint8_t i = 0; i < nZones; i++)
    if (i != sel) dotAt(ringAngle(dotOff[i], selOff), DOT_R, i == 0 ? RGB565_CYAN : RGB565_DARKGREY);
  dotAt(0, DOT_R + 3, RGB565_YELLOW);  // the selected zone, always at the top
}

// Weather line under the offset: "22C rain", blank until fetched or when the
// zone has no coordinates to ask about.
static void drawWeather() {
  char b[24], line[17];
  if (isnan(zones[sel].lat)) b[0] = '\0';
  else if (wx[sel].code < 0) snprintf(b, sizeof b, "%s", wx[sel].at ? "no weather" : "weather...");
  else snprintf(b, sizeof b, "%d%c %s", (int)lroundf(wx[sel].temp), fahrenheit ? 'F' : 'C', wmoLabel(wx[sel].code));
  centre(line, 16, b);
  field(96, 372, 3, 16, wx[sel].code < 0 ? RGB565_DARKGREY : RGB565_WHITE, line);
}

static bool isNight(int hour) {
  return nightFrom > nightTo ? (hour >= nightFrom || hour < nightTo) : (hour >= nightFrom && hour < nightTo);
}

static void drawZone(time_t now, bool full) {
  static int lastMin = -1, lastDay = -1;
  struct tm t;
  zoneLocal(zones[sel].tz, now, &t);
  char b[32], c[NAME_CHARS + 1];
  // Night in the zone on screen: cool digits, so 03:00 somewhere reads as night
  // even when it's afternoon here.
  uint16_t digits = isNight(t.tm_hour) ? RGB565(120, 150, 255) : RGB565_WHITE;

  if (full) {
    centre(c, NAME_CHARS, zones[sel].name);
    field(96, 84, 4, NAME_CHARS, RGB565_YELLOW, c);
    int off = offsetMin(zones[sel].tz, now), home = offsetMin(zones[0].tz, now), d = off - home;
    char utc[12], vs[16];
    snprintf(utc, sizeof utc, "UTC%+d", off / 60);
    if (off % 60) snprintf(utc, sizeof utc, "UTC%+d:%02d", off / 60, abs(off % 60));
    if (sel == 0) snprintf(vs, sizeof vs, "HOME");
    else if (d % 60) snprintf(vs, sizeof vs, "HOME%+d:%02dh", d / 60, abs(d % 60));
    else snprintf(vs, sizeof vs, "HOME%+dh", d / 60);
    snprintf(b, sizeof b, "%s  %s", utc, vs);
    char line[23];
    centre(line, 22, b);
    field(108, 336, 2, 22, RGB565_LIGHTGREY, line);
    drawWeather();
    lastMin = lastDay = -1;
  }
  if (t.tm_min != lastMin) {
    int h = hour24 ? t.tm_hour : (t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12);
    snprintf(b, sizeof b, "%02d:%02d", h, t.tm_min);
    field(120, 160, 8, 5, digits, b);
    if (!hour24) field(372, 208, 2, 2, digits, t.tm_hour < 12 ? "AM" : "PM");
    lastMin = t.tm_min;
  }
  snprintf(b, sizeof b, "%02d", t.tm_sec);
  field(222, 236, 3, 2, RGB565_DARKGREY, b);
  if (t.tm_yday != lastDay) {
    strftime(b, sizeof b, "%a %d %b", &t);
    char line[11];
    centre(line, 10, b);
    field(150, 284, 3, 10, RGB565_LIGHTGREY, line);
    lastDay = t.tm_yday;
  }
}

static void drawWaiting(const char *why) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(CX, CY, RING_R, RGB565_DARKGREY);
  static bool once = false;
  if (!once) { memset(globe::bg, 0, GLOBE_W * GLOBE_H * 2); once = true; }  // fields erase to black until the globe exists
  field(150, 200, 3, 10, RGB565_YELLOW, " NO TIME  ");
  char line[23];
  centre(line, 22, why);
  field(108, 250, 2, 22, RGB565_LIGHTGREY, line);
}

// ── setup / loop ──────────────────────────────────────────────────────────
static void loadConfig() {
  defaultZones();
  if (!cfgLoad()) return;
  hour24 = cfgBool("hour24", hour24);
  blDay = (uint8_t)cfgInt("brightness.day", blDay, 8, 255);
  blNight = (uint8_t)cfgInt("brightness.night", blNight, 8, 255);
  nightFrom = (uint8_t)cfgInt("night.from", nightFrom, 0, 23);
  nightTo = (uint8_t)cfgInt("night.to", nightTo, 0, 23);
  globeTilt = cfgFloat("globe.tilt", globeTilt, -60.0f, 60.0f);
  char units[12] = "metric";
  if (cfgStr("units", units, sizeof units)) {
    if (!strcmp(units, "imperial")) fahrenheit = true;
    else if (strcmp(units, "metric")) Serial.printf("config units: '%s' is not metric/imperial, keeping metric\n", units);
  }
  uint8_t n = 0;
  for (JsonVariant v : cfgArr("zones")) {
    if (n >= MAX_ZONES) { Serial.printf("config zones: more than %d, rest ignored\n", MAX_ZONES); break; }
    const char *name = v["name"] | (const char *)nullptr;
    const char *tz = v["tz"] | (const char *)nullptr;
    if (!name || !*name || !tz || !*tz) { Serial.printf("config zones[%u]: needs name and tz, skipped\n", n); continue; }
    snprintf(zones[n].name, sizeof zones[n].name, "%s", name);
    snprintf(zones[n].tz, sizeof zones[n].tz, "%s", tz);
    zones[n].lat = zones[n].lon = NAN;
    if (v["lat"].is<float>() && v["lon"].is<float>()) {
      float la = v["lat"].as<float>(), lo = v["lon"].as<float>();
      if (la < -90 || la > 90 || lo < -180 || lo > 180)
        Serial.printf("config zones[%u]: lat/lon out of range, no dot and no weather\n", n);
      else { zones[n].lat = la; zones[n].lon = lo; }
    }
    n++;
  }
  if (n) nZones = n;  // a present but empty list keeps the defaults
  cfgRelease();
}

static bool haveTime() { return time(nullptr) > 1700000000; }

// Blocking, ~1-2s for the TLS handshake. Fine: it only runs once a zone has
// sat selected for WX_SETTLE_MS, and the encoder keeps counting on interrupts
// underneath it. Same request shape and the same filter-and-validate rules as
// desk-clock's fetchWeather().
static bool fetchWeather(uint8_t z) {
  if (WiFi.status() != WL_CONNECTED) return false;
  NetworkClientSecure client;
  client.setInsecure();  // public read-only API; nothing secret in the request
  char url[200];
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code%s",
           zones[z].lat, zones[z].lon, fahrenheit ? "&temperature_unit=fahrenheit" : "");
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != 200) { Serial.printf("wx %s: http %d\n", zones[z].name, status); http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument filter, doc;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err || !doc["current"]["weather_code"].is<int>() || !doc["current"]["temperature_2m"].is<float>()) {
    Serial.printf("wx %s: bad payload (%s) body[0..160]: %s\n", zones[z].name, err ? err.c_str() : "no current",
                  body.substring(0, 160).c_str());
    return false;
  }
  wx[z].temp = doc["current"]["temperature_2m"].as<float>();
  wx[z].code = doc["current"]["weather_code"].as<int>();
  Serial.printf("wx %s: %.1f %s\n", zones[z].name, wx[z].temp, wmoLabel(wx[z].code));
  return true;
}

void setup() {
  Serial.begin(115200);
  bool xok = boardBegin();
  gfx = boardDisplay();
  gfx->begin();
  cfgSelfCheck();
  selfCheck();
  loadConfig();
  uint32_t t0 = millis();
  bool gok = globe::begin(globeTilt);
  if (gok) globe::selfCheck();
  Serial.printf("pcf8574 %s, %u zones, home %s, globe tables %s in %lums, psram %u KB free\n",
                xok ? "OK" : "MISSING", nZones, zones[0].name, gok ? "OK" : "NO PSRAM", millis() - t0,
                ESP.getFreePsram() / 1024);

  drawWaiting("joining wifi");
  backlight(blDay);
  encoderBegin();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinBest(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC; zones are applied per draw
}

void loop() {
  static int32_t lastPos = 0;
  static bool lastPressed = false, shown = false;
  static uint32_t lastJoin = 0, lastSec = 0;
  static uint16_t retries = 0;
  static uint8_t lastBl = 0;

  if (!haveTime()) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastJoin > netRetryDelay(retries)) {
      lastJoin = millis();
      retries++;
      netJoinBest(WIFI_SSID, WIFI_PASS);
      drawWaiting("joining wifi");
    } else if (WiFi.status() == WL_CONNECTED && !shown) {
      drawWaiting("waiting for ntp");
      shown = true;
    }
    delay(100);
    return;
  }

  time_t now = time(nullptr);
  bool full = false;
  int32_t pos = encoderPosition();
  if (pos != lastPos) {
    sel = (uint8_t)(((int32_t)sel + (pos - lastPos)) % nZones + nZones) % nZones;
    lastPos = pos;
    full = true;
  }
  bool pressed = knobPressed();
  if (pressed && !lastPressed && sel != 0) { sel = 0; full = true; }
  lastPressed = pressed;

  // The terminator moves a pixel every few minutes; re-render on the minute,
  // which is when the digits change anyway.
  static int lastMinute = -1;
  static uint32_t lastTurn = 0;
  int minute = (int)(now / 60);
  if (!shown || minute != lastMinute) { full = true; shown = true; lastMinute = minute; }
  if (full) {
    uint32_t t0 = millis();
    globe::render(zoneLon(sel, now), now);
    globe::blit(gfx);
    drawRing(now);
    drawZone(now, true);
    drawCities(now);
    Serial.printf("zone %u %s, redraw %lums\n", sel, zones[sel].name, millis() - t0);
    lastSec = (uint32_t)now;
    lastTurn = millis();
  } else if ((uint32_t)now != lastSec) {
    drawZone(now, false);
    drawCities(now);  // the seconds box may have covered a dot
    lastSec = (uint32_t)now;
  }

  // Weather for the zone on screen, once it has stopped moving and the cache
  // is stale. A failure backs off WX_RETRY_MS rather than hammering.
  if (!isnan(zones[sel].lat) && millis() - lastTurn > WX_SETTLE_MS &&
      (wx[sel].at == 0 || millis() - wx[sel].at > WX_KEEP_MS)) {
    uint8_t z = sel;
    bool ok = fetchWeather(z);
    wx[z].at = ok ? millis() : millis() - (WX_KEEP_MS - WX_RETRY_MS);
    if (!ok && wx[z].code < 0) wx[z].code = -1;
    if (z == sel && encoderPosition() == lastPos) drawWeather();
  }
  {
    // Backlight follows HOME's night window, not the zone on screen.
    struct tm h;
    zoneLocal(zones[0].tz, now, &h);
    uint8_t bl = isNight(h.tm_hour) ? blNight : blDay;
    if (bl != lastBl) { backlight(bl); lastBl = bl; }
  }
  delay(10);
}
