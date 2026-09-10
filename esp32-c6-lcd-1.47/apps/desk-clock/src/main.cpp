// desk-clock: NTP clock + open-meteo weather on the 172x320 panel.
//
// Layout is three stacked blocks (time / current / forecast) plus a date
// footer. All coordinates are fixed at compile time so every redraw erases an
// exact box -- there is no fillScreen() in loop(), which is what stops the
// large digits from flickering.
#include <board.h>
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
static const uint8_t BL_DAY = 200, BL_NIGHT = 50;       // ponytail: tune these by eye, in the dark
static const uint8_t NIGHT_FROM = 23, NIGHT_TO = 7;

// The built-in 6x8 font scales by integer size, so a glyph is exactly
// 6*size wide and 8*size tall. That exactness is why the dirty rects below
// can be hardcoded.
#define GW(size) (6 * (size))
#define GH(size) (8 * (size))

static Arduino_GFX *gfx;

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
    case 0: case 1: return RGB565_ORANGE;
    case 71: case 73: case 75: case 77: case 85: case 86: return RGB565_CYAN;
    case 61: case 63: case 65: case 80: case 81: case 82: return RGB565_SKYBLUE;
    case 95: case 96: case 99: return RGB565_RED;
    default: return RGB565_LIGHTGREY;
  }
}

// ── drawing helpers ──────────────────────────────────────────────────────
// Erase an exact box, then draw into it. `chars` sizes the box, so a shorter
// string never leaves stale pixels from a longer one behind.
static void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg,
                  const char *s, bool center = false) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(x, y, w, GH(size), RGB565_BLACK);
  int16_t tx = center ? x + (w - (int16_t)(GW(size) * strlen(s))) / 2 : x;
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(tx, y);
  gfx->print(s);
}

static void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size,
                       uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  int16_t x = right - w;
  gfx->fillRect(x, y, w, GH(size), RGB565_BLACK);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(right - (int16_t)(GW(size) * strlen(s)), y);
  gfx->print(s);
}

// The 6x8 font has no reliable degree glyph, so draw it as a ring.
static void degree(int16_t x, int16_t y, uint16_t fg) { gfx->drawCircle(x, y, 2, fg); }

// ── layout ───────────────────────────────────────────────────────────────
// y-coordinates in one place so the blocks can be nudged without hunting.
enum : int16_t {
  Y_TIME = 24,   // size 5 -> 40 tall
  Y_SECS = 70,   // size 2 -> 16 tall
  Y_RULE1 = 96,
  Y_PLACE = 106,
  Y_TEMP = 120,  // size 4 -> 32 tall
  Y_META = 120,  // right column, size 1, 3 rows
  Y_RULE2 = 162,
  Y_FCDAY = 172,
  Y_FCTMP = 186,
  Y_FCCND = 200,
  Y_RULE3 = 216,
  Y_DATE = 228,  // size 2
  Y_STATUS = 296,
  Y_STALE = 308,
};
static const int16_t X_TIME = (LCD_W - GW(5) * 5) / 2;  // 5 glyphs: "14:32"
static const int16_t X_SECS = (LCD_W - GW(2) * 2) / 2;

static void drawChrome() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawFastHLine(12, Y_RULE1, LCD_W - 24, RGB565_DARKGREY);
  gfx->drawFastHLine(12, Y_RULE2, LCD_W - 24, RGB565_DARKGREY);
  gfx->drawFastHLine(12, Y_RULE3, LCD_W - 24, RGB565_DARKGREY);
  field(8, Y_PLACE, 12, 1, RGB565_GREY, PLACE);
}

// Only repaints what changed -- the minute block once a minute, seconds once a
// second. Repainting the whole screen here is what makes big digits flicker.
static void drawClock(const struct tm &t) {
  static char lastHM[6] = "", lastSS[3] = "";
  char hm[6], ss[3];
  strftime(hm, sizeof hm, "%H:%M", &t);
  strftime(ss, sizeof ss, "%S", &t);

  if (strcmp(hm, lastHM) != 0) {
    field(X_TIME, Y_TIME, 5, 5, RGB565_WHITE, hm);
    strcpy(lastHM, hm);
  }
  if (strcmp(ss, lastSS) != 0) {
    field(X_SECS, Y_SECS, 2, 2, RGB565_DIMGREY, ss);
    strcpy(lastSS, ss);
  }
}

static void drawDate(const struct tm &t) {
  static char last[18] = "";
  char d[18];
  strftime(d, sizeof d, "%a %d %b", &t);
  if (strcmp(d, last) == 0) return;
  field(8, Y_DATE, 14, 2, RGB565_WHITE, d, true);
  strcpy(last, d);
}

