// Home -- the base OS. Owns the board, the shared settings, and which app
// boots. See the README next to this file for the design.
//
// It lives in `factory` and stays there. Tapping an app sets the boot
// partition to ota_0 and restarts; the app's Home control sets it back. Two
// seconds each way, no network involved.
//
// v1 loads only what is already in the slot. Getting a DIFFERENT app in there
// is still `pio run -e <app> -t upload` over USB, which lands in ota_0 by
// itself because the platform prefers that partition when the table has one.
// Copying a cached binary from LittleFS is the next step and changes nothing
// here except where the bytes come from.
#include <baseos.h>
#include <board.h>
#include <helv.h>
#include <sleep.h>
#include <ui.h>

// Every app the board knows about. Only one can be in the slot at a time, so
// the rest are drawn dim -- the grid is what EXISTS, not what is runnable,
// because "where did Game Day go" is a worse question than a greyed tile.
struct AppEntry {
  const char *id, *name, *blurb;
};
static const AppEntry APPS[] = {
    {"ticker", "Ticker Tape", "stocks, indices, FX, coins"},
    {"sports", "Game Day", "scores across eleven leagues"},
    {"unifi", "Network", "devices, clients, cameras"},
    {"social", "Social", "your own numbers"},
};
static const uint8_t N_APPS = sizeof APPS / sizeof APPS[0];

enum class View : uint8_t { Picker, Settings, Themes, Sleep };
static View view = View::Picker;
static char slotApp[24] = "";
static bool slotFilled = false;
static bool confirmOpen = false;

// ── the picker ───────────────────────────────────────────────────────────
static const uint8_t COLS = 2;
static void appTile(uint8_t i) {
  int16_t x, y, w, h;
  tileRect(i, N_APPS, COLS, &x, &y, &w, &h);
  bool loaded = slotFilled && strcmp(APPS[i].id, slotApp) == 0;
  uint16_t edge = loaded ? C_GOLD : C_RULE, label = loaded ? C_FG : C_MUTED;
  gfx->fillRoundRect(x, y, w, h, 14, towardsWhite(C_BG, loaded ? 10 : 4));
  gfx->drawRoundRect(x, y, w, h, 14, edge);
  if (loaded) gfx->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 13, edge);
  textAt(x + 20, y + 18, 3, label, APPS[i].name);
  textAt(x + 20, y + 18 + GH(3) + 8, 1, C_MUTED, APPS[i].blurb);
  const char *tag = loaded ? "IN THE SLOT -- tap to run" : "not loaded";
  textAt(x + 20, y + h - 26, 1, loaded ? C_GOOD : C_DIM, tag);
}

static void drawPicker() {
  gfx->fillScreen(C_BG);
  textAt(20, 10, 2, C_FG, "Apps");
  settingsIcon(LCD_W - 52, C_MUTED);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, C_RULE);
  for (uint8_t i = 0; i < N_APPS; i++) appTile(i);
  drawHint(slotFilled ? "tap the loaded app to run it        settings, top right"
                      : "no app in the slot -- flash one: pio run -e <app> -t upload");
}

