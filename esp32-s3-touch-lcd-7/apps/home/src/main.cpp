// Home -- the base app. Owns the board, the shared settings, and which app
// boots. See the README next to this file for the design.
//
// It lives in `factory` and stays there. Choosing an app sets the boot
// partition to ota_0 and restarts; the app's Home control sets it back. Two
// seconds each way, no network involved.
#include <baseos.h>
#include <board.h>
#include <helv.h>
#include <LittleFS.h>
#include <sleep.h>
#include <ui.h>
#include <appstore.h>
#include <update.h>
#include <wifisetup.h>

// Every app in the repo, built or not. Showing the unbuilt ones is the
// point: this board is mostly a list of things worth making, and hiding
// fifteen of them behind a directory listing made the picker look like the
// whole story when it is four apps out of twenty.
//
// `accent` is why the grid is not a page of grey boxes -- with no icons and
// no screenshots at tile size, colour is what makes one tile findable at a
// glance. Ready means there is code and an env; Idea means a README and a
// preview and nothing else yet.
enum class Status : uint8_t { Ready, Idea };
struct AppEntry {
  const char *id, *name, *blurb;
  uint16_t accent;
  Status status;
  const char *about;
};

// Ready first, then ideas, so the useful ones need no scrolling.
static const AppEntry APPS[] = {
    {"ticker", "Ticker Tape", "stocks, indices, FX, coins", rgb(34, 208, 90), Status::Ready,
     "A scrolling tape of your watchlist: live prices with a sparkline per row, tap one for a full chart "
     "with candles and volume, plus news with pictures and a heatmap of the day. Keyless, from Yahoo. "
     "Sleeps overnight and wakes with last night's prices already on screen."},
    {"sports", "Game Day", "scores across eleven leagues", rgb(240, 138, 40), Status::Ready,
     "Scores and schedules across eleven leagues, team logos on every row, your favourites pinned to the "
     "top. Tabs are sports rather than leagues, so all seven soccer competitions sit together. Keyless, "
     "from ESPN's own scoreboard endpoint."},
    {"unifi", "UniFi", "devices, clients, cameras", rgb(64, 156, 255), Status::Ready,
     "Your UniFi console at a glance: gateway and switch health, who is connected, and live snapshots "
     "from the Protect cameras. Needs an API key from the Integrations page of the Network app, typed "
     "once on the board."},
    {"social", "Social", "your own numbers", rgb(180, 120, 255), Status::Ready,
     "A ticker of your own figures rather than the market's -- followers, stars, downloads, subscribers "
     "-- from Bluesky, Mastodon, GitHub, npm and YouTube, all keyless. Keeps thirty days of history so "
     "the arrows mean something. Built as a demo and parked."},
    {"weather", "Weather", "now, the hours, the week", rgb(90, 190, 255), Status::Ready,
     "Now, the next 24 hours and the week, big enough to read across the room. Temperature in the tall "
     "digits, a drawn condition glyph, wind and gusts, humidity, sunrise and sunset. Hours is a strip of "
     "24 with the chance of rain as a bar; Week puts every day on the week's own temperature scale. "
     "Open-Meteo, keyless, one request every ten minutes."},

    {"flights", "Flights", "aircraft overhead", rgb(120, 200, 180), Status::Idea,
     "Every aircraft above the house on a map of the sky, from OpenSky. Keyless. Callsign, altitude, "
     "heading and climb, with the track of where each one has been while you watched."},
    {"transit", "Transit", "the stops near the house", rgb(250, 180, 60), Status::Idea,
     "A departure board for the stops you actually use, from GTFS-realtime. Minutes to the next few "
     "vehicles per route, delays called out, walking time subtracted so the number is when to leave."},
    {"calendar", "Calendar", "today and the week", rgb(230, 110, 140), Status::Idea,
     "Today and the week from any iCal URL, with the next thing counting down. No account: a secret "
     "calendar address from Google, Fastmail or iCloud is a URL you paste once."},
    {"space", "Space", "the ISS and this week's launches", rgb(140, 140, 240), Status::Idea,
     "Where the ISS is now and when it next passes over, the picture of the day, and this week's "
     "launches with a countdown to the next window. All keyless."},
    {"earthquakes", "Earthquakes", "the last day, from USGS", rgb(220, 120, 80), Status::Idea,
     "The last day of quakes on a world map, sized and coloured by magnitude, with the significant ones "
     "listed. USGS publishes it keyless as GeoJSON and updates every minute."},
    {"solar", "Solar", "the house's power flow", rgb(250, 210, 70), Status::Idea,
     "Generation, consumption, grid and battery as a live flow diagram, plus today's totals and what the "
     "sun did this week. One key from whichever inverter cloud the panels report to."},
    {"pihole", "Pi-hole", "queries and blocks today", rgb(180, 70, 70), Status::Idea,
     "What the DNS blocker is doing: queries and blocks today, the block rate as a ring, the top blocked "
     "domains and busiest clients, and a pause button. Could equally be a tab in UniFi."},
    {"printer", "Printer", "the print in progress", rgb(160, 200, 90), Status::Idea,
     "The job on the 3D printer: progress, layer, time left, nozzle and bed temperatures, and the camera "
     "if there is one. OctoPrint, Moonraker or Bambu over MQTT."},
    {"media", "Media", "what is playing", rgb(200, 100, 200), Status::Idea,
     "What is playing on Plex, Jellyfin or Sonos: artwork, title, progress, and who is watching. Big "
     "enough to read from the sofa, with transport controls."},
    {"ci-status", "CI Status", "the builds, red or green", rgb(120, 220, 160), Status::Idea,
     "A wall of your repositories' build status, red or green at a glance, with the failing job named "
     "and how long it has been broken. GitHub Actions and GitLab CI."},
    {"wall-dashboard", "Wall Dashboard", "calendar, weather, transit", rgb(100, 170, 230), Status::Idea,
     "The flagship use for a 7in panel: one screen with the calendar, the weather and the next departures "
     "together, designed to be read from across the room and never touched."},
    {"ha-panel", "Home Assistant", "a control surface", rgb(70, 190, 210), Status::Idea,
     "A full-size Home Assistant control surface: lights, scenes, climate and sensors as tiles you can "
     "actually hit with a thumb, over the REST API or a websocket."},
    {"obd2-gauge", "OBD-II Gauge", "live car telemetry", rgb(230, 90, 90), Status::Idea,
     "Live telemetry from a car over a BLE OBD-II dongle: boost, coolant, oil, RPM and a peak hold. "
     "Standard PIDs, so it works on anything sold since the mid-2000s. See cars/ for the notes."},
    {"modbus-readout", "Modbus", "RS485 sensors", rgb(150, 150, 160), Status::Idea,
     "Poll RS485/Modbus sensors through the board's own header and chart them. The sleeper feature of "
     "this panel: no other board here talks to industrial kit without add-on hardware."},
};
static const uint8_t N_APPS = sizeof APPS / sizeof APPS[0];