static void drawWeather() {
  char buf[16];

  if (!wx.valid) {
    field(8, Y_TEMP, 6, 4, RGB565_DIMGREY, "--");
    return;
  }

  snprintf(buf, sizeof buf, "%d", (int)lroundf(wx.temp));
  field(8, Y_TEMP, 4, 4, wmoColor(wx.code), buf);
  degree(8 + GW(4) * strlen(buf) + 5, Y_TEMP + 5, wmoColor(wx.code));

  snprintf(buf, sizeof buf, "feels %d", (int)lroundf(wx.feels));
  fieldRight(LCD_W - 8, Y_META, 12, 1, RGB565_GREY, buf);
  fieldRight(LCD_W - 8, Y_META + 14, 12, 1, wmoColor(wx.code), wmoLabel(wx.code));
  snprintf(buf, sizeof buf, "%d%% hum", wx.humidity);
  fieldRight(LCD_W - 8, Y_META + 28, 12, 1, RGB565_GREY, buf);

  // Three forecast columns across 172px.
  for (int i = 0; i < 3; i++) {
    int16_t x = 8 + i * 52;
    field(x, Y_FCDAY, 8, 1, RGB565_GREY, wx.day[i], true);
    snprintf(buf, sizeof buf, "%d/%d", wx.hi[i], wx.lo[i]);
    field(x, Y_FCTMP, 8, 1, RGB565_WHITE, buf, true);
    field(x, Y_FCCND, 8, 1, wmoColor(wx.dayCode[i]), wmoLabel(wx.dayCode[i]), true);
  }
}

// ── network ──────────────────────────────────────────────────────────────
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
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("wx json %s\n", err.c_str());
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
  if (c <= 0.0f)       led(0, 0, 20);
  else if (c < 18.0f)  led(0, 16, 6);
  else                 led(22, 5, 0);
}

// ── self-check ───────────────────────────────────────────────────────────
// One runnable check, per the repo's habit. Covers the code->label map, which
// is the only logic here that can be wrong without being obvious on screen.
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
  assert(X_TIME >= 0 && X_TIME + GW(5) * 5 <= LCD_W);  // "14:32" must fit
}

// ── main ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // Native USB CDC: anything printed in the first few hundred ms is lost while
  // the host is still enumerating the port. Don't block on Serial being open --
  // this has to boot headless too.
  delay(300);
  selfCheck();

  gfx = boardDisplay();
  gfx->begin();
  backlight(BL_DAY);
  drawChrome();

  field(8, Y_STATUS, 26, 1, RGB565_GREY, "wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s %ddBm ip %s\n", WiFi.SSID().c_str(), WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
    field(8, Y_STATUS, 26, 1, RGB565_GREY, "ntp...");
    configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
    // Wait for a real time, not 1970, before drawing a clock.
    struct tm t;
    bool synced = false;
    for (int i = 0; i < 40 && !(synced = getLocalTime(&t, 250)); i++) {}
    Serial.printf("ntp %s\n", synced ? "ok" : "FAILED");
  } else {
    Serial.println("wifi FAILED - check lib/board/secrets.h");
    field(8, Y_STATUS, 26, 1, RGB565_RED, "no wifi");
  }

  // Logged here rather than inside selfCheck(): USB CDC needs ~2s to enumerate,
  // so anything printed at the top of setup() is never seen. The asserts still
  // run first, where they can abort before anything is drawn.
  Serial.println("selfcheck ok");
}

void loop() {
  static uint32_t lastWx = 0;
  static int lastMinute = -1;

  struct tm t;
  if (getLocalTime(&t, 100)) {
    drawClock(t);
    drawDate(t);

    if (t.tm_min != lastMinute) {
      lastMinute = t.tm_min;
      bool night = (t.tm_hour >= NIGHT_FROM || t.tm_hour < NIGHT_TO);
      backlight(night ? BL_NIGHT : BL_DAY);
    }
  }

  // ponytail: this GET blocks for a second or two on a single-core chip, but
  // the clock reads the RTC rather than counting ticks, so the seconds just
  // jump and self-correct. Not worth a task for a 15-minute poll.
  if (lastWx == 0 || millis() - lastWx > WX_PERIOD_MS) {
    lastWx = millis();
    if (fetchWeather()) {
      drawWeather();
      ledByTemp(wx.temp);
    } else if (!wx.valid) {
      drawWeather();
    }
  }

  // Staleness has to be visible: a frozen panel showing nice weather is worse
  // than one that admits it lost the network.
  static char lastStale[26] = "";
  char stale[26];
  if (wx.valid) {
    snprintf(stale, sizeof stale, "wx %lum ago", (millis() - wx.fetchedAt) / 60000);
  } else {
    snprintf(stale, sizeof stale, "wx unavailable");
  }
  if (strcmp(stale, lastStale) != 0) {
    field(8, Y_STALE, 26, 1, wx.valid ? RGB565_DIMGREY : RGB565_RED, stale);
    strcpy(lastStale, stale);
  }

  static char lastNet[26] = "";
  char net[26];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(net, sizeof net, "%s %ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
  } else {
    // No manual reconnect here: setAutoReconnect() already retries, and calling
    // WiFi.reconnect() from a 250ms loop just spams
    // "sta is connecting, return error" while a connect is already in flight.
    snprintf(net, sizeof net, "wifi down, retrying");
  }
  if (strcmp(net, lastNet) != 0) {
    field(8, Y_STATUS, 26, 1,
          WiFi.status() == WL_CONNECTED ? RGB565_GREY : RGB565_RED, net);
    strcpy(lastNet, net);
  }

  delay(250);
}
