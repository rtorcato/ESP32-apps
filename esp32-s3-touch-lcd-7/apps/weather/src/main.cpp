// WEATHER -- now, the next 24 hours, and the week, for the Waveshare
// ESP32-S3-Touch-LCD-7.
//
// Open-Meteo (open-meteo.com), keyless and account-free: one request returns
// current conditions, hourly and daily together, which is the whole reason
// this is the easiest app on the board. The rotary's world-clock already
// talks to it, so the endpoint is proven on this hardware.
//
// The first app written ON the base rather than retrofitted onto it: Wi-Fi,
// theme, orientation and the sleep window all come from Home through
// baseos.h, and every widget on screen is lib/ui's. What is left is the
// fetch, the glyphs and three pages.
//
// Condition glyphs are drawn from primitives rather than fetched. There are
// nine of them, they are circles and lines, and a logo pipeline for nine
// shapes would be more machinery than the shapes.
#include <appcfg.h>
#include <baseos.h>
#include <board.h>
#include <helv.h>
#include <netjoin.h>
#include <sleep.h>
#include <ui.h>

#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

static const char *UA = "Mozilla/5.0 (esp32-weather)";
static const uint32_t REFRESH_MS = 10UL * 60 * 1000;  // Open-Meteo updates every 15min; asking oftener is noise

// ── where, and in what units ─────────────────────────────────────────────
// Toronto until config.json says otherwise. Coordinates rather than a place
// name because the forecast endpoint wants numbers and geocoding is a second
// request to get wrong.
static double lat = 43.6532, lon = -79.3832;
static char place[28] = "Toronto";
static bool metric = true;
static char tzString[48] = "EST5EDT,M3.2.0/2,M11.1.0/2";
static char wifiSsid[33] = "", wifiPass[65] = "";

// ── the data ─────────────────────────────────────────────────────────────
struct Now {
  float temp, feels, wind, gust;
  uint8_t humidity, windDir;
  uint16_t code;
  bool isDay, valid;
};
struct Hour {
  int8_t temp;
  uint8_t pop;
  uint16_t code;
};
struct Day {
  int8_t hi, lo;
  uint8_t pop;
  uint16_t code;
  char name[4];
};
static const uint8_t N_HOURS = 24, N_DAYS = 7;
static Now now_;
static Hour hours[N_HOURS];
static Day days[N_DAYS];
static uint8_t nHours = 0, nDays = 0;
static char sunrise[6] = "", sunset[6] = "";
static SemaphoreHandle_t mux;
static volatile uint32_t lastOk = 0;
static char fetchErr[48] = "";

// ── WMO weather codes ────────────────────────────────────────────────────
// Open-Meteo reports WMO 4677. Nine buckets is all a glyph can say apart,
// and all anyone reads at a glance.
enum class Sky : uint8_t { Clear, Few, Cloud, Fog, Drizzle, Rain, Snow, Showers, Storm };

static Sky skyOf(uint16_t code) {
  if (code == 0) return Sky::Clear;
  if (code <= 2) return Sky::Few;
  if (code == 3) return Sky::Cloud;
  if (code <= 48) return Sky::Fog;
  if (code <= 57) return Sky::Drizzle;
  if (code <= 67) return Sky::Rain;
  if (code <= 77) return Sky::Snow;
  if (code <= 86) return Sky::Showers;
  return Sky::Storm;
}

static const char *skyWord(uint16_t code) {
  switch (code) {
    case 0: return "clear";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: return "light showers";
    case 81: return "showers";
    case 82: return "violent showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm, hail";
    default: return "--";
  }
}