enum class View : uint8_t { Splash, Legal, Picker, Detail, Settings, Themes, Sleep, Info, About, Keyboard, Setup };
static View view = View::Splash;
static bool legalTicked = false;
static uint8_t sel = 0;  // which app the detail page is showing
static char slotApp[24] = "";
static bool slotFilled = false;
static bool confirmOpen = false;
static char urlBuf[200] = "";
static uint16_t *preview = nullptr;  // PREVIEW_W x PREVIEW_H, in PSRAM
static const int16_t PREVIEW_W = 400, PREVIEW_H = 240;

static bool inSlot(uint8_t i) { return slotFilled && strcmp(APPS[i].id, slotApp) == 0; }

// ── the picker ───────────────────────────────────────────────────────────
// Three columns and a drag to scroll. Nineteen apps do not fit a screen, and
// the alternative -- hiding the unbuilt ones -- is what made the board look
// like four apps when it is a list of twenty things worth making.
static const uint8_t COLS = 3;
static const int16_t TILE_W = (LCD_W - 40 - 16 * (COLS - 1)) / COLS, TILE_H = 104, TILE_GAP = 14;
static const int16_t GRID_Y = UI_Y_ROW0 + 10, GRID_BOT = UI_Y_HINT - 10;
static int16_t scrollY = 0;

static int16_t gridHeight() {
  uint8_t rows = (N_APPS + COLS - 1) / COLS;
  return rows * (TILE_H + TILE_GAP) - TILE_GAP;
}
static int16_t maxScroll() {
  int16_t over = gridHeight() - (GRID_BOT - GRID_Y);
  return over > 0 ? over : 0;
}
static void tileAt(uint8_t i, int16_t *x, int16_t *y) {
  *x = 20 + (i % COLS) * (TILE_W + 16);
  *y = GRID_Y + (i / COLS) * (TILE_H + TILE_GAP) - scrollY;
}

