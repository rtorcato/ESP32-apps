// Shared UI state for every app on this board: colour scheme, rotation,
// backlight, screen blanking, and the one-button gestures that drive them.
//
// Every app should offer all four rotations and a choice of scheme -- the USB-C
// socket is on a fixed edge, so which way up the board sits depends entirely on
// where the cable needs to go. This header exists so that is one include rather
// than a copy-paste tax per app.
//
// What is NOT here: layout. Coordinates are inherently app-specific, so each app
// keeps its own Layout struct and picks one on `uiLandscape()`. Only the pattern
// is shared, not the numbers.
//
// The board has two buttons but only BOOT (GPIO9) is readable: RESET is wired to
// the chip's EN pin, so it can only reboot. That's why all gestures share one
// button.
//
// Usage:
//   uiBegin(gfx, "myapp");                     // after gfx->begin()
//   const Layout *L = uiLandscape() ? &LANDSCAPE : &PORTRAIT;
//   if (uiHandle(uiPoll())) redrawEverything();
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "board.h"

// ── colour schemes ───────────────────────────────────────────────────────
// Custom RGB565 values rather than only the named constants, so palettes can be
// chosen properly instead of approximated.
//
// Backlight lives in the scheme rather than as one global pair: a white
// background emits far more light than a black one at the same duty, so a light
// scheme needs a much lower number to be equally comfortable. The backlight is
// also the board's dominant heat source (measured: 200 -> 140 is worth ~6 C),
// so these numbers are the main power/heat knob too.
//
// >>> blDay/blNight are the brightness setting. Tune them here. <<<
struct Theme {
  const char *name;
  uint16_t bg, fg, dim, muted, rule;
  uint16_t snow, rain, sun, storm, neutral;  // accents: cool -> warm -> alert
  uint16_t warn, bad, good;
  uint8_t blDay, blNight;  // backlight duty, 0-255
};

inline constexpr Theme UI_SCHEMES[] = {
    // 0: dark -- the default. Neutral greys, saturated accents.
    {"dark", RGB565(0, 0, 0), RGB565(248, 252, 248), RGB565(105, 105, 105),
     RGB565(150, 150, 150), RGB565(90, 90, 90), RGB565(0, 255, 255), RGB565(135, 206, 235),
     RGB565(255, 165, 0), RGB565(255, 60, 60), RGB565(200, 200, 200), RGB565(255, 165, 0),
     RGB565(255, 60, 60), RGB565(60, 220, 100), 140, 40},

    // 1: light -- for a bright room. Much lower backlight; a near-white panel
    // at 200 is genuinely unpleasant.
    {"light", RGB565(245, 245, 245), RGB565(0, 0, 0), RGB565(170, 170, 170),
     RGB565(70, 80, 90), RGB565(200, 200, 200), RGB565(0, 110, 130), RGB565(50, 100, 160),
     RGB565(200, 100, 0), RGB565(150, 20, 20), RGB565(70, 80, 90), RGB565(150, 90, 0),
     RGB565(150, 20, 20), RGB565(0, 110, 40), 70, 18},

    // 2: amber -- amber CRT. Warm, very easy at night.
    {"amber", RGB565(8, 4, 0), RGB565(255, 176, 0), RGB565(120, 80, 0), RGB565(190, 130, 0),
     RGB565(90, 60, 0), RGB565(255, 210, 130), RGB565(230, 170, 60), RGB565(255, 200, 0),
     RGB565(255, 90, 0), RGB565(200, 140, 0), RGB565(255, 140, 0), RGB565(255, 80, 0),
     RGB565(210, 190, 0), 130, 30},

    // 3: green -- green phosphor terminal.
    {"green", RGB565(0, 12, 0), RGB565(60, 255, 110), RGB565(0, 100, 40),
     RGB565(0, 170, 70), RGB565(0, 80, 35), RGB565(160, 255, 200), RGB565(80, 220, 160),
     RGB565(200, 255, 120), RGB565(255, 100, 80), RGB565(0, 190, 90), RGB565(230, 220, 80),
     RGB565(255, 90, 70), RGB565(60, 255, 110), 130, 30},

    // 4: ocean -- dark blue-grey, cooler than plain black and easy to read.
    {"ocean", RGB565(11, 30, 45), RGB565(207, 232, 245), RGB565(80, 110, 130),
     RGB565(140, 175, 195), RGB565(50, 80, 100), RGB565(120, 230, 255), RGB565(100, 180, 240),
     RGB565(255, 190, 90), RGB565(255, 110, 110), RGB565(180, 205, 220), RGB565(255, 190, 90),
     RGB565(255, 110, 110), RGB565(110, 230, 160), 135, 35},
};