// ── the glyphs ───────────────────────────────────────────────────────────
// All built from circles and lines at a caller-given radius, so the same
// code draws the 96px one on Now and the 18px ones down the Hours strip.
static void gSun(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  gfx->fillCircle(cx, cy, r * 55 / 100, c);
  for (uint8_t i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4;
    int16_t x0 = cx + (int16_t)(cosf(a) * r * 0.75f), y0 = cy + (int16_t)(sinf(a) * r * 0.75f);
    int16_t x1 = cx + (int16_t)(cosf(a) * r), y1 = cy + (int16_t)(sinf(a) * r);
    gfx->drawLine(x0, y0, x1, y1, c);
    if (r > 30) gfx->drawLine(x0 + 1, y0, x1 + 1, y1, c);
  }
}
static void gCloud(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  gfx->fillCircle(cx - r / 2, cy + r / 6, r * 42 / 100, c);
  gfx->fillCircle(cx + r / 3, cy + r / 6, r * 34 / 100, c);
  gfx->fillCircle(cx - r / 12, cy - r / 5, r * 48 / 100, c);
  gfx->fillRoundRect(cx - r * 9 / 10, cy + r / 8, r * 18 / 10, r * 45 / 100, r / 5, c);
}
static void gDrops(int16_t cx, int16_t cy, int16_t r, uint16_t c, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    int16_t x = cx - r / 2 + i * (r / 2);
    gfx->drawLine(x, cy, x - r / 6, cy + r / 2, c);
    if (r > 30) gfx->drawLine(x + 1, cy, x - r / 6 + 1, cy + r / 2, c);
  }
}
static void gFlakes(int16_t cx, int16_t cy, int16_t r, uint16_t c, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    int16_t x = cx - r / 2 + i * (r / 2), y = cy + r / 4;
    int16_t s = r / 7 > 2 ? r / 7 : 2;
    gfx->drawFastHLine(x - s, y, s * 2, c);
    gfx->drawFastVLine(x, y - s, s * 2, c);
  }
}
static void gBolt(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t w = r / 4 > 2 ? r / 4 : 2;
  for (int16_t d = 0; d < w; d++) {
    gfx->drawLine(cx + d, cy, cx - r / 4 + d, cy + r / 2, c);
    gfx->drawLine(cx - r / 4 + d, cy + r / 2, cx + r / 6 + d, cy + r / 2, c);
    gfx->drawLine(cx + r / 6 + d, cy + r / 2, cx - r / 8 + d, cy + r, c);
  }
}

// Sun behind cloud for the "few clouds" case, and the sun swapped for a moon
// at night -- a sun glyph at 3am is the kind of detail that makes a panel
// look like it is not paying attention.
static void gMoon(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  gfx->fillCircle(cx, cy, r * 60 / 100, c);
  gfx->fillCircle(cx + r * 32 / 100, cy - r * 28 / 100, r * 55 / 100, C_BG);
}

static void skyGlyph(int16_t cx, int16_t cy, int16_t r, uint16_t code, bool isDay) {
  uint16_t sun = rgb(255, 200, 60), cloud = rgb(190, 200, 210), wet = rgb(110, 180, 255);
  uint16_t snow = rgb(220, 240, 255), bolt = rgb(255, 220, 80);
  switch (skyOf(code)) {
    case Sky::Clear: isDay ? gSun(cx, cy, r, sun) : gMoon(cx, cy, r, cloud); break;
    case Sky::Few:
      isDay ? gSun(cx + r / 3, cy - r / 3, r * 65 / 100, sun) : gMoon(cx + r / 3, cy - r / 3, r * 65 / 100, cloud);
      gCloud(cx - r / 5, cy + r / 5, r * 80 / 100, cloud);
      break;
    case Sky::Cloud: gCloud(cx, cy, r, cloud); break;
    case Sky::Fog:
      gCloud(cx, cy - r / 5, r * 85 / 100, cloud);
      for (uint8_t i = 0; i < 3; i++) gfx->drawFastHLine(cx - r * 3 / 4, cy + r / 2 + i * (r / 4), r * 3 / 2, cloud);
      break;
    case Sky::Drizzle: gCloud(cx, cy - r / 4, r * 85 / 100, cloud); gDrops(cx, cy + r / 2, r, wet, 2); break;
    case Sky::Rain: gCloud(cx, cy - r / 4, r * 85 / 100, cloud); gDrops(cx, cy + r / 2, r, wet, 3); break;
    case Sky::Showers: gCloud(cx, cy - r / 4, r * 85 / 100, cloud); gDrops(cx, cy + r / 2, r, wet, 2); break;
    case Sky::Snow: gCloud(cx, cy - r / 4, r * 85 / 100, cloud); gFlakes(cx, cy + r / 2, r, snow, 3); break;
    case Sky::Storm: gCloud(cx, cy - r / 4, r * 85 / 100, cloud); gBolt(cx, cy + r / 3, r * 70 / 100, bolt); break;
  }
}