static void appTile(uint8_t i) {
  int16_t x, y;
  tileAt(i, &x, &y);
  if (y + TILE_H < GRID_Y || y > GRID_BOT) return;  // scrolled out of view
  const AppEntry &a = APPS[i];
  bool loaded = inSlot(i);
  bool ready = a.status == Status::Ready;

  // An idea gets the same colour at a quarter of the strength. Greying them
  // out entirely would say "broken"; this says "not yet".
  uint8_t wash = loaded ? 22 : ready ? 12 : 5;
  gfx->fillRoundRect(x, y, TILE_W, TILE_H, 12, mix(C_BG, a.accent, wash));
  gfx->drawRoundRect(x, y, TILE_W, TILE_H, 12, loaded ? a.accent : mix(C_RULE, a.accent, ready ? 40 : 15));
  gfx->fillRoundRect(x, y, 6, TILE_H, 3, ready ? a.accent : mix(C_BG, a.accent, 45));
  gfx->fillRect(x + 3, y, 3, TILE_H, mix(C_BG, a.accent, wash));

  field(x + 20, y + 14, (TILE_W - 32) / GW(2), 2, ready ? C_FG : C_MUTED, a.name);
  field(x + 20, y + 44, (TILE_W - 32) / GW(1), 1, ready ? towardsWhite(a.accent, 35) : C_DIM, a.blurb);

  const char *tag = loaded ? "LOADED" : ready ? "ready" : "idea";
  uint16_t tagFg = loaded ? RGB565_BLACK : ready ? towardsWhite(a.accent, 30) : C_DIM;
  int16_t tw = textWidth(1, tag) + 14;
  if (loaded) gfx->fillRoundRect(x + 20, y + TILE_H - 30, tw, 20, 10, a.accent);
  textAt(x + (loaded ? 27 : 20), y + TILE_H - 26, 1, tagFg, tag);
}

static void drawPicker() {
  gfx->fillScreen(C_BG);
  for (uint8_t i = 0; i < N_APPS; i++) appTile(i);
  // The header last: tiles scrolled up must not paint over it.
  gfx->fillRect(0, 0, LCD_W, UI_Y_ROW0 - 1, C_BG);
  char t[32];
  snprintf(t, sizeof t, "Apps");
  textAt(20, 10, 2, C_FG, t);
  snprintf(t, sizeof t, "%u here, %u ready", N_APPS, 5u);
  textAt(20 + textWidth(2, "Apps") + 16, 14, 1, C_MUTED, t);
  settingsIcon(LCD_W - 52, C_MUTED);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, C_RULE);

  // A scrollbar, because otherwise nothing says the list continues.
  if (maxScroll()) {
    int16_t track = GRID_BOT - GRID_Y;
    int16_t knob = track * track / gridHeight();
    int16_t at = GRID_Y + (track - knob) * scrollY / maxScroll();
    gfx->fillRoundRect(LCD_W - 8, at, 4, knob, 2, C_RULE);
  }
  drawHint("drag to scroll, tap an app        settings, top right");
}

// ── the detail page ──────────────────────────────────────────────────────
static const int16_t PV_X = LCD_W - PREVIEW_W - 24, PV_Y = 56;
static int16_t btnX, btnY, btnW = 260, btnH = 52;

