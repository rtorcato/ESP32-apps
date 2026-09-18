// ui.h -- what every app on the 7" shares: the faces and text helpers, the
// themes and the colours they set, the header's marks, the sheet, the tile
// pages, the gesture recogniser, the HTTP body reader. Lifted from the
// ticker on 2026-09-18 when the sports board wanted all of it; the ticker
// still carries its own copies (ponytail: migrate it when it is next open).
// Landscape 800x480 only; the ticker's portrait layout stays its own.
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <board.h>
#include <esp_heap_caps.h>
#include <helv.h>

inline Arduino_GFX *gfx = nullptr;
inline bool touchHeld = false;

// ── faces ────────────────────────────────────────────────────────────────
struct Face { const uint8_t *font; uint8_t cap, desc; };
inline const Face FACES[] = {{u8g2_font_helvR14_tr, 14, 4}, {u8g2_font_helvB18_tr, 19, 5}, {u8g2_font_helvB24_tr, 25, 7},
                             {u8g2_font_logisoso50_tn, 50, 13}, {u8g2_font_logisoso58_tr, 58, 15}};
inline const uint8_t GWS[] = {8, 11, 14, 28, 34};
#define GW(s) (GWS[(s) - 1])
#define GH(s) (FACES[(s) - 1].cap + FACES[(s) - 1].desc)

// ── colours and themes ───────────────────────────────────────────────────
inline constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
inline const uint16_t C_FG = 0xFFFF, C_GOOD = 0x07E0, C_BAD = 0xF800, C_WARN = 0xFFE0;
struct Theme { const char *name; uint16_t bg, rule, accent; };
inline const Theme THEMES[] = {
    {"black", rgb(0, 0, 0), rgb(32, 32, 32), rgb(248, 168, 0)},
    {"midnight", rgb(0, 24, 72), rgb(0, 56, 140), rgb(90, 200, 255)},
    {"royal", rgb(24, 0, 120), rgb(64, 32, 180), rgb(255, 200, 80)},
    {"ocean", rgb(0, 60, 80), rgb(0, 110, 140), rgb(0, 240, 220)},
    {"forest", rgb(0, 56, 24), rgb(0, 110, 50), rgb(200, 255, 120)},
    {"terminal", rgb(0, 16, 0), rgb(0, 80, 0), rgb(120, 255, 120)},
    {"espresso", rgb(56, 28, 8), rgb(110, 60, 20), rgb(255, 180, 60)},
    {"burgundy", rgb(96, 0, 32), rgb(160, 24, 64), rgb(255, 170, 190)},
    {"purple", rgb(64, 0, 96), rgb(120, 30, 170), rgb(255, 120, 255)},
    {"slate", rgb(36, 48, 68), rgb(72, 96, 130), rgb(140, 190, 255)},
    {"graphite", rgb(64, 64, 70), rgb(110, 110, 120), rgb(255, 140, 0)},
    {"olive", rgb(48, 56, 0), rgb(96, 110, 20), rgb(255, 230, 90)},
};
inline const uint8_t N_THEMES = sizeof THEMES / sizeof THEMES[0];
inline uint8_t sTheme = 0;
inline uint16_t C_BG = THEMES[0].bg, C_RULE = THEMES[0].rule, C_GOLD = THEMES[0].accent, C_DIM = 0x630C, C_MUTED = 0xA534;
inline uint16_t mix(uint16_t a, uint16_t b, uint8_t pct) {  // pct of the way from a to b
  int r0 = (a >> 11) * 255 / 31, g0 = ((a >> 5) & 63) * 255 / 63, b0 = (a & 31) * 255 / 31;
  int r1 = (b >> 11) * 255 / 31, g1 = ((b >> 5) & 63) * 255 / 63, b1 = (b & 31) * 255 / 31;
  return rgb(r0 + (r1 - r0) * pct / 100, g0 + (g1 - g0) * pct / 100, b0 + (b1 - b0) * pct / 100);
}
inline uint16_t towardsWhite(uint16_t c, uint8_t pct) { return mix(c, 0xFFFF, pct); }
inline void applyTheme() {
  C_BG = THEMES[sTheme].bg;
  C_RULE = THEMES[sTheme].rule;
  C_GOLD = THEMES[sTheme].accent;
  C_DIM = towardsWhite(C_BG, 40);
  C_MUTED = towardsWhite(C_BG, 65);
}