// ── the fetch ────────────────────────────────────────────────────────────
// One request for everything. The filter matters more than usual here: the
// full reply carries hourly arrays 168 entries long and we want 24, so
// without it ArduinoJson allocates several times what the data needs.
static const size_t BUF_CAP = 48 * 1024;
static uint8_t *buf = nullptr;

static void buildUrl(char *out, size_t n) {
  snprintf(out, n,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,"
           "wind_speed_10m,wind_direction_10m,wind_gusts_10m"
           "&hourly=temperature_2m,precipitation_probability,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
           "&timezone=auto&forecast_days=7&forecast_hours=24%s",
           lat, lon, metric ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");
}

static bool fetchOnce() {
  char url[512];
  buildUrl(url, sizeof url);
  NetworkClientSecure client;
  client.setInsecure();  // ponytail: no key and no secret in the reply; a CA bundle costs 6KB of heap for nothing
  int len = fetchBytes(client, url, UA, buf, BUF_CAP, "weather");
  if (len <= 0) {
    snprintf(fetchErr, sizeof fetchErr, "could not reach open-meteo");
    return false;
  }

  JsonDocument filter;
  for (const char *k : {"temperature_2m", "relative_humidity_2m", "apparent_temperature", "is_day", "weather_code",
                        "wind_speed_10m", "wind_direction_10m", "wind_gusts_10m"})
    filter["current"][k] = true;
  for (const char *k : {"time", "temperature_2m", "precipitation_probability", "weather_code"})
    filter["hourly"][k] = true;
  for (const char *k : {"time", "weather_code", "temperature_2m_max", "temperature_2m_min",
                        "precipitation_probability_max", "sunrise", "sunset"})
    filter["daily"][k] = true;

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, buf, len, DeserializationOption::Filter(filter));
  if (e) {
    snprintf(fetchErr, sizeof fetchErr, "reply was not JSON (%s)", e.c_str());
    return false;
  }

  xSemaphoreTake(mux, portMAX_DELAY);
  JsonObject c = doc["current"];
  now_.temp = c["temperature_2m"] | 0.0f;
  now_.feels = c["apparent_temperature"] | 0.0f;
  now_.humidity = c["relative_humidity_2m"] | 0;
  now_.code = c["weather_code"] | 0;
  now_.isDay = (c["is_day"] | 1) != 0;
  now_.wind = c["wind_speed_10m"] | 0.0f;
  now_.gust = c["wind_gusts_10m"] | 0.0f;
  now_.windDir = (uint8_t)(((c["wind_direction_10m"] | 0) + 22) / 45 % 8);
  now_.valid = true;

  JsonArray ht = doc["hourly"]["temperature_2m"], hp = doc["hourly"]["precipitation_probability"],
            hc = doc["hourly"]["weather_code"];
  nHours = 0;
  for (uint8_t i = 0; i < N_HOURS && i < ht.size(); i++) {
    hours[i].temp = (int8_t)lroundf(ht[i] | 0.0f);
    hours[i].pop = hp[i] | 0;
    hours[i].code = hc[i] | 0;
    nHours++;
  }

  JsonArray dt = doc["daily"]["time"], dmax = doc["daily"]["temperature_2m_max"],
            dmin = doc["daily"]["temperature_2m_min"], dp = doc["daily"]["precipitation_probability_max"],
            dc = doc["daily"]["weather_code"];
  nDays = 0;
  for (uint8_t i = 0; i < N_DAYS && i < dmax.size(); i++) {
    days[i].hi = (int8_t)lroundf(dmax[i] | 0.0f);
    days[i].lo = (int8_t)lroundf(dmin[i] | 0.0f);
    days[i].pop = dp[i] | 0;
    days[i].code = dc[i] | 0;
    // "2026-09-19" -> a weekday name, worked out rather than fetched.
    const char *ds = dt[i] | "";
    struct tm t = {};
    if (strlen(ds) >= 10 && sscanf(ds, "%4d-%2d-%2d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
      t.tm_year -= 1900;
      t.tm_mon -= 1;
      t.tm_hour = 12;
      mktime(&t);
      strftime(days[i].name, sizeof days[i].name, "%a", &t);
    } else {
      snprintf(days[i].name, sizeof days[i].name, "%u", i);
    }
    nDays++;
  }
  const char *sr = doc["daily"]["sunrise"][0] | "", *ss = doc["daily"]["sunset"][0] | "";
  if (strlen(sr) >= 16) snprintf(sunrise, sizeof sunrise, "%.5s", sr + 11);
  if (strlen(ss) >= 16) snprintf(sunset, sizeof sunset, "%.5s", ss + 11);
  xSemaphoreGive(mux);

  fetchErr[0] = '\0';
  lastOk = millis();
  Serial.printf("weather: %s %.1f%s %s, %u hours, %u days\n", place, now_.temp, metric ? "C" : "F",
                skyWord(now_.code), nHours, nDays);
  return true;
}

static void fetchTask(void *) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (!lastOk || millis() - lastOk > REFRESH_MS)) fetchOnce();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ── the pages ────────────────────────────────────────────────────────────
enum class View : uint8_t { Now, Hours, Week };
static View view = View::Now;
static const char *const TABS[] = {"NOW", "HOURS", "WEEK"};
static int16_t tabX[3], tabW[3];
static bool confirmOpen = false;

