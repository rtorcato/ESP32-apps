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
#include <wifisetup.h>

// Every app the board knows about. `accent` is the app's colour and the only
// reason the grid is not a page of grey boxes -- with no icons and no
// screenshots at tile size, colour is the thing that makes one tile findable
// at a glance from across a desk.
struct AppEntry {
  const char *id, *name, *blurb;
  uint16_t accent;
  const char *about;
};
static const AppEntry APPS[] = {
    {"ticker", "Ticker Tape", "stocks, indices, FX, coins", rgb(34, 208, 90),
     "A scrolling tape of your watchlist: live prices with a sparkline per row, tap one for a full chart "
     "with candles and volume, plus news with pictures and a heatmap of the day. Keyless, from Yahoo. "
     "Sleeps overnight and wakes with last night's prices already on screen."},
    {"sports", "Game Day", "scores across eleven leagues", rgb(240, 138, 40),
     "Scores and schedules across eleven leagues, team logos on every row, your favourites pinned to the "
     "top. Tabs are sports rather than leagues, so all seven soccer competitions sit together. Keyless, "
     "from ESPN's own scoreboard endpoint."},
    {"unifi", "Network", "devices, clients, cameras", rgb(64, 156, 255),
     "Your UniFi console at a glance: gateway and switch health, who is connected, and live snapshots "
     "from the Protect cameras. Needs an API key from the Integrations page of the Network app, typed "
     "once on the board."},
    {"social", "Social", "your own numbers", rgb(180, 120, 255),
     "A ticker of your own figures rather than the market's -- followers, stars, downloads, subscribers "
     "-- from Bluesky, Mastodon, GitHub, npm and YouTube, all keyless. Keeps thirty days of history so "
     "the arrows mean something. Built as a demo and parked."},
};
static const uint8_t N_APPS = sizeof APPS / sizeof APPS[0];

enum class View : uint8_t { Picker, Detail, Settings, Themes, Sleep, Info, Setup };
static View view = View::Picker;
static uint8_t sel = 0;  // which app the detail page is showing
static char slotApp[24] = "";
static bool slotFilled = false;
static bool confirmOpen = false;
static uint16_t *preview = nullptr;  // PREVIEW_W x PREVIEW_H, in PSRAM
static const int16_t PREVIEW_W = 400, PREVIEW_H = 240;

static bool inSlot(uint8_t i) { return slotFilled && strcmp(APPS[i].id, slotApp) == 0; }

// ── the picker ───────────────────────────────────────────────────────────
static const uint8_t COLS = 2;
static void appTile(uint8_t i) {
  int16_t x, y, w, h;
  tileRect(i, N_APPS, COLS, &x, &y, &w, &h);
  const AppEntry &a = APPS[i];
  bool loaded = inSlot(i);

  // A wash of the app's own colour, and a solid bar down the left edge. The
  // bar is what you actually recognise from a distance; the wash keeps the
  // tile from reading as a button.
  gfx->fillRoundRect(x, y, w, h, 14, mix(C_BG, a.accent, loaded ? 22 : 12));
  gfx->drawRoundRect(x, y, w, h, 14, loaded ? a.accent : mix(C_RULE, a.accent, 40));
  gfx->fillRoundRect(x, y, 8, h, 4, a.accent);
  gfx->fillRect(x + 4, y, 4, h, mix(C_BG, a.accent, loaded ? 22 : 12));

  textAt(x + 26, y + 18, 3, C_FG, a.name);
  textAt(x + 26, y + 18 + GH(3) + 8, 1, towardsWhite(a.accent, 35), a.blurb);
  if (loaded) {
    const char *t = "LOADED";
    int16_t tw = textWidth(1, t) + 16;
    gfx->fillRoundRect(x + w - tw - 14, y + 16, tw, 22, 11, a.accent);
    textAt(x + w - tw - 8, y + 20, 1, RGB565_BLACK, t);
  }
  textAt(x + 26, y + h - 26, 1, C_MUTED, "tap for details");
}

