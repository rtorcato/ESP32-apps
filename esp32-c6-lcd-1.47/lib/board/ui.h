// Shared UI state for every app on this board: theme, rotation, backlight,
// screen blanking, and the one-button gestures that drive them.
//
// Every app should support light/dark and all four rotations -- the USB-C socket
// is on a fixed edge, so which way up the board sits depends entirely on where
// the cable needs to go. This header exists so that is one include rather than
// a copy-paste tax per app.
//
// What is NOT here: layout. Coordinates are inherently app-specific, so each app
// keeps its own Layout struct and picks one on `uiLandscape()`. Only the pattern
// is shared, not the numbers.
//
// Usage:
//   uiBegin(gfx);                      // after gfx->begin(): loads NVS, applies rotation
//   const Layout *L = uiLandscape() ? &LANDSCAPE : &PORTRAIT;
//   ...
//   switch (uiPoll()) {
//     case UiPress::Rotate: uiApply(uiRot() + 1, uiLight(), true); redraw(); break;
//     case UiPress::Theme:  uiApply(uiRot(), !uiLight(), true);    redraw(); break;
//     case UiPress::Blank:  uiSetScreen(false);                    break;
//     default: break;
//   }
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "board.h"

// ── theme ────────────────────────────────────────────────────────────────
// Backlight lives in the theme rather than as one global pair: a white
// background emits far more light than a black one at the same duty, so light
// mode needs a much lower number to be equally comfortable.
//
// >>> blDay/blNight are the brightness setting. Tune them here. <<<
struct Theme {
  uint16_t bg, fg, dim, muted, rule;
  uint16_t snow, rain, sun, storm, neutral;  // weather/plot accents
  uint16_t warn, bad, good;
  uint8_t blDay, blNight;  // backlight duty, 0-255
};

inline constexpr Theme UI_DARK = {
    RGB565_BLACK,  RGB565_WHITE,   RGB565_DIMGREY, RGB565_GREY, RGB565_DARKGREY,
    RGB565_CYAN,   RGB565_SKYBLUE, RGB565_ORANGE,  RGB565_RED,  RGB565_LIGHTGREY,
    RGB565_ORANGE, RGB565_RED,     RGB565_GREEN,
    /*blDay*/ 140, /*blNight*/ 40,
};

inline constexpr Theme UI_LIGHT = {
    RGB565_WHITESMOKE,    RGB565_BLACK,         RGB565_SILVER,      RGB565_DARKSLATEGREY,
    RGB565_SILVER,        RGB565_DARKCYAN,      RGB565_STEELBLUE,   RGB565_DARKORANGE,
    RGB565_MAROON,        RGB565_DARKSLATEGREY, RGB565_SADDLEBROWN, RGB565_MAROON,
    RGB565_DARKGREEN,
    // Much lower than dark: a near-white panel at 200 is genuinely unpleasant.
    /*blDay*/ 70, /*blNight*/ 18,
};

// Night window for the dimmer backlight. Apps may override before uiBegin().
inline uint8_t uiNightFrom = 23, uiNightTo = 7;

// The backlight duty currently driven. uiBacklightNow() says what the theme
// *wants*; this says what is actually on the panel, which differs while blanked.
inline uint8_t uiBacklightApplied = 0;

// ── state ────────────────────────────────────────────────────────────────
namespace uidetail {
inline Arduino_GFX *gfx = nullptr;
inline Preferences prefs;
inline const Theme *theme = &UI_DARK;
inline uint8_t rot = 0;
inline bool light = false;
inline bool screenOn = true;
}  // namespace uidetail

inline const Theme *uiTheme() { return uidetail::theme; }
inline uint8_t uiRot() { return uidetail::rot; }
inline bool uiLight() { return uidetail::light; }
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

// Backlight duty for right now: theme-specific, day or night. Falls back to the
// day level before the clock is set, since 1970 is not a useful hour.
inline uint8_t uiBacklightNow() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return uidetail::theme->blDay;
  bool night = (t.tm_hour >= uiNightFrom || t.tm_hour < uiNightTo);
  return night ? uidetail::theme->blNight : uidetail::theme->blDay;
}

