// DIYmalls ESP32-2432S028R "Cheap Yellow Display" -- board pins and setup.
//
// Pinout from the community repo (github.com/witnessmenow/ESP32-Cheap-Yellow-
// Display, PINS.md) and Arduino_GFX's own ESP32_2432S028 dev-device entry.
// Verified on this unit by apps/hello -- see the README for what was checked.
//
// Unlike the rotary there is no expander: everything is a plain GPIO. Three
// SPI devices, three separate pin sets: display on HSPI (IOMUX pins, 40MHz),
// SD on the VSPI pins, touch bit-banged on its own pins.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define LCD_W 240
#define LCD_H 320
#define LCD_DC 2
#define LCD_CS 15
#define LCD_SCK 14
#define LCD_MOSI 13
#define LCD_MISO 12
#define LCD_BL 21
// Calibration knob: the single-USB ESP32-2432S028R is an ILI9341. The two-USB
// revision of the same listing ships an ST7789 that wants inverted colours --
// if the screen comes up washed-out white with the right shapes, set this 1.
#define LCD_ST7789 0

// XPT2046 resistive touch. Own pins, not a hardware SPI host.
#define TP_CLK 25
#define TP_MOSI 32
#define TP_MISO 39  // input-only pin
#define TP_CS 33
#define TP_IRQ 36   // input-only, low while pressed

// microSD on the VSPI IOMUX pins. Not used by hello; SD.begin(SD_CS) with a
// SPIClass(VSPI) is all it needs.
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5

#define LED_R 4    // all three active low
#define LED_G 16
#define LED_B 17
#define LDR 34     // analog, input-only; darker = higher reading
#define SPK 26     // speaker header, DAC-capable
#define BTN_BOOT 0

// --- Display -----------------------------------------------------------------
// The bus is reachable on its own for panel commands Arduino_GFX has no API
// for (ticker uses the ILI9341's hardware vertical scroll, 0x33/0x37).
inline Arduino_DataBus *boardBus() {
  static Arduino_ESP32SPI bus(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO, HSPI);
  return &bus;
}
inline Arduino_GFX *boardDisplay() {
#if LCD_ST7789
  static Arduino_ST7789 gfx(boardBus(), GFX_NOT_DEFINED /* RST tied to EN */, 0, true /* IPS */, LCD_W, LCD_H);
#else
  static Arduino_ILI9341 gfx(boardBus(), GFX_NOT_DEFINED /* RST tied to EN */, 0);
#endif
  return &gfx;
}

// Backlight on PWM, 0-255.
inline void backlight(uint8_t level) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    attached = true;
  }
  ledcWrite(LCD_BL, level);
}

// --- RGB LED -----------------------------------------------------------------
inline void boardBegin() {
  for (uint8_t p : {LED_R, LED_G, LED_B}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);  // off
  }
  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(TP_IRQ, INPUT);
  pinMode(TP_CLK, OUTPUT);
  pinMode(TP_MOSI, OUTPUT);
  pinMode(TP_MISO, INPUT);
  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  digitalWrite(TP_CLK, LOW);
}

inline void led(bool r, bool g, bool b) {
  digitalWrite(LED_R, !r);
  digitalWrite(LED_G, !g);
  digitalWrite(LED_B, !b);
}

inline bool bootPressed() { return digitalRead(BTN_BOOT) == LOW; }

// --- Touch (XPT2046, bit-banged) -----------------------------------------------
// ponytail: bit-banged SPI mode 0 at digitalWrite speed (~1MHz), which the
// chip is happy with and leaves VSPI free for the SD card. Move to a
// SPIClass if a third SPI host ever appears, which on this chip it won't.
//
// Raw ADC range is panel-specific: the community defaults (200-3700 in X,
// 240-3800 in Y) are the starting point and apps/hello/data/config.json
// overrides them. Tune until the dot sits under the stylus in all four corners.
struct TouchCal {
  uint16_t xMin = 200, xMax = 3700, yMin = 240, yMax = 3800;
};
inline TouchCal touchCal;

namespace boarddetail {
inline uint16_t xptXfer(uint8_t cmd) {
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    digitalWrite(TP_CLK, HIGH);
    digitalWrite(TP_CLK, LOW);
  }
  uint16_t v = 0;
  for (uint8_t i = 0; i < 16; i++) {  // 1 busy clock, 12 data bits, padding
    digitalWrite(TP_CLK, HIGH);
    v = (v << 1) | digitalRead(TP_MISO);
    digitalWrite(TP_CLK, LOW);
  }
  return (v >> 3) & 0x0FFF;
}
}  // namespace boarddetail

// Raw 12-bit ADC values, no calibration. False when nothing is pressing.
inline bool touchRaw(uint16_t *x, uint16_t *y) {
  if (digitalRead(TP_IRQ) == HIGH) return false;
  digitalWrite(TP_CS, LOW);
  // Read each axis twice and keep the second: the first sample after switching
  // the multiplexer is the noisy one.
  boarddetail::xptXfer(0xD0);
  *x = boarddetail::xptXfer(0xD0);
  boarddetail::xptXfer(0x90);
  *y = boarddetail::xptXfer(0x90);
  digitalWrite(TP_CS, HIGH);
  return true;
}

// Pure, so the self-check can drive it. Raw -> screen for rotation 0.
inline void touchMap(const TouchCal &c, uint16_t rx, uint16_t ry, int16_t *x, int16_t *y) {
  long mx = map(constrain(rx, c.xMin, c.xMax), c.xMin, c.xMax, 0, LCD_W - 1);
  long my = map(constrain(ry, c.yMin, c.yMax), c.yMin, c.yMax, 0, LCD_H - 1);
  *x = (int16_t)mx;
  *y = (int16_t)my;
}

// Screen coordinates for rotation 0 (portrait, 240x320).
inline bool touchRead(int16_t *x, int16_t *y) {
  uint16_t rx, ry;
  if (!touchRaw(&rx, &ry)) return false;
  touchMap(touchCal, rx, ry, x, y);
  return true;
}

inline void touchSelfCheck() {
  TouchCal c;
  int16_t x, y;
  touchMap(c, c.xMin, c.yMin, &x, &y);
  assert(x == 0 && y == 0);
  touchMap(c, c.xMax, c.yMax, &x, &y);
  assert(x == LCD_W - 1 && y == LCD_H - 1);
  touchMap(c, 0, 4095, &x, &y);  // out of range clamps, never wraps
  assert(x == 0 && y == LCD_H - 1);
}