inline constexpr uint8_t UI_SCHEME_COUNT = sizeof(UI_SCHEMES) / sizeof(UI_SCHEMES[0]);

// Night window for the dimmer backlight. Apps may override before uiBegin().
inline uint8_t uiNightFrom = 23, uiNightTo = 7;

// The duty currently driven. uiBacklightNow() says what the scheme *wants*;
// this says what is actually on the panel, which differs while blanked.
inline uint8_t uiBacklightApplied = 0;

// Set false to silence the per-press serial log once the gestures are trusted.
inline bool uiLogButton = true;

// ── state ────────────────────────────────────────────────────────────────
namespace uidetail {
inline Arduino_GFX *gfx = nullptr;
inline Preferences prefs;
inline uint8_t scheme = 0;
inline uint8_t rot = 0;
inline bool screenOn = true;
}  // namespace uidetail

inline const Theme *uiTheme() { return &UI_SCHEMES[uidetail::scheme]; }
inline uint8_t uiScheme() { return uidetail::scheme; }
inline const char *uiSchemeName() { return UI_SCHEMES[uidetail::scheme].name; }
inline uint8_t uiRot() { return uidetail::rot; }
inline bool uiScreenOn() { return uidetail::screenOn; }
// Rotations 0 and 2 are portrait, 1 and 3 are landscape -- the 180 degree flips
// reuse the same layout and only hand a different rotation to the driver.
inline bool uiLandscape() { return uidetail::rot & 1; }

inline const char *uiRotName() {
  switch (uidetail::rot & 3) {
    case 0:  return "portrait 0";
    case 1:  return "landscape 90";
    case 2:  return "portrait 180";
    default: return "landscape 270";
  }
}

// Backlight duty for right now: scheme-specific, day or night. Falls back to the
// day level before the clock is set, since 1970 is not a useful hour.
inline uint8_t uiBacklightNow() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return uiTheme()->blDay;
  bool night = (t.tm_hour >= uiNightFrom || t.tm_hour < uiNightTo);
  return night ? uiTheme()->blNight : uiTheme()->blDay;
}

// Applies rotation, scheme and backlight together. Safe in all four rotations:
// board.h's (34, 0, 34, 0) offsets are correct for each, because the driver
// selects a different offset pair per rotation and 240 - 172 - 34 = 34 makes
// the panel symmetric.
inline void uiApply(uint8_t newRot, uint8_t newScheme, bool persist) {
  uidetail::rot = newRot & 3;
  uidetail::scheme = newScheme % UI_SCHEME_COUNT;
  if (uidetail::gfx) uidetail::gfx->setRotation(uidetail::rot);
  uiBacklightApplied = uiBacklightNow();
  backlight(uiBacklightApplied);
  if (persist) {
    uidetail::prefs.putUChar("rot", uidetail::rot);
    uidetail::prefs.putUChar("scheme", uidetail::scheme);
  }
  Serial.printf("ui: %s, scheme %u/%u %s, bl %u\n", uiRotName(), uidetail::scheme,
                UI_SCHEME_COUNT, uiSchemeName(), uiBacklightApplied);
}

// Call after gfx->begin(). `ns` is the NVS namespace -- give each app its own so
// two apps on the same board don't fight over one stored rotation.
inline void uiBegin(Arduino_GFX *g, const char *ns = "ui") {
  uidetail::gfx = g;
  uidetail::prefs.begin(ns, false);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  uiApply(uidetail::prefs.getUChar("rot", 0), uidetail::prefs.getUChar("scheme", 0), false);
}

// "Off" means the display, not the chip. True deep sleep cannot be woken by this
// button: BOOT is GPIO9 and the C6's RTC-capable pins are GPIO0-7
// (SOC_RTCIO_PIN_COUNT == 8), so only RESET or a timer could bring it back. On a
// USB-powered device that is a worse interface for no meaningful saving, and
// keeping the app running means its data is current when the screen returns.
inline void uiSetScreen(bool on) {
  uidetail::screenOn = on;
  if (!uidetail::gfx) return;
  if (on) {
    uidetail::gfx->displayOn();
    uiBacklightApplied = uiBacklightNow();
    backlight(uiBacklightApplied);
    Serial.println("ui: screen on");
  } else {
    uiBacklightApplied = 0;
    backlight(0);
    uidetail::gfx->displayOff();  // ST7789 SLPIN -- stops the panel, not just the light
    led(0, 0, 0);
    Serial.println("ui: screen off (press BOOT to wake)");
  }
}