static void drawPicker() {
  gfx->fillScreen(C_BG);
  textAt(20, 10, 2, C_FG, "Apps");
  settingsIcon(LCD_W - 52, C_MUTED);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, C_RULE);
  for (uint8_t i = 0; i < N_APPS; i++) appTile(i);
  drawHint("tap an app        settings, top right");
}

// ── the detail page ──────────────────────────────────────────────────────
static const int16_t PV_X = LCD_W - PREVIEW_W - 24, PV_Y = 56;
static int16_t btnX, btnY, btnW = 260, btnH = 52;

// /preview/<id>.565, made by tools/make-previews.py. Absent is normal: two
// apps have no preview.svg to render from, and a board that has never had
// `push-config home` run has none at all.
static bool loadPreview(const char *id) {
  if (!preview) preview = (uint16_t *)heap_caps_malloc((size_t)PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
  if (!preview) return false;
  char path[40];
  snprintf(path, sizeof path, "/preview/%s.565", id);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t want = (size_t)PREVIEW_W * PREVIEW_H * 2;
  bool ok = f.size() == want && f.read((uint8_t *)preview, want) == want;
  f.close();
  return ok;
}

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

  if (loadPreview(a.id)) {
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

  // The about text, wrapped by hand: the panel has no word wrap and the
  // column is a fixed width, so the split is arithmetic, not a guess.
  const int16_t tx = 24, tw = PV_X - 48;
  int16_t ty = PV_Y + 4;
  const char *p = a.about;
  char line[96];
  while (*p && ty < UI_Y_HINT - 90) {
    uint16_t n = 0, lastSpace = 0;
    while (p[n] && n < sizeof line - 1) {
      if (p[n] == ' ') {
        line[n] = '\0';
        if (textWidth(1, line) > tw) break;
        lastSpace = n;
      }
      line[n] = p[n];
      n++;
    }
    line[n] = '\0';
    if (p[n] && lastSpace) n = lastSpace;
    line[n] = '\0';
    textAt(tx, ty, 1, C_MUTED, line);
    ty += 22;
    p += n;
    while (*p == ' ') p++;
  }

  btnX = tx;
  btnY = UI_Y_HINT - 72;
  uint16_t fill = loaded ? a.accent : mix(C_BG, a.accent, 30);
  gfx->fillRoundRect(btnX, btnY, btnW, btnH, 26, fill);
  if (!loaded) gfx->drawRoundRect(btnX, btnY, btnW, btnH, 26, a.accent);
  const char *label = loaded ? "Run" : "Install";
  textAt(btnX + (btnW - textWidth(2, label)) / 2, btnY + (btnH - FACES[1].cap) / 2, 2,
         loaded ? RGB565_BLACK : towardsWhite(a.accent, 30), label);
  drawHint(loaded ? "Run hands the screen over; hold BOOT at power-on to come back" : "< apps");
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
  settingRow(5, "About this board", "");
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

// About the BOARD, which is what "Info" should always have meant. The old
// version showed "slot: unknown app", which answered a question nobody asked
// and phrased it as if something were wrong.
static void drawInfoPage() {
  pageHeader("ABOUT THIS BOARD");
  char l[13][56];
  uint8_t n = 0;
  uint64_t mac = ESP.getEfuseMac();
  const esp_partition_t *run = esp_ota_get_running_partition();
  snprintf(l[n++], 56, "Waveshare ESP32-S3-Touch-LCD-7");
  snprintf(l[n++], 56, "800x480 RGB panel, GT911 touch, CH422G expander");
  snprintf(l[n++], 56, "%s rev%d, %d MHz, %d cores", ESP.getChipModel(), ESP.getChipRevision(),
           ESP.getCpuFreqMHz(), ESP.getChipCores());
  snprintf(l[n++], 56, "flash %luMB, PSRAM %luMB", (unsigned long)(ESP.getFlashChipSize() / 1048576),
           (unsigned long)(ESP.getPsramSize() / 1048576));
  snprintf(l[n++], 56, "MAC %02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)(mac >> 40), (uint8_t)(mac >> 32),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
  l[n++][0] = '\0';
  snprintf(l[n++], 56, "Wi-Fi %s  (2.4GHz only -- no 5GHz on this radio)", baseCfg.ssid[0] ? baseCfg.ssid : "not set up");
  snprintf(l[n++], 56, "heap %uk free, PSRAM %uk free", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  uint32_t up = millis() / 1000;
  snprintf(l[n++], 56, "up %luh %02lum", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
  l[n++][0] = '\0';
  snprintf(l[n++], 56, "Home in %s, %uk", run ? run->label : "?", (unsigned)(ESP.getSketchSize() / 1024));
  snprintf(l[n++], 56, "app slot: %s", slotFilled ? (slotApp[0] ? slotApp : "an app that has not named itself") : "empty");
  snprintf(l[n++], 56, "built " __DATE__ " " __TIME__);
  for (uint8_t i = 0; i < n; i++) field(24, 56 + i * 24, (LCD_W - 48) / 8, 1, i < 2 ? C_FG : C_MUTED, l[i]);
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
}

static void draw() {
  switch (view) {
    case View::Picker: drawPicker(); break;
    case View::Detail: drawDetail(); break;
    case View::Settings: drawSettings(); break;
    case View::Themes: drawThemesPage(); break;
    case View::Sleep: drawSleepPage(); break;
    case View::Info: drawInfoPage(); break;
    case View::Setup: wifisetup::draw("waiting for you"); break;
  }
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

  gfx = boardDisplay();
  bool panelOk = gfx->begin();
  boardSetRotation(baseCfg.rotation);
  gfx->setTextWrap(false);
  slotFilled = baseSlotFilled();
  if (!LittleFS.begin(false)) Serial.println("home: no LittleFS -- previews will be placeholders");

  Serial.printf("home: expander %s, panel %s, slot %s (%s), theme %s\n", xp ? "ok" : "NO ACK",
                panelOk ? "ok" : "FAILED", slotFilled ? "filled" : "EMPTY", slotApp[0] ? slotApp : "unclaimed",
                THEMES[baseTheme(N_THEMES)].name);

  drawSplash();
  backlight(255);
  delay(900);

  // A board with no network cannot do anything useful, so setup comes before
  // the picker rather than hiding behind a settings row.
  if (!baseCfg.ssid[0]) {
    view = View::Setup;
    wifisetup::begin();
    return;
  }
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
  bool tap = g == Gesture::Tap || g == Gesture::TapUp;

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
    case View::Picker:
      if (tap && x > LCD_W - 64 && y < UI_Y_ROW0) {
        view = View::Settings;
        draw();
      } else if (tap) {
        for (uint8_t i = 0; i < N_APPS; i++) {
          int16_t tx, ty, tw, th;
          tileRect(i, N_APPS, COLS, &tx, &ty, &tw, &th);
          if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
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
        if (inSlot(sel)) {
          drawHint("starting...", C_GOOD);
          baseHandoff();  // or the app bounces straight back here
          bootIntoApp();
        } else {
          // Installing means getting the binary into ota_0, and Home cannot do
          // that yet -- the LittleFS cache is the next piece. Say the command
          // rather than pretending the button did something.
          char m[96];
          snprintf(m, sizeof m, "not on the board yet:  pio run -e %s -t upload", APPS[sel].id);
          drawHint(m, C_WARN);
        }
      }
      break;

    case View::Settings:
      if (hitBack(x, y, g)) {
        view = View::Picker;
        draw();
      } else if (tap) {
        switch (hitSettingRow(y, 6)) {
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
          case 5: view = View::Info; draw(); break;
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

    case View::Info:
      if (hitBack(x, y, g) || tap) {
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