// Applies rotation, theme and backlight together. Safe in all four rotations:
// board.h's (34, 0, 34, 0) offsets are correct for each, because the driver
// selects a different offset pair per rotation and 240 - 172 - 34 = 34 makes
// the panel symmetric.
inline void uiApply(uint8_t newRot, bool newLight, bool persist) {
  uidetail::rot = newRot & 3;
  uidetail::light = newLight;
  uidetail::theme = newLight ? &UI_LIGHT : &UI_DARK;
  if (uidetail::gfx) uidetail::gfx->setRotation(uidetail::rot);
  uiBacklightApplied = uiBacklightNow();
  backlight(uiBacklightApplied);
  if (persist) {
    uidetail::prefs.putUChar("rot", uidetail::rot);
    uidetail::prefs.putBool("light", uidetail::light);
  }
  Serial.printf("ui: %s, %s (bl %u)\n", uiRotName(), newLight ? "light" : "dark",
                uiBacklightNow());
}

// Call after gfx->begin(). `ns` is the NVS namespace -- give each app its own so
// two apps on the same board don't fight over one stored rotation.
inline void uiBegin(Arduino_GFX *g, const char *ns = "ui") {
  uidetail::gfx = g;
  uidetail::prefs.begin(ns, false);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  uiApply(uidetail::prefs.getUChar("rot", 0), uidetail::prefs.getBool("light", false), false);
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
// Three actions on one button, chosen by hold duration. Firing on release
// rather than on threshold is deliberate: holding for the third action would
// otherwise trigger the second on the way past. A hint is drawn while holding
// so the gestures don't have to be memorised.
enum class UiPress { None, Rotate, Theme, Blank };

inline constexpr uint32_t UI_HOLD_THEME_MS = 1200, UI_HOLD_BLANK_MS = 3000;

inline void uiHoldHint(const char *s) {
  if (!uidetail::gfx) return;
  int16_t w = 6 * (int16_t)strlen(s) + 10, h = 8 + 10;
  int16_t x = (uidetail::gfx->width() - w) / 2, y = (uidetail::gfx->height() - h) / 2;
  uidetail::gfx->fillRect(x, y, w, h, uidetail::theme->fg);  // inverted = overlay
  uidetail::gfx->setTextSize(1);
  uidetail::gfx->setTextColor(uidetail::theme->bg);
  uidetail::gfx->setCursor(x + 5, y + 5);
  uidetail::gfx->print(s);
}

inline UiPress uiPoll() {
  static bool wasDown = false;
  static uint32_t downAt = 0;
  static uint8_t hinted = 0;
  bool down = digitalRead(BTN_BOOT) == LOW;  // active low

  if (down && !wasDown) {
    downAt = millis();
    wasDown = true;
    hinted = 0;
  } else if (down && wasDown) {
    uint32_t held = millis() - downAt;
    uint8_t stage = held >= UI_HOLD_BLANK_MS ? 2 : held >= UI_HOLD_THEME_MS ? 1 : 0;
    if (stage != hinted && uidetail::screenOn) {
      hinted = stage;
      if (stage == 1) uiHoldHint("release: THEME");
      else if (stage == 2) uiHoldHint("release: SCREEN OFF");
    }
  } else if (!down && wasDown) {
    wasDown = false;
    uint32_t held = millis() - downAt;
    if (held <= 40) return UiPress::None;  // debounce
    if (held >= UI_HOLD_BLANK_MS) return UiPress::Blank;
    if (held >= UI_HOLD_THEME_MS) return UiPress::Theme;
    return UiPress::Rotate;
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
    case UiPress::Blank:  uiSetScreen(false); return false;
    case UiPress::Theme:  uiApply(uidetail::rot, !uidetail::light, true); return true;
    case UiPress::Rotate: uiApply(uidetail::rot + 1, uidetail::light, true); return true;
    default:              return false;
  }
}