static const char *DIRS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static char degC() { return metric ? 'C' : 'F'; }

static void drawHeader() {
  gfx->fillRect(0, 0, LCD_W, UI_Y_ROW0 - 1, C_BG);
  int16_t x = 20;
  for (uint8_t i = 0; i < 3; i++) {
    tabX[i] = x;
    tabW[i] = textWidth(2, TABS[i]) + 28;
    bool on = (uint8_t)view == i;
    if (on) gfx->fillRoundRect(x, 6, tabW[i], 28, 14, mix(C_BG, C_GOLD, 30));
    textAt(x + 14, 10, 2, on ? C_FG : C_MUTED, TABS[i]);
    x += tabW[i] + 8;
  }
  char r[40];
  if (lastOk) snprintf(r, sizeof r, "%s  %lum ago", place, (unsigned long)((millis() - lastOk) / 60000));
  else snprintf(r, sizeof r, "%s  no data yet", place);
  textAt(LCD_W - 20 - textWidth(1, r), 14, 1, C_MUTED, r);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, C_RULE);
}

static void drawNow() {
  gfx->fillScreen(C_BG);
  drawHeader();
  if (!now_.valid) {
    fieldCentre(LCD_W / 2, 220, 60, 2, C_MUTED, fetchErr[0] ? fetchErr : "waiting for the first forecast");
    drawHint("open-meteo, keyless");
    return;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  Now n = now_;
  xSemaphoreGive(mux);

  skyGlyph(140, 170, 78, n.code, n.isDay);

  char t[24];
  snprintf(t, sizeof t, "%d", (int)lroundf(n.temp));
  textAt(250, 96, 5, C_FG, t);
  int16_t dx = 250 + textWidth(5, t) + 10;
  snprintf(t, sizeof t, "%c", degC());
  textAt(dx, 104, 3, C_GOLD, t);
  textAt(250, 190, 2, C_MUTED, skyWord(n.code));
  snprintf(t, sizeof t, "feels like %d%c", (int)lroundf(n.feels), degC());
  textAt(250, 222, 1, C_DIM, t);

  // The numbers that change the day: a column of label-and-value pairs, not
  // a paragraph. Reading a panel from the kitchen is scanning, not reading.
  struct Pair { const char *k; char v[20]; } rows[5];
  uint8_t n2 = 0;
  snprintf(rows[n2].v, 20, "%d%%", n.humidity); rows[n2++].k = "humidity";
  snprintf(rows[n2].v, 20, "%s %d%s", DIRS[n.windDir], (int)lroundf(n.wind), metric ? "km/h" : "mph");
  rows[n2++].k = "wind";
  snprintf(rows[n2].v, 20, "%d%s", (int)lroundf(n.gust), metric ? "km/h" : "mph"); rows[n2++].k = "gusts";
  snprintf(rows[n2].v, 20, "%s", sunrise[0] ? sunrise : "--"); rows[n2++].k = "sunrise";
  snprintf(rows[n2].v, 20, "%s", sunset[0] ? sunset : "--"); rows[n2++].k = "sunset";
  for (uint8_t i = 0; i < n2; i++) {
    int16_t y = 90 + i * 40;
    textAt(520, y, 1, C_DIM, rows[i].k);
    textAt(520, y + 16, 2, C_FG, rows[i].v);
  }
  drawHint("swipe for the hours and the week");
}

static void drawHours() {
  gfx->fillScreen(C_BG);
  drawHeader();
  if (!nHours) {
    fieldCentre(LCD_W / 2, 220, 60, 2, C_MUTED, "waiting for the first forecast");
    return;
  }
  // Twelve columns across, two rows of twelve for the full 24.
  const uint8_t COLS = 12;
  const int16_t w = (LCD_W - 40) / COLS;
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);
  for (uint8_t i = 0; i < nHours; i++) {
    uint8_t col = i % COLS, row = i / COLS;
    int16_t x = 20 + col * w, y = 52 + row * 196;
    char l[8];
    int hh = haveTime ? (t.tm_hour + i) % 24 : i;
    snprintf(l, sizeof l, "%02d", hh);
    textAt(x + (w - textWidth(1, l)) / 2, y, 1, hh == 0 ? C_GOLD : C_MUTED, l);
    skyGlyph(x + w / 2, y + 44, 18, hours[i].code, hh >= 7 && hh < 19);
    snprintf(l, sizeof l, "%d", hours[i].temp);
    textAt(x + (w - textWidth(2, l)) / 2, y + 74, 2, C_FG, l);
    // Rain chance as a bar, because a column of percentages is unreadable
    // at this width and the shape of the day is the point.
    int16_t bh = hours[i].pop * 52 / 100;
    gfx->drawRect(x + w / 2 - 7, y + 104, 14, 52, C_RULE);
    if (bh) gfx->fillRect(x + w / 2 - 6, y + 104 + 52 - bh, 12, bh, rgb(110, 180, 255));
    if (hours[i].pop >= 10) {
      snprintf(l, sizeof l, "%u", hours[i].pop);
      textAt(x + (w - textWidth(1, l)) / 2, y + 160, 1, C_DIM, l);
    }
  }
  drawHint("the next 24 hours: temperature, and the chance of rain as a bar");
}