// ── button ───────────────────────────────────────────────────────────────
// Three actions on one button, chosen by hold duration:
//   tap        -> next rotation
//   hold 1.2s  -> next colour scheme
//   hold 3s    -> blank the panel
//   hold 6s    -> UiPress::Setup, returned to the app (uiHandle ignores it) for
//                 apps that have something to configure
//
// Decided on release rather than at the threshold, because holding for the third
// action would otherwise fire the second on the way past. A hint is drawn while
// holding so the gestures don't have to be memorised, and every press is logged
// with its measured duration -- if a gesture "doesn't work", the log says
// whether the firmware saw a different duration than you intended.
enum class UiPress { None, Rotate, Scheme, Blank, Setup };

inline constexpr uint32_t UI_HOLD_SCHEME_MS = 1200, UI_HOLD_BLANK_MS = 3000,
                          UI_HOLD_SETUP_MS = 6000;
inline constexpr uint32_t UI_DEBOUNCE_MS = 25;

inline void uiHoldHint(const char *s) {
  if (!uidetail::gfx) return;
  int16_t w = 6 * (int16_t)strlen(s) + 10, h = 8 + 10;
  int16_t x = (uidetail::gfx->width() - w) / 2, y = (uidetail::gfx->height() - h) / 2;
  uidetail::gfx->fillRect(x, y, w, h, uiTheme()->fg);  // inverted = reads as an overlay
  uidetail::gfx->setTextSize(1);
  uidetail::gfx->setTextColor(uiTheme()->bg);
  uidetail::gfx->setCursor(x + 5, y + 5);
  uidetail::gfx->print(s);
}

inline UiPress uiPoll() {
  // Level must be stable for UI_DEBOUNCE_MS before a transition is accepted.
  // Without this, contact bounce during a long hold registers as release +
  // press, and every fragment shorter than 1.2s looks like a tap -- so a hold
  // would appear to only ever rotate.
  static bool pressed = false, lastRaw = false;
  static uint32_t lastChange = 0, downAt = 0;
  static uint8_t hinted = 0;

  bool raw = digitalRead(BTN_BOOT) == LOW;  // active low
  uint32_t now = millis();
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = now;
  }

  if (now - lastChange >= UI_DEBOUNCE_MS && raw != pressed) {
    pressed = raw;
    if (pressed) {
      downAt = now;
      hinted = 0;
    } else {
      uint32_t held = now - downAt;
      UiPress p = held >= UI_HOLD_SETUP_MS   ? UiPress::Setup
                  : held >= UI_HOLD_BLANK_MS ? UiPress::Blank
                  : held >= UI_HOLD_SCHEME_MS ? UiPress::Scheme
                                              : UiPress::Rotate;
      if (uiLogButton) {
        Serial.printf("btn: held %lums -> %s\n", (unsigned long)held,
                      p == UiPress::Setup   ? "SETUP (6s)"
                      : p == UiPress::Blank  ? "BLANK (3s)"
                      : p == UiPress::Scheme ? "SCHEME (1.2s)"
                                             : "ROTATE (tap)");
      }
      return p;
    }
  }

  if (pressed) {
    uint32_t held = now - downAt;
    uint8_t stage = held >= UI_HOLD_BLANK_MS ? 2 : held >= UI_HOLD_SCHEME_MS ? 1 : 0;
    if (stage != hinted && uidetail::screenOn) {
      hinted = stage;
      if (stage == 1) uiHoldHint("release: COLOUR");
      else if (stage == 2) uiHoldHint("release: SCREEN OFF");
    }
  }
  return UiPress::None;
}

// Handles the gestures an app never wants to special-case. Returns true if the
// app must redraw. Blanking and waking are handled internally.
inline bool uiHandle(UiPress p) {
  if (!uidetail::screenOn) {
    // Any press wakes -- making you remember which gesture brings the screen
    // back would be a bad joke.
    if (p != UiPress::None) {
      uiSetScreen(true);
      return true;
    }
    return false;
  }
  switch (p) {
    // Setup falls through to blanking. An app with something to configure
    // checks for Setup *before* delegating here, so it never reaches this line;
    // an app without one would otherwise silently lose the gesture -- holding
    // past 6s would do nothing where holding 3s blanked. Falling back keeps a
    // long hold meaning "off" everywhere.
    case UiPress::Setup:
    case UiPress::Blank:  uiSetScreen(false); return false;
    case UiPress::Scheme: uiApply(uidetail::rot, uidetail::scheme + 1, true); return true;
    case UiPress::Rotate: uiApply(uidetail::rot + 1, uidetail::scheme, true); return true;
    default:              return false;
  }
}