// ── text ─────────────────────────────────────────────────────────────────
inline int16_t textWidth(uint8_t size, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(FACES[size - 1].font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}
inline void textAt(int16_t x, int16_t y, uint8_t size, uint16_t fg, const char *s) {  // y is the box top
  gfx->setFont(FACES[size - 1].font);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y + FACES[size - 1].cap);
  gfx->print(s);
}
inline void field(int16_t x, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, GW(size) * chars, GH(size), C_BG);
  textAt(x, y, size, fg, s);
}
inline void fieldRight(int16_t right, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(right - w, y, min<int16_t>(w + 6, LCD_W - (right - w)), GH(size), C_BG);
  textAt(right - textWidth(size, s), y, size, fg, s);
}
inline void fieldCentre(int16_t cx, int16_t y, uint8_t chars, uint8_t size, uint16_t fg, const char *s) {
  int16_t w = GW(size) * chars;
  gfx->fillRect(cx - w / 2, y, w, GH(size), C_BG);
  textAt(cx - textWidth(size, s) / 2, y, size, fg, s);
}
inline const int16_t UI_Y_HINT = 452, UI_Y_ROW0 = 40, UI_S_Y0 = 56, UI_S_H = 35;
inline void drawHint(const char *s, uint16_t c = C_DIM) { fieldCentre(LCD_W / 2, UI_Y_HINT, 80, 1, c, s); }

// ── the header's marks ───────────────────────────────────────────────────
inline void chevron(int16_t right, int16_t cy, uint16_t c) {  // a disclosure mark, its point at right
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(right - 9 + d, cy - 8, right - 1 + d, cy, c);
    gfx->drawLine(right - 1 + d, cy, right - 9 + d, cy + 8, c);
  }
}
inline void backMark(int16_t left, int16_t cy, uint16_t c) {  // its mirror, the point at left
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(left + 8 + d, cy - 8, left + d, cy, c);
    gfx->drawLine(left + d, cy, left + 8 + d, cy + 8, c);
  }
}
inline void checkMark(int16_t right, int16_t cy, uint16_t c) {
  for (int8_t d = 0; d < 2; d++) {
    gfx->drawLine(right - 16, cy + d, right - 10, cy + 6 + d, c);
    gfx->drawLine(right - 10, cy + 6 + d, right, cy - 6 + d, c);
  }
}
inline void settingsIcon(int16_t x, uint16_t c) {  // three sliders, 32px wide, in the header
  for (uint8_t r = 0; r < 3; r++) {
    gfx->drawFastHLine(x + 5, 12 + r * 8, 22, c);
    gfx->fillCircle(x + 9 + (r == 1 ? 12 : r == 2 ? 6 : 0), 12 + r * 8, 3, c);
  }
}
// A page's header: the back mark, the title, a note; the rule under. The app draws the right side.
inline void pageHeader(const char *title, const char *note = nullptr) {
  gfx->fillScreen(C_BG);
  backMark(14, 22, C_MUTED);
  textAt(36, 10, 2, C_FG, title);
  if (note) textAt(36 + textWidth(2, title) + 14, 14, 1, C_MUTED, note);
  gfx->drawFastHLine(0, UI_Y_ROW0 - 1, LCD_W, C_RULE);
}
// A settings row: the label, a value in grey, the chevron; a rule under.
inline void settingRow(uint8_t i, const char *label, const char *value, uint16_t labelColour = C_MUTED) {
  int16_t y = UI_S_Y0 + i * UI_S_H;
  gfx->fillRect(20, y + 3, LCD_W - 20, GH(2) + 4, C_BG);
  textAt(20, y + 5, 2, labelColour, label);
  chevron(LCD_W - 20, y + 5 + GH(2) / 2, C_MUTED);
  if (value && value[0]) textAt(LCD_W - 40 - textWidth(2, value), y + 5, 2, C_MUTED, value);
  gfx->drawFastHLine(20, y + UI_S_H - 1, LCD_W - 40, C_RULE);
}
inline int8_t hitSettingRow(int16_t y, uint8_t n) {
  if (y < UI_S_Y0 || y >= UI_S_Y0 + UI_S_H * n) return -1;
  return (y - UI_S_Y0) / UI_S_H;
}