// /preview/<id>.565, made by tools/make-previews.py. Absent is normal: two
// apps have no preview.svg to render from, and a board that has never had
// `push-config home` run has none at all.
static bool previewOk = false;
static bool loadPreview(const char *id) {
  previewOk = false;
  if (!preview) preview = (uint16_t *)heap_caps_malloc((size_t)PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
  if (!preview) return false;
  char path[40];
  snprintf(path, sizeof path, "/preview/%s.565", id);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t want = (size_t)PREVIEW_W * PREVIEW_H * 2;
  previewOk = f.size() == want && f.read((uint8_t *)preview, want) == want;
  f.close();
  return previewOk;
}

// Drawn from appstore's callback, so the installer never has to know what
// the screen looks like. Only the bar is repainted -- redrawing the page a
// hundred times during a download would make the download the slow part.
static void installProgress(uint8_t pct, const char *stage) {
  const AppEntry &a = APPS[sel];
  int16_t x = 24, y = UI_Y_HINT - 72, w = LCD_W / 2 - 48, h = 52;
  gfx->fillRoundRect(x, y, w, h, 26, mix(C_BG, a.accent, 12));
  gfx->fillRoundRect(x, y, (int16_t)((long)w * pct / 100), h, 26, a.accent);
  char t[40];
  snprintf(t, sizeof t, "%s  %u%%", stage, pct);
  textAt(x + (w - textWidth(2, t)) / 2, y + (h - FACES[1].cap) / 2, 2, C_FG, t);
}

static int16_t btnYTop() { return UI_Y_HINT - 72; }

static void drawDetail() {
  const AppEntry &a = APPS[sel];
  bool loaded = inSlot(sel);
  gfx->fillScreen(C_BG);

  // The header carries the app's colour so the page is obviously still about
  // the thing you tapped.
  gfx->fillRect(0, 0, LCD_W, UI_Y_ROW0 - 1, mix(C_BG, a.accent, 18));
  backMark(14, 22, towardsWhite(a.accent, 40));
  textAt(36, 10, 2, C_FG, a.name);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, a.accent);

  // Not on the board? Ask the app source for it once. Costs a second the
  // first time a detail page is opened and nothing thereafter.
  if (!loadPreview(a.id) && appstore::haveManifest) {
    const appstore::Entry *en = appstore::find(a.id);
    char path[40];
    snprintf(path, sizeof path, "/preview/%s.565", a.id);
    if (en && appstore::fetchPreview(*en, path)) loadPreview(a.id);
  }
  if (previewOk) {
    gfx->draw16bitRGBBitmap(PV_X, PV_Y, preview, PREVIEW_W, PREVIEW_H);
    gfx->drawRect(PV_X - 1, PV_Y - 1, PREVIEW_W + 2, PREVIEW_H + 2, mix(C_RULE, a.accent, 40));
  } else {
    gfx->fillRoundRect(PV_X, PV_Y, PREVIEW_W, PREVIEW_H, 10, mix(C_BG, a.accent, 8));
    gfx->drawRoundRect(PV_X, PV_Y, PREVIEW_W, PREVIEW_H, 10, mix(C_RULE, a.accent, 30));
    const char *m = "no preview on the board";
    textAt(PV_X + (PREVIEW_W - textWidth(1, m)) / 2, PV_Y + PREVIEW_H / 2 - 20, 1, C_MUTED, m);
    const char *m2 = "python3 tools/make-previews.py";
    textAt(PV_X + (PREVIEW_W - textWidth(1, m2)) / 2, PV_Y + PREVIEW_H / 2 + 4, 1, C_DIM, m2);
  }

  // The about text. Wrapped by measuring, but DRAWN with field(), which
  // truncates to a character budget -- so a line that measures wrong cannot
  // paint over the preview beside it. It did: textWidth uses real
  // proportional metrics and the earlier version only checked them at
  // spaces, so a long line drew straight through the screenshot.
  //
  // The budget is deliberately pessimistic: GW(1) is the widest glyph, so
  // the count is what fits in the worst case rather than the average.
  const int16_t tx = 24, tw = PV_X - tx - 24;
  const uint8_t budget = tw / GW(1);
  int16_t ty = PV_Y + 4;
  const char *p = a.about;
  char line[96];
  while (*p && ty < btnYTop() - 12) {
    uint16_t n = 0, lastSpace = 0;
    while (p[n] && n < sizeof line - 1) {
      line[n] = '\0';
      if (p[n] == ' ') {
        if (textWidth(1, line) > tw) break;
        lastSpace = n;
      }
      line[n] = p[n];
      n++;
    }
    line[n] = '\0';
    if (p[n] && lastSpace) n = lastSpace;      // step back to the last space
    else if (p[n] && !lastSpace) n = budget;   // one unbroken word: cut it, never loop forever
    if (!n) break;                             // nothing consumed: stop rather than spin
    line[n] = '\0';
    field(tx, ty, budget, 1, C_MUTED, line);
    ty += 22;
    p += n;
    while (*p == ' ') p++;
  }

  btnX = tx;
  btnY = btnYTop();
  bool ready = a.status == Status::Ready;
  const char *label = loaded ? "Run" : "Install";
  if (loaded) {
    gfx->fillRoundRect(btnX, btnY, btnW, btnH, 26, a.accent);
  } else if (ready) {
    gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, a.accent);
    gfx->fillRoundRect(btnX, btnY, btnW, btnH, 26, mix(C_BG, a.accent, 30));
    gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, a.accent);
  } else {
    // Disabled: an outline in the rule colour, no fill, grey text. It reads
    // as "not yet" rather than "broken", and it does not invite a tap.
    gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, C_RULE);
  }
  uint16_t fg = loaded ? RGB565_BLACK : ready ? towardsWhite(a.accent, 30) : C_DIM;
  textAt(btnX + (btnW - textWidth(2, label)) / 2, btnY + (btnH - FACES[1].cap) / 2, 2, fg, label);
  drawHint(loaded    ? "Run hands the screen over; hold BOOT at power-on to come back"
           : ready   ? "< apps"
                     : "not built yet -- a README and a preview, no code        < apps");
}

// ── settings ─────────────────────────────────────────────────────────────
// Words, not numbers. "0" told you nothing about which way up the panel was.
// 180 rather than "upside down": it is the rotation in degrees, it matches
// what the other two are implicitly saying, and it fits the row.
static const char *const ROT_NAMES[] = {"Landscape", "Portrait", "Landscape 180", "Portrait 180"};
static const char *const SLEEP_NAMES[] = {"never", "night"};

static void drawSettings() {
  pageHeader("SETTINGS");
  char v[48];
  settingRow(0, "Theme", THEMES[baseTheme(N_THEMES)].name);
  settingRow(1, "Orientation", ROT_NAMES[baseCfg.rotation & 3]);
  if (baseCfg.sleepMode) snprintf(v, sizeof v, "night, %02u:00-%02u:00", baseCfg.sleepFrom, baseCfg.sleepTo);
  else snprintf(v, sizeof v, "never");
  settingRow(2, "Sleep", v);
  settingRow(3, "Wi-Fi", baseCfg.ssid[0] ? baseCfg.ssid : "not set up");
  settingRow(4, "Shut down", "");
  settingRow(5, "App source", appstore::manifestUrl[0] ? appstore::manifestUrl : "not set");
  settingRow(6, "About this device", "");
  drawHint("< apps");
}