static void drawWeek() {
  gfx->fillScreen(C_BG);
  drawHeader();
  if (!nDays) {
    fieldCentre(LCD_W / 2, 220, 60, 2, C_MUTED, "waiting for the first forecast");
    return;
  }
  // One bar per day, all on the week's own scale, so the shape of the week
  // is visible rather than seven unrelated pairs of numbers.
  int8_t wkLo = 127, wkHi = -128;
  for (uint8_t i = 0; i < nDays; i++) {
    if (days[i].lo < wkLo) wkLo = days[i].lo;
    if (days[i].hi > wkHi) wkHi = days[i].hi;
  }
  if (wkHi <= wkLo) wkHi = wkLo + 1;
  const int16_t BX = 250, BW = LCD_W - BX - 150;
  for (uint8_t i = 0; i < nDays; i++) {
    int16_t y = 56 + i * 54;
    textAt(24, y + 8, 2, i == 0 ? C_FG : C_MUTED, i == 0 ? "Today" : days[i].name);
    skyGlyph(150, y + 20, 20, days[i].code, true);
    char l[8];
    snprintf(l, sizeof l, "%d", days[i].lo);
    textAt(BX - 10 - textWidth(2, l), y + 8, 2, C_DIM, l);
    int16_t x0 = BX + (days[i].lo - wkLo) * BW / (wkHi - wkLo);
    int16_t x1 = BX + (days[i].hi - wkLo) * BW / (wkHi - wkLo);
    gfx->fillRoundRect(BX, y + 16, BW, 8, 4, mix(C_BG, C_RULE, 80));
    gfx->fillRoundRect(x0, y + 14, (x1 - x0) > 8 ? (x1 - x0) : 8, 12, 6, C_GOLD);
    snprintf(l, sizeof l, "%d", days[i].hi);
    textAt(BX + BW + 10, y + 8, 2, C_FG, l);
    if (days[i].pop >= 10) {
      snprintf(l, sizeof l, "%u%%", days[i].pop);
      textAt(LCD_W - 24 - textWidth(1, l), y + 12, 1, rgb(110, 180, 255), l);
    }
  }
  drawHint("high and low against the week's own range");
}