// ── tile pages: the themes ───────────────────────────────────────────────
inline void tileRect(uint8_t i, uint8_t n, uint8_t cols, int16_t *x, int16_t *y, int16_t *w, int16_t *h) {
  uint8_t rows = (n + cols - 1) / cols;
  const int16_t gap = 16;
  *w = (LCD_W - 40 - gap * (cols - 1)) / cols;
  *h = (UI_Y_HINT - 8 - UI_S_Y0 - gap * (rows - 1)) / rows;
  *x = 20 + (i % cols) * (*w + gap);
  *y = UI_S_Y0 + (i / cols) * (*h + gap);
}
inline void drawThemeTile(uint8_t i) {
  int16_t x, y, w, h;
  tileRect(i, N_THEMES, 4, &x, &y, &w, &h);
  const Theme &t = THEMES[i];
  gfx->fillRoundRect(x, y, w, h, 12, t.bg);
  gfx->drawRoundRect(x, y, w, h, 12, i == sTheme ? C_FG : t.rule);
  if (i == sTheme)
    for (int8_t d = 1; d < 3; d++) gfx->drawRoundRect(x + d, y + d, w - 2 * d, h - 2 * d, 12 - d, C_FG);
  textAt(x + 14, y + 10, 2, C_FG, t.name);
  gfx->fillRect(x + 14, y + 10 + GH(2) + 4, 40, 3, t.accent);
  int16_t ry = y + h / 2 + 2;
  gfx->drawFastHLine(x + 14, ry - 6, w - 28, t.rule);
  textAt(x + 14, ry, 2, C_FG, "HOME");
  textAt(x + w - 14 - textWidth(2, "24"), ry, 2, C_GOOD, "24");
  textAt(x + 14, ry + GH(2) + 4, 2, towardsWhite(t.bg, 65), "AWAY");
  textAt(x + w - 14 - textWidth(2, "17"), ry + GH(2) + 4, 2, C_BAD, "17");
}
inline void drawThemesPage() {
  pageHeader("THEMES", "colours for every page");
  for (uint8_t i = 0; i < N_THEMES; i++) drawThemeTile(i);
  drawHint("tap a theme        < settings");
}
inline int8_t hitTheme(int16_t tx, int16_t ty) {
  for (uint8_t i = 0; i < N_THEMES; i++) {
    int16_t x, y, w, h;
    tileRect(i, N_THEMES, 4, &x, &y, &w, &h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return i;
  }
  return -1;
}

// ── the sheet: a panel over a page the scan-out reads 1:1 ─────────────────
inline uint16_t *sheetSave = nullptr;
inline int16_t shX, shY, shW, shH;
inline uint16_t sheetFill() { return towardsWhite(C_BG, 8); }
inline void sheetOpen(int16_t h) {
  const size_t px = (size_t)LCD_W * LCD_H;
  uint16_t *fb = boardFramebuffer();
  if (!sheetSave) sheetSave = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
  if (sheetSave) memcpy(sheetSave, fb, px * 2);
  for (size_t i = 0; i < px; i++) fb[i] = (fb[i] >> 1) & 0x7BEF;
  shW = min<int16_t>(LCD_W - 40, 560);
  shH = h;
  shX = (LCD_W - shW) / 2;
  shY = LCD_H - shH - 20;
  gfx->fillRoundRect(shX, shY, shW, shH, 18, sheetFill());
  gfx->drawRoundRect(shX, shY, shW, shH, 18, C_RULE);
}
inline void sheetClose() {
  if (sheetSave) memcpy(boardFramebuffer(), sheetSave, (size_t)LCD_W * LCD_H * 2);
}
inline bool inSheet(int16_t x, int16_t y) { return x >= shX && x < shX + shW && y >= shY && y < shY + shH; }
// The shutdown sheet: a power glyph, the title, two lines, the pill, Cancel. hitPill says the pill was tapped.
inline int16_t pillX, pillY, pillW;
inline void powerGlyph(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (float a = 35; a <= 325; a += 0.5f) {
    float rad = (a - 90) * 3.14159265f / 180;
    for (int16_t k = 0; k < 5; k++) gfx->drawPixel(cx + (int16_t)lroundf((r - k) * cosf(rad)), cy + (int16_t)lroundf((r - k) * sinf(rad)), c);
  }
  gfx->fillRect(cx - 2, cy - r - 4, 5, r + 2, c);
}
inline void drawShutdownSheet() {
  sheetOpen(336);
  int16_t cx = LCD_W / 2, gy = shY + 60;
  powerGlyph(cx, gy, 36, C_FG);
  textAt(cx - textWidth(3, "Shut Down") / 2, shY + 110, 3, C_FG, "Shut Down");
  const char *l1 = "Everything goes dark and stays dark.", *l2 = "The BOOT button on the back turns it on.";
  textAt(cx - textWidth(1, l1) / 2, shY + 150, 1, C_MUTED, l1);
  textAt(cx - textWidth(1, l2) / 2, shY + 172, 1, C_MUTED, l2);
  pillX = shX + 40;
  pillW = shW - 80;
  pillY = shY + 208;
  gfx->fillRoundRect(pillX, pillY, pillW, 48, 24, rgb(44, 48, 54));
  textAt(cx - textWidth(2, "Shut Down") / 2, pillY + (48 - FACES[1].cap) / 2, 2, C_FG, "Shut Down");
  gfx->drawRoundRect(pillX, shY + 268, pillW, 48, 24, C_RULE);
  textAt(cx - textWidth(2, "Cancel") / 2, shY + 268 + (48 - FACES[1].cap) / 2, 2, C_MUTED, "Cancel");
}
inline bool hitPill(int16_t x, int16_t y) { return y >= pillY && y < pillY + 48 && x >= pillX && x < pillX + pillW; }
// A picker sheet: a title, a row per value, a check on the current one.
inline const int16_t SH_ROW = 44;
inline void drawChoiceSheet(const char *title, const char *const *names, uint8_t n, uint8_t sel) {
  sheetOpen(SH_ROW + n * SH_ROW + 16);
  textAt(shX + 20, shY + (SH_ROW - GH(2)) / 2, 2, C_MUTED, title);
  for (uint8_t i = 0; i < n; i++) {
    int16_t y = shY + SH_ROW + i * SH_ROW, x = shX + 20, w = shW - 40;
    textAt(x, y + (SH_ROW - GH(2)) / 2, 2, i == sel ? C_FG : C_MUTED, names[i]);
    if (i == sel) checkMark(x + w, y + SH_ROW / 2, C_GOOD);
    gfx->drawFastHLine(x, y + SH_ROW - 1, w, C_RULE);
  }
}
inline int8_t hitChoice(int16_t x, int16_t y, uint8_t n) {
  int16_t y0 = shY + SH_ROW;
  if (x < shX || x >= shX + shW || y < y0 || y >= y0 + n * SH_ROW) return -1;
  return (y - y0) / SH_ROW;
}
// A five-point star centred at cx,cy with outer radius r.
inline void starGlyph(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t px[10], py[10];
  for (uint8_t k = 0; k < 10; k++) {
    float a = -1.5708f + k * 0.62832f, rr = k % 2 ? r * 0.42f : r;
    px[k] = cx + (int16_t)lroundf(rr * cosf(a));
    py[k] = cy + (int16_t)lroundf(rr * sinf(a));
  }
  for (uint8_t k = 0; k < 10; k++) gfx->fillTriangle(cx, cy, px[k], py[k], px[(k + 1) % 10], py[(k + 1) % 10], c);
}
// "9:05 PM" or "21:05".
inline void clockStr(const struct tm &t, bool h24, char *out, size_t n) {
  if (h24) snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(out, n, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

// ── touch: tap, tap-up, long press, vertical drag, horizontal swipe ───────
enum class Gesture : uint8_t { None, Tap, TapUp, LongPress, Drag, SwipeRight, SwipeLeft, SwipeUp, SwipeDown };
inline const uint32_t TAP_MS = 100, LONG_MS = 700;
inline const int16_t AXIS_PX = 24, SWIPE_PX = 50;
inline bool tapUpQuick = false;
inline Gesture pollGesture(int16_t *x, int16_t *y, int16_t *dy) {
  static int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
  static uint32_t t0 = 0, lastTap = 0;
  static uint8_t axis = 0, gap = 0;
  static bool tapped = false, longed = false, dragging = false;
  int16_t cx, cy;
  bool contact = touchRead(&cx, &cy);
  if (contact) gap = 0;
  else if (touchHeld && ++gap < 3) return Gesture::None;
  bool now = contact;
  Gesture g = Gesture::None;
  if (now && !touchHeld) {
    x0 = lx = cx;
    y0 = ly = cy;
    t0 = millis();
    axis = 0;
    tapped = longed = dragging = false;
  } else if (now) {
    int16_t ddx = cx - x0, ddy = cy - y0;
    if (!axis && (abs(ddx) >= AXIS_PX || abs(ddy) >= AXIS_PX)) {
      if (abs(ddx) * 2 >= abs(ddy) * 3) axis = 1;
      else if (abs(ddy) * 2 >= abs(ddx) * 3) axis = 2;
    }
    if (axis == 2) {
      static float sy = 0;
      static int16_t applied = 0;
      if (!dragging) {
        sy = cy;
        applied = cy;
      }
      sy += (cy - sy) * 0.5f;
      int16_t target = (int16_t)lroundf(sy);
      *dy = abs(target - applied) >= 2 ? target - applied : 0;
      if (*dy) applied = target;
      dragging = true;
      g = Gesture::Drag;
    } else if (!axis && !tapped && millis() - t0 >= TAP_MS && millis() - lastTap > 150) {
      tapped = true;
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::Tap;
    } else if (!axis && tapped && !longed && millis() - t0 >= LONG_MS) {
      longed = true;
      *x = x0;
      *y = y0;
      g = Gesture::LongPress;
    }
    lx = cx;
    ly = cy;
  } else if (touchHeld) {
    int16_t ddx = lx - x0, ddy = ly - y0;
    if (axis == 1) {
      if (ddx >= SWIPE_PX) g = Gesture::SwipeRight;
      else if (ddx <= -SWIPE_PX) g = Gesture::SwipeLeft;
    } else if (axis == 2) {
      if (ddy >= SWIPE_PX) g = Gesture::SwipeDown;
      else if (ddy <= -SWIPE_PX) g = Gesture::SwipeUp;
    } else {
      tapUpQuick = !tapped;
      lastTap = millis();
      *x = x0;
      *y = y0;
      g = Gesture::TapUp;
    }
  }
  touchHeld = now;
  return g;
}

// ── GET a url over HTTP/1.0 into out; the length, or -1 ──────────────────
inline int fetchBytes(NetworkClientSecure &client, const char *url, const char *ua, uint8_t *out, size_t cap, const char *tag) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return -1;
  http.addHeader("User-Agent", ua);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s http %d\n", tag, code);
    http.end();
    return -1;
  }
  size_t len = 0;
  NetworkClient *st = http.getStreamPtr();
  uint32_t last = millis();
  while (millis() - last < 8000 && len < cap) {
    int n = st->available();
    if (n > 0) {
      n = st->read(out + len, min((size_t)n, cap - len));
      if (n > 0) {
        len += n;
        last = millis();
      }
    } else if (!st->connected()) {
      break;
    } else {
      delay(5);
    }
  }
  http.end();
  return (int)len;
}

// ── an on-screen keyboard, for the one secret an app needs typed once ──────
// kbOpen(title, buffer, cap) then kbDraw(); feed taps to kbTap(): 0 nothing,
// 1 the text changed (redraw the field), 2 Done, 3 the back mark. Four rows
// of ten 76px keys, a shift that lets go after one letter, a symbols layer,
// and a bottom row of shift, symbols, space, backspace, Done.
inline char *kbBuf = nullptr;
inline size_t kbCap = 0;
inline bool kbShift = false, kbSym = false;
inline const char *kbTitle = "";
inline const char *const KB_ROWS[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
inline const char *const KB_SYM[4] = {"!@#$%^&*()", "-_=+[]{}\\|", "~`;:'\",.<>", "/?"};
inline const int16_t KB_X0 = 20, KB_Y0 = 150, KB_W = 76, KB_H = 52;
inline void kbField() {
  gfx->fillRoundRect(20, 96, LCD_W - 40, 44, 8, towardsWhite(C_BG, 8));
  gfx->drawRoundRect(20, 96, LCD_W - 40, 44, 8, C_RULE);
  size_t len = strlen(kbBuf);
  const char *tail = kbBuf;
  while (textWidth(2, tail) > LCD_W - 80 && *tail) tail++;  // the end of a long key, not the start
  textAt(34, 96 + (44 - GH(2)) / 2, 2, C_FG, tail);
  int16_t cx = 34 + textWidth(2, tail) + 3;
  gfx->fillRect(cx, 104, 2, 28, C_GOLD);
  char n[16];
  snprintf(n, sizeof n, "%u", (unsigned)len);
  textAt(LCD_W - 34 - textWidth(1, n), 96 + (44 - GH(1)) / 2, 1, C_DIM, n);
}
inline void kbKey(int16_t x, int16_t y, int16_t w, const char *label, bool on = false) {
  gfx->fillRoundRect(x, y, w - 6, KB_H - 6, 8, on ? C_DIM : towardsWhite(C_BG, 10));
  gfx->drawRoundRect(x, y, w - 6, KB_H - 6, 8, C_RULE);
  textAt(x + (w - 6 - textWidth(2, label)) / 2, y + (KB_H - 6 - GH(2)) / 2, 2, C_FG, label);
}
inline void kbDrawKeys() {
  for (uint8_t r = 0; r < 4; r++) {
    const char *row = kbSym ? KB_SYM[r] : KB_ROWS[r];
    for (uint8_t c = 0; row[c]; c++) {
      char l[2] = {kbShift && !kbSym ? (char)toupper((unsigned char)row[c]) : row[c], 0};
      kbKey(KB_X0 + c * KB_W, KB_Y0 + r * KB_H, KB_W, l);
    }
    for (uint8_t c = strlen(row); c < 10; c++) gfx->fillRect(KB_X0 + c * KB_W, KB_Y0 + r * KB_H, KB_W, KB_H, C_BG);
  }
  int16_t y = KB_Y0 + 4 * KB_H;
  kbKey(KB_X0, y, 110, kbShift ? "SHIFT" : "shift", kbShift);
  kbKey(KB_X0 + 110, y, 110, kbSym ? "abc" : "#+=", kbSym);
  kbKey(KB_X0 + 220, y, 230, "space");
  kbKey(KB_X0 + 450, y, 120, "<-");
  gfx->fillRoundRect(KB_X0 + 570, y, 190 - 6, KB_H - 6, 8, C_GOLD);
  textAt(KB_X0 + 570 + (184 - textWidth(2, "Done")) / 2, y + (KB_H - 6 - GH(2)) / 2, 2, 0x0000, "Done");
}
inline void kbOpen(const char *title, char *buffer, size_t cap) {
  kbTitle = title;
  kbBuf = buffer;
  kbCap = cap;
  kbShift = kbSym = false;
}
inline void kbDraw(const char *note = nullptr) {
  pageHeader(kbTitle, note);
  kbField();
  kbDrawKeys();
  drawHint("tap Done when it is all in        < back");
}
inline uint8_t kbTap(int16_t x, int16_t y) {
  if (y < UI_Y_ROW0 && x < 140) return 3;
  if (y < KB_Y0 || x < KB_X0 || x >= KB_X0 + 10 * KB_W) return 0;
  uint8_t r = (y - KB_Y0) / KB_H;
  size_t len = strlen(kbBuf);
  if (r < 4) {
    const char *row = kbSym ? KB_SYM[r] : KB_ROWS[r];
    uint8_t c = (x - KB_X0) / KB_W;
    if (c >= strlen(row)) return 0;
    if (len + 1 >= kbCap) return 0;
    char ch = kbShift && !kbSym ? (char)toupper((unsigned char)row[c]) : row[c];
    kbBuf[len] = ch;
    kbBuf[len + 1] = '\0';
    if (kbShift) { kbShift = false; kbDrawKeys(); }
    kbField();
    return 1;
  }
  if (r != 4) return 0;
  int16_t rx = x - KB_X0;
  if (rx < 110) { kbShift = !kbShift; kbDrawKeys(); return 0; }
  if (rx < 220) { kbSym = !kbSym; kbDrawKeys(); return 0; }
  if (rx < 450) { if (len + 1 < kbCap) { kbBuf[len] = ' '; kbBuf[len + 1] = '\0'; kbField(); return 1; } return 0; }
  if (rx < 570) { if (len) { kbBuf[len - 1] = '\0'; kbField(); return 1; } return 0; }
  return 2;
}