// ── settings ─────────────────────────────────────────────────────────────
static const char *const SLEEP_NAMES[] = {"never", "night"};
static void drawSettings() {
  pageHeader("SETTINGS", "the board's, not any one app's");
  char v[40];
  settingRow(0, "Theme", THEMES[baseTheme(N_THEMES)].name);
  snprintf(v, sizeof v, "%u\xC2\xB0", (unsigned)(baseCfg.rotation * 90));
  settingRow(1, "Orientation", v);
  if (baseCfg.sleepMode) snprintf(v, sizeof v, "night, %02u-%02u", baseCfg.sleepFrom, baseCfg.sleepTo);
  else snprintf(v, sizeof v, "never");
  settingRow(2, "Sleep", v);
  settingRow(3, "Wi-Fi", baseCfg.ssid[0] ? baseCfg.ssid : "not set");
  settingRow(4, "Shut down", "");
  snprintf(v, sizeof v, "slot: %s", slotFilled ? (slotApp[0] ? slotApp : "unknown app") : "empty");
  settingRow(5, "Info", v);
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

// The splash. Brief and plain on purpose: it exists so a power-on reads as
// "the board is starting" rather than "the screen is broken", and every
// millisecond it holds is a millisecond before the picker is usable.
static void drawSplash() {
  gfx->fillScreen(C_BG);
  const char *name = "ESP32-S3 7\"";
  int16_t cy = LCD_H / 2;
  textAt((LCD_W - textWidth(5, name)) / 2, cy - 90, 5, C_FG, name);
  gfx->fillRoundRect((LCD_W - 200) / 2, cy + 18, 200, 4, 2, C_GOLD);
  char sub[64];
  snprintf(sub, sizeof sub, "%u apps  ·  %s", N_APPS, slotFilled ? (slotApp[0] ? slotApp : "app in the slot") : "no app loaded");
  textAt((LCD_W - textWidth(2, sub)) / 2, cy + 44, 2, C_MUTED, sub);
}

static void draw() {
  switch (view) {
    case View::Picker: drawPicker(); break;
    case View::Settings: drawSettings(); break;
    case View::Themes: drawThemesPage(); break;
    case View::Sleep: drawSleepPage(); break;
  }
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

  Serial.printf("Home: expander %s, panel %s, woke by %s\n", xp ? "ok" : "NO ACK", panelOk ? "ok" : "FAILED",
                wakeCause() == Wake::Cold ? "power-on" : "a button or a timer");
  Serial.printf("Home: slot %s (%s), theme %s, psram %uk free\n", slotFilled ? "filled" : "EMPTY",
                slotApp[0] ? slotApp : "unclaimed", THEMES[baseTheme(N_THEMES)].name, ESP.getFreePsram() / 1024);

  drawSplash();
  backlight(255);
  delay(900);  // long enough to read, short enough not to be in the way
  draw();
}

void loop() {
  int16_t x, y, dy;
  Gesture g = pollGesture(&x, &y, &dy);
  if (g == Gesture::None) return;

  if (confirmOpen) {  // the shutdown sheet owns every touch while it is up
    if (g == Gesture::Tap || g == Gesture::TapUp) {
      if (hitPill(x, y)) goToSleep(0);
      confirmOpen = false;
      sheetClose();
      draw();
    }
    return;
  }

  switch (view) {
    case View::Picker:
      if ((g == Gesture::Tap || g == Gesture::TapUp) && x > LCD_W - 64 && y < UI_Y_ROW0) {
        view = View::Settings;
        draw();
      } else if (g == Gesture::Tap || g == Gesture::TapUp) {
        for (uint8_t i = 0; i < N_APPS; i++) {
          int16_t tx, ty, tw, th;
          tileRect(i, N_APPS, COLS, &tx, &ty, &tw, &th);
          if (x < tx || x >= tx + tw || y < ty || y >= ty + th) continue;
          if (slotFilled && strcmp(APPS[i].id, slotApp) == 0) {
            drawHint("starting...", C_GOOD);
            baseHandoff();  // or the app bounces straight back here
            bootIntoApp();
          } else {
            char m[80];
            snprintf(m, sizeof m, "%s is not in the slot -- pio run -e %s -t upload", APPS[i].name, APPS[i].id);
            drawHint(m, C_WARN);
          }
        }
      }
      break;

    case View::Settings:
      if (g == Gesture::SwipeLeft || (g != Gesture::Drag && x < 60 && y < UI_Y_ROW0)) {
        view = View::Picker;
        draw();
      } else if (g == Gesture::Tap || g == Gesture::TapUp) {
        switch (hitSettingRow(y, 6)) {
          case 0: view = View::Themes; draw(); break;
          case 1:
            baseCfg.rotation = (baseCfg.rotation + 1) % 4;
            baseSave();
            drawHint("rotation saved -- it applies on the next boot", C_WARN);
            drawSettings();
            break;
          case 2: view = View::Sleep; draw(); break;
          case 3: drawHint("Wi-Fi setup is not in Home yet", C_WARN); break;
          case 4: confirmOpen = true; drawShutdownSheet(); break;
          default: break;
        }
      }
      break;

    case View::Themes:
      if (g == Gesture::SwipeLeft || (g != Gesture::Drag && x < 60 && y < UI_Y_ROW0)) {
        view = View::Settings;
        draw();
      } else if (g == Gesture::Tap || g == Gesture::TapUp) {
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

    case View::Sleep:
      if (g == Gesture::SwipeLeft || (g != Gesture::Drag && x < 60 && y < UI_Y_ROW0)) {
        view = View::Settings;
        draw();
      } else if (g == Gesture::Tap || g == Gesture::TapUp) {
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
  }
}