static void drawSleepPage() {
  pageHeader("SLEEP", "when the panel goes dark");
  settingRow(0, "Mode", SLEEP_NAMES[baseCfg.sleepMode]);
  char v[16];
  snprintf(v, sizeof v, "%02u:00", baseCfg.sleepFrom);
  settingRow(1, "From", v);
  snprintf(v, sizeof v, "%02u:00", baseCfg.sleepTo);
  settingRow(2, "To", v);
  drawHint("tap From or To to step the hour        < settings");
}

// About the DEVICE, which is what "Info" should always have meant. The old
// version showed "slot: unknown app", which answered a question nobody asked
// and phrased it as if something were wrong.
//
// Two columns. One ran sixteen lines at 24px from y=56 to y=416 and walked
// straight through the buttons at 386.
static int16_t splashBtnX, splashBtnY, splashBtnW = 200;

static void drawInfoPage() {
  pageHeader("ABOUT THIS DEVICE");
  char left[8][56], right[8][56];
  uint8_t nl = 0, nr = 0;
  uint64_t mac = ESP.getEfuseMac();
  const esp_partition_t *run = esp_ota_get_running_partition();

  snprintf(left[nl++], 56, "Home %s", FW_VERSION);
  snprintf(left[nl++], 56, "built %s", FW_BUILT);
  snprintf(left[nl++], 56, "updates: %s", update::stateWord());
  left[nl++][0] = '\0';
  snprintf(left[nl++], 56, "Waveshare ESP32-S3-Touch-LCD-7");
  snprintf(left[nl++], 56, "800x480 RGB, GT911 touch, CH422G");
  snprintf(left[nl++], 56, "%s rev%d, %dMHz, %d cores", ESP.getChipModel(), ESP.getChipRevision(),
           ESP.getCpuFreqMHz(), ESP.getChipCores());
  snprintf(left[nl++], 56, "flash %luMB, PSRAM %luMB", (unsigned long)(ESP.getFlashChipSize() / 1048576),
           (unsigned long)(ESP.getPsramSize() / 1048576));

  snprintf(right[nr++], 56, "MAC %02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)(mac >> 40), (uint8_t)(mac >> 32),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
  snprintf(right[nr++], 56, "Wi-Fi %s", baseCfg.ssid[0] ? baseCfg.ssid : "not set up");
  snprintf(right[nr++], 56, "2.4GHz only -- no 5GHz on this radio");
  right[nr++][0] = '\0';
  snprintf(right[nr++], 56, "heap %uk free, PSRAM %uk free", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  uint32_t up = millis() / 1000;
  snprintf(right[nr++], 56, "up %luh %02lum", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
  snprintf(right[nr++], 56, "Home in %s, %uk", run ? run->label : "?", (unsigned)(ESP.getSketchSize() / 1024));
  snprintf(right[nr++], 56, "app slot: %s",
           slotFilled ? (slotApp[0] ? slotApp : "an app that has not named itself") : "empty");

  const int16_t colW = (LCD_W - 72) / 2, RX = 24 + colW + 24;
  for (uint8_t i = 0; i < nl; i++) field(24, 56 + i * 24, colW / 8, 1, i == 0 ? C_FG : C_MUTED, left[i]);
  for (uint8_t i = 0; i < nr; i++) field(RX, 56 + i * 24, colW / 8, 1, C_MUTED, right[i]);

  btnW = 300;
  btnX = 24;
  btnY = UI_Y_HINT - 66;
  if (update::state == update::State::Available) {
    gfx->fillRoundRect(btnX, btnY, btnW, btnH, 26, C_GOLD);
    const char *t = "Update Home";
    textAt(btnX + (btnW - textWidth(2, t)) / 2, btnY + (btnH - FACES[1].cap) / 2, 2, RGB565_BLACK, t);
  } else {
    gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, C_RULE);
    const char *t = "Check for updates";
    textAt(btnX + (btnW - textWidth(2, t)) / 2, btnY + (btnH - FACES[1].cap) / 2, 2, C_MUTED, t);
  }
  splashBtnX = btnX + btnW + 16;
  splashBtnY = btnY;
  gfx->drawRoundRect(splashBtnX, splashBtnY, splashBtnW, btnH, 26, C_RULE);
  const char *t2 = "Splash";
  textAt(splashBtnX + (splashBtnW - textWidth(2, t2)) / 2, splashBtnY + (btnH - FACES[1].cap) / 2, 2, C_MUTED, t2);
  drawHint("< settings");
}

static void drawSplash() {
  gfx->fillScreen(C_BG);
  const char *name = "ESP32-S3 7\"";
  int16_t cy = LCD_H / 2;
  textAt((LCD_W - textWidth(5, name)) / 2, cy - 90, 5, C_FG, name);
  gfx->fillRoundRect((LCD_W - 200) / 2, cy + 18, 200, 4, 2, C_GOLD);
  char sub[64];
  snprintf(sub, sizeof sub, "%u apps  %s", N_APPS, slotFilled ? (slotApp[0] ? slotApp : "app loaded") : "no app loaded");
  textAt((LCD_W - textWidth(2, sub)) / 2, cy + 44, 2, C_MUTED, sub);
  snprintf(sub, sizeof sub, "Home %s", FW_VERSION);
  textAt((LCD_W - textWidth(1, sub)) / 2, cy + 80, 1, C_DIM, sub);
  // A Continue button rather than a timer. The old splash cleared itself
  // after 900ms, which is long enough to notice and not long enough to read
  // -- and it is also where the version is, which is the one thing someone
  // might actually be trying to read off it.
  if (view == View::Splash) {
    splashBtnW = 240;
    splashBtnX = (LCD_W - splashBtnW) / 2;
    splashBtnY = LCD_H - 96;
    gfx->fillRoundRect(splashBtnX, splashBtnY, splashBtnW, 52, 26, C_GOLD);
    const char *t = "Continue";
    textAt(splashBtnX + (splashBtnW - textWidth(2, t)) / 2, splashBtnY + (52 - FACES[1].cap) / 2, 2, RGB565_BLACK, t);
  }
}

// The terms. Deliberately short and in plain words: a wall of legalese on a
// touch panel is not read, and an unread agreement is worth nothing to
// either side. Bump LEGAL_VERSION in baseos.h if this changes materially.
static const char *const LEGAL[] = {
    "This is a hobbyist device. It is provided as-is, with no warranty of",
    "any kind, and no guarantee that it works or keeps working.",
    "",
    "Everything it shows comes from third-party services over the internet.",
    "That data may be wrong, stale, delayed, or simply missing, and those",
    "services can change or disappear without notice.",
    "",
    "Nothing here is advice. Prices are delayed and are not a basis for",
    "trading. Forecasts are not a basis for any decision about safety.",
    "Do not rely on this device for anything that matters.",
    "",
    "Your Wi-Fi password and any API keys are kept in this board's own",
    "flash and are sent nowhere else. Anyone holding the board can read",
    "them out of it, so use credentials you are willing to rotate.",
};
static const uint8_t N_LEGAL = sizeof LEGAL / sizeof LEGAL[0];
static int16_t tickX, tickY, tickS = 34;

static void drawLegal() {
  pageHeader("BEFORE YOU START");
  for (uint8_t i = 0; i < N_LEGAL; i++) field(28, 54 + i * 21, (LCD_W - 56) / 8, 1, C_MUTED, LEGAL[i]);

  tickX = 28;
  tickY = UI_Y_HINT - 78;
  gfx->drawRoundRect(tickX, tickY, tickS, tickS, 6, legalTicked ? C_GOOD : C_RULE);
  if (legalTicked) {
    gfx->fillRoundRect(tickX, tickY, tickS, tickS, 6, C_GOOD);
    checkMark(tickX + tickS - 6, tickY + tickS / 2, RGB565_BLACK);
  }
  textAt(tickX + tickS + 14, tickY + (tickS - FACES[1].cap) / 2, 2, C_FG, "I have read and accept this");

  // The button is dead until the box is ticked, and looks it. A greyed
  // control that still works is worse than either state on its own.
  btnW = 240;
  btnX = LCD_W - btnW - 28;
  btnY = tickY - 9;
  btnH = 52;
  if (legalTicked) {
    gfx->fillRoundRect(btnX, btnY, btnW, btnH, 26, C_GOLD);
    textAt(btnX + (btnW - textWidth(2, "Continue")) / 2, btnY + (btnH - FACES[1].cap) / 2, 2, RGB565_BLACK, "Continue");
  } else {
    gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, C_RULE);
    textAt(btnX + (btnW - textWidth(2, "Continue")) / 2, btnY + (btnH - FACES[1].cap) / 2, 2, C_DIM, "Continue");
  }
  drawHint("tap the box, then Continue");
}

static void draw() {
  switch (view) {
    case View::Splash: drawSplash(); break;
    case View::Legal: drawLegal(); break;
    case View::Picker: drawPicker(); break;
    case View::Detail: drawDetail(); break;
    case View::Settings: drawSettings(); break;
    case View::Themes: drawThemesPage(); break;
    case View::Sleep: drawSleepPage(); break;
    case View::Info: drawInfoPage(); break;
    case View::About: drawSplash(); break;
    case View::Keyboard: kbDraw("where Install fetches apps from"); break;
    case View::Setup: wifisetup::draw("waiting for you"); break;
  }
}

// Tap Install -> it installs. No laptop, no pio, no cable. Everything that
// can go wrong says so on the hint line in words, because the person reading
// it is standing in front of a panel, not a terminal.
static void doInstall() {
  const AppEntry &a = APPS[sel];
  if (WiFi.status() != WL_CONNECTED) {
    drawHint("no network yet -- Settings > Wi-Fi", C_WARN);
    return;
  }
  if (!appstore::manifestUrl[0]) {
    drawHint("no app source set -- Settings > App source", C_WARN);
    return;
  }
  installProgress(0, "looking");
  if (!appstore::haveManifest && !appstore::fetchManifest()) {
    drawDetail();
    drawHint(appstore::err, C_BAD);
    return;
  }
  const appstore::Entry *en = appstore::find(a.id);
  if (!en) {
    drawDetail();
    char m[80];
    snprintf(m, sizeof m, "%s is not in the app source", a.name);
    drawHint(m, C_WARN);
    return;
  }
  drawHint("installing replaces whatever app is loaded", C_MUTED);
  if (!appstore::install(*en, installProgress)) {
    slotFilled = baseSlotFilled();
    baseSlotApp(slotApp, sizeof slotApp);
    drawDetail();
    drawHint(appstore::err, C_BAD);
    return;
  }
  installProgress(100, "done");
  drawHint("installed -- starting it now", C_GOOD);
  delay(700);
  baseHandoff();
  bootIntoApp();
}

static bool hitBack(int16_t x, int16_t y, Gesture g) {
  return g == Gesture::SwipeLeft || (g != Gesture::Drag && x < 60 && y < UI_Y_ROW0);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bool xp = boardBegin();
  sleepSelfCheck();

  baseLoad();
  sTheme = baseTheme(N_THEMES);
  applyTheme();
  baseSlotApp(slotApp, sizeof slotApp);
  appstore::loadUrl();

  gfx = boardDisplay();
  bool panelOk = gfx->begin();
  boardSetRotation(baseCfg.rotation);
  gfx->setTextWrap(false);
  slotFilled = baseSlotFilled();
  if (!LittleFS.begin(false)) Serial.println("home: no LittleFS -- previews will be placeholders");

  Serial.printf("home: expander %s, panel %s, slot %s (%s), theme %s\n", xp ? "ok" : "NO ACK",
                panelOk ? "ok" : "FAILED", slotFilled ? "filled" : "EMPTY", slotApp[0] ? slotApp : "unclaimed",
                THEMES[baseTheme(N_THEMES)].name);

  view = View::Splash;
  draw();
  backlight(255);
}

// What follows the splash. Kept in one place because the order matters and
// is easy to get wrong spread across taps: terms first, since nothing should
// happen before someone has seen them, then Wi-Fi if there is none, then the
// picker.
static void afterSplash() {
  if (!legalAccepted()) {
    view = View::Legal;
    draw();
    return;
  }
  // A board with no network cannot do anything useful, so setup comes before
  // the picker rather than hiding behind a settings row.
  if (!baseCfg.ssid[0]) {
    view = View::Setup;
    wifisetup::begin();
    return;
  }
  view = View::Picker;
  draw();
}

void loop() {
  if (view == View::Setup) {  // the portal owns the board until it restarts
    wifisetup::tick();
    return;
  }
  int16_t x, y, dy;
  Gesture g = pollGesture(&x, &y, &dy);
  if (g == Gesture::None) return;
  // TapUp only, never Tap. The picker scrolls, and Tap fires 100ms after
  // touch-down while the finger is still there -- so a slow drag opened an
  // app before it had moved far enough to count as a scroll. See the note
  // above Gesture in ui.h; this is the third time the same bug has shipped.
  bool tap = g == Gesture::TapUp;

  if (confirmOpen) {  // the shutdown sheet owns every touch while it is up
    if (tap) {
      if (hitPill(x, y)) goToSleep(0);
      confirmOpen = false;
      sheetClose();
      draw();
    }
    return;
  }

  switch (view) {
    case View::Splash:
      if (tap && x >= splashBtnX && x < splashBtnX + splashBtnW && y >= splashBtnY && y < splashBtnY + 52)
        afterSplash();
      break;

    case View::Legal:
      if (tap && x >= tickX - 8 && x < tickX + tickS + 340 && y >= tickY - 10 && y < tickY + tickS + 10) {
        legalTicked = !legalTicked;  // the label is part of the target, as a checkbox should be
        drawLegal();
      } else if (tap && legalTicked && x >= btnX && x < btnX + btnW && y >= btnY && y < btnY + btnH) {
        legalAccept();
        if (!baseCfg.ssid[0]) {
          view = View::Setup;
          wifisetup::begin();
        } else {
          view = View::Picker;
          draw();
        }
      }
      break;

    case View::Picker:
      if (g == Gesture::Drag) {
        int16_t was = scrollY;
        scrollY -= dy;
        if (scrollY < 0) scrollY = 0;
        if (scrollY > maxScroll()) scrollY = maxScroll();
        if (scrollY != was) drawPicker();
      } else if (tap && x > LCD_W - 64 && y < UI_Y_ROW0) {
        view = View::Settings;
        draw();
      } else if (tap && y >= GRID_Y) {
        for (uint8_t i = 0; i < N_APPS; i++) {
          int16_t tx, ty;
          tileAt(i, &tx, &ty);
          if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H) {
            sel = i;
            view = View::Detail;
            draw();
            break;
          }
        }
      }
      break;

    case View::Detail:
      if (hitBack(x, y, g)) {
        view = View::Picker;
        draw();
      } else if (tap && x >= btnX && x < btnX + btnW && y >= btnY && y < btnY + btnH) {
        if (APPS[sel].status == Status::Idea) {
          drawHint("nothing to install yet -- this one is still just a README", C_MUTED);
        } else if (inSlot(sel)) {
          drawHint("starting...", C_GOOD);
          baseHandoff();  // or the app bounces straight back here
          bootIntoApp();
        } else {
          doInstall();
        }
      }
      break;

    case View::Settings:
      if (hitBack(x, y, g)) {
        view = View::Picker;
        draw();
      } else if (tap) {
        switch (hitSettingRow(y, 7)) {
          case 0: view = View::Themes; draw(); break;
          case 1:
            baseCfg.rotation = (baseCfg.rotation + 1) % 4;
            baseSave();
            drawSettings();
            drawHint("saved -- the new orientation applies on the next boot", C_WARN);
            break;
          case 2: view = View::Sleep; draw(); break;
          case 3:
            view = View::Setup;
            wifisetup::begin();
            break;
          case 4: confirmOpen = true; drawShutdownSheet(); break;
          case 5:
            // Typed on the panel with lib/ui's keyboard. A URL is the one
            // thing that has to be settable without a rebuild: the whole
            // point is that whoever is holding the board did not compile it.
            snprintf(urlBuf, sizeof urlBuf, "%s", appstore::manifestUrl);
            kbOpen("APP SOURCE", urlBuf, sizeof urlBuf);
            view = View::Keyboard;
            draw();
            break;
          case 6: view = View::Info; draw(); break;
          default: break;
        }
      }
      break;

    case View::Themes:
      if (hitBack(x, y, g)) {
        view = View::Settings;
        draw();
      } else if (tap) {
        int8_t t = hitTheme(x, y);
        if (t >= 0) {
          baseCfg.theme = (uint8_t)t;
          sTheme = baseTheme(N_THEMES);
          applyTheme();
          baseSave();
          draw();
        }
      }
      break;

    case View::Keyboard:
      if (tap) {
        switch (kbTap(x, y)) {
          case 1: kbField(); break;  // text changed: repaint the field only
          case 2:                    // Done
            appstore::saveUrl(urlBuf);
            appstore::haveManifest = false;  // re-read from wherever it now points
            view = View::Settings;
            draw();
            break;
          case 3:  // the back mark: leave the old URL alone
            view = View::Settings;
            draw();
            break;
          default: break;
        }
      }
      break;

    case View::About:  // the splash, shown on purpose; any touch dismisses it
      if (tap || g == Gesture::SwipeLeft) {
        view = View::Info;
        draw();
      }
      break;

    case View::Info:
      if (tap && x >= splashBtnX && x < splashBtnX + splashBtnW && y >= splashBtnY && y < splashBtnY + btnH) {
        view = View::About;
        draw();
      } else if (tap && x >= btnX && x < btnX + btnW && y >= btnY && y < btnY + btnH) {
        if (update::state == update::State::Available) {
          // Deliberately does not do it. Home runs from `factory` and nothing
          // can rewrite the partition it is running from -- the only route
          // writes the new Home into ota_0, boots it, copies it into factory
          // and boots back, wiping the installed app on the way. Say that,
          // rather than starting it and finding out.
          drawHint("updating Home would erase the app in the slot -- not wired up yet", C_WARN);
        } else {
          drawHint("checking...", C_MUTED);
          update::check();
          draw();
        }
      } else if (hitBack(x, y, g)) {
        view = View::Settings;
        draw();
      }
      break;

    case View::Sleep:
      if (hitBack(x, y, g)) {
        view = View::Settings;
        draw();
      } else if (tap) {
        int8_t r = hitSettingRow(y, 3);
        if (r == 0) baseCfg.sleepMode = !baseCfg.sleepMode;
        else if (r == 1) baseCfg.sleepFrom = (baseCfg.sleepFrom + 1) % 24;
        else if (r == 2) baseCfg.sleepTo = (baseCfg.sleepTo + 1) % 24;
        if (r >= 0) {
          baseSave();
          drawSleepPage();
        }
      }
      break;

    case View::Setup: break;  // handled above
  }
}