static void draw() {
  switch (view) {
    case View::Now: drawNow(); break;
    case View::Hours: drawHours(); break;
    case View::Week: drawWeek(); break;
  }
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
  } else if (millis() - joinStarted > 20000 &&
             (lastRetry == 0 || millis() - lastRetry > netRetryDelay(wifiRetries, 20000))) {
    lastRetry = millis();
    Serial.printf("wifi retry #%u\n", ++wifiRetries);
    WiFi.disconnect();
    netJoinStart();
    scanning = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bool xp = boardBegin();
  // The base: BOOT held goes back to it, a cold boot with no handoff goes
  // back to it, and otherwise we tell it the slot is ours. Must run before
  // the panel -- GPIO0 is BOOT and also the panel green bit 0.
  baseAppBoot("weather");

  mux = xSemaphoreCreateMutex();
  buf = (uint8_t *)heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM);

  if (cfgLoad()) {
    lat = cfgFloat("latitude", lat, -90, 90);
    lon = cfgFloat("longitude", lon, -180, 180);
    cfgStr("place", place, sizeof place);
    metric = cfgInt("metric", metric ? 1 : 0, 0, 1) != 0;
    cfgRelease();
  }

  // Everything the board knows rather than this app: network, theme,
  // orientation, timezone. Home set them once.
  baseLoad();
  snprintf(wifiSsid, sizeof wifiSsid, "%s", baseCfg.ssid);
  snprintf(wifiPass, sizeof wifiPass, "%s", baseCfg.pass);
  snprintf(tzString, sizeof tzString, "%s", baseCfg.tz);
  if (!wifiSsid[0]) Serial.println("no network: Home runs setup -- hold BOOT at power-on to get there");
  setenv("TZ", tzString, 1);
  tzset();
  sTheme = baseTheme(N_THEMES);
  applyTheme();

  gfx = boardDisplay();
  bool panelOk = gfx->begin();
  boardSetRotation(baseCfg.rotation);
  gfx->setTextWrap(false);
  Serial.printf("weather: expander %s, panel %s, %s at %.3f,%.3f, %s\n", xp ? "ok" : "NO ACK",
                panelOk ? "ok" : "FAILED", place, lat, lon, metric ? "metric" : "imperial");

  draw();
  backlight(255);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  netTune();
  netJoinStart();
  joinStarted = millis();
  xTaskCreatePinnedToCore(fetchTask, "weather", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  netTick();

  static uint32_t lastDraw = 0, seenOk = 0;
  if (lastOk != seenOk) {  // a fetch landed: redraw whatever is on screen
    seenOk = lastOk;
    draw();
  } else if (millis() - lastDraw > 30000) {  // the "Nm ago" in the header ages
    lastDraw = millis();
    if (view == View::Now) drawHeader();
  }

  // Night: the panel dark inside Home's window, a touch lights it a minute.
  static bool dark = false;
  static uint32_t litAt = 0;
  struct tm t;
  bool night = baseCfg.sleepMode == 1 && getLocalTime(&t, 0) && inNight(t.tm_hour, baseCfg.sleepFrom, baseCfg.sleepTo);
  if (touchHeld) litAt = millis();
  bool wantDark = night && millis() - litAt > 60000;
  if (wantDark != dark) {
    dark = wantDark;
    backlight(dark ? 0 : 255);
  }

  int16_t x, y, dy;
  Gesture g = pollGesture(&x, &y, &dy);
  if (g == Gesture::None) return;
  bool tap = g == Gesture::Tap || g == Gesture::TapUp;

  if (confirmOpen) {
    if (tap) {
      if (hitPill(x, y)) goToSleep(0);
      confirmOpen = false;
      sheetClose();
      draw();
    }
    return;
  }
  if (g == Gesture::LongPress && y < UI_Y_ROW0) {  // hold the header: shut down
    confirmOpen = true;
    drawShutdownSheet();
    return;
  }
  if (tap && y < UI_Y_ROW0) {
    for (uint8_t i = 0; i < 3; i++)
      if (x >= tabX[i] && x < tabX[i] + tabW[i] && (uint8_t)view != i) {
        view = (View)i;
        draw();
        return;
      }
    return;
  }
  if (g == Gesture::SwipeLeft && (uint8_t)view < 2) {
    view = (View)((uint8_t)view + 1);
    draw();
  } else if (g == Gesture::SwipeRight && (uint8_t)view > 0) {
    view = (View)((uint8_t)view - 1);
    draw();
  }
}
