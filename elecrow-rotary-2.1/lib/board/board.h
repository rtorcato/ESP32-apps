// Elecrow CrowPanel 2.1" HMI ESP32-S3 Rotary Display -- board pins and setup.
//
// Pinout taken from Elecrow's factory sketch (github.com/Elecrow-RD/
// CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen)
// and cross-checked against the wiki and a working ESPHome config. Beware the
// "Simple example/Encoder_code.ino" in that same repo: it uses 45/42/41, which
// is a different board's knob.
//
// Unlike the C6, the housekeeping lines (LCD power, LCD reset, touch reset and
// interrupt, the knob's push switch) are NOT GPIOs. They sit behind a PCF8574
// I2C expander at 0x21, so nothing draws until the expander has been talked to.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ST7701, 480x480 round. Pixels go over a 16-bit RGB parallel bus; the panel's
// register init goes over a separate 3-wire SPI (no DC line, no MISO).
#define LCD_W 480
#define LCD_H 480
#define LCD_BL 6
#define LCD_SPI_CS 16
#define LCD_SPI_SCK 2
#define LCD_SPI_SDA 1

// Calibration knobs for the RGB timing. Verified on this unit 2026-09-15:
// non-inverted pixel clock at 16MHz. Inverted (what ESPHome's config for this
// board says) draws the circle fine but mangles every thin stroke -- text
// turns to noise -- because the panel latches data on the wrong edge. If a
// future unit shows shifting or tearing, lower the clock (Arduino_GFX's own
// type5 example runs 12MHz) before touching anything else.
#define LCD_PCLK_NEG 0
#define LCD_PCLK_HZ 16000000

#define I2C_SDA 38
#define I2C_SCL 39
#define PCF8574_ADDR 0x21
#define CST816_ADDR 0x15

// PCF8574 bit numbers (P0..P7).
#define X_TP_RST 0
#define X_TP_INT 2
#define X_LCD_PWR 3
#define X_LCD_RST 4
#define X_KNOB_SW 5  // active low

#define ENC_A 42
#define ENC_B 4
#define BTN_BOOT 0  // strapping pin, input only in practice

// --- PCF8574 -----------------------------------------------------------------
// Quasi-bidirectional: a bit written 1 is a weak pull-up that also serves as an
// input, so the shadow byte keeps every input bit high. A library for one
// byte in and one byte out is not worth the dependency.
namespace boarddetail {
inline uint8_t xShadow = 0xFF;
}

inline bool xWrite(uint8_t bit, bool hi) {
  if (hi) boarddetail::xShadow |= (1 << bit);
  else boarddetail::xShadow &= ~(1 << bit);
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(boarddetail::xShadow);
  return Wire.endTransmission() == 0;
}

inline bool xRead(uint8_t bit) {
  if (Wire.requestFrom((uint8_t)PCF8574_ADDR, (uint8_t)1) != 1) return true;  // absent reads as released
  return (Wire.read() >> bit) & 1;
}

inline bool knobPressed() { return !xRead(X_KNOB_SW); }

// Power and reset sequence, verbatim timings from the factory sketch. Returns
// whether the expander acknowledged, which is the first thing to check when the
// screen stays dark. Call before boardDisplay()->begin().
inline bool boardBegin() {
  // Native USB CDC blocks on every print, 100ms at a time, once the host has
  // opened the port and stopped reading (a closed monitor, a finished script)
  // -- "host backpressure" in HWCDC.cpp. With a print per encoder click that
  // turned the knob to treacle. Zero timeout: drop the line, keep the loop.
  Serial.setTxTimeoutMs(0);
  Wire.begin(I2C_SDA, I2C_SCL);
  bool ok = xWrite(X_LCD_PWR, true);
  delay(100);
  xWrite(X_LCD_RST, true);  delay(100);
  xWrite(X_LCD_RST, false); delay(120);
  xWrite(X_LCD_RST, true);  delay(120);
  xWrite(X_TP_RST, true);   delay(100);
  xWrite(X_TP_RST, false);  delay(120);
  xWrite(X_TP_RST, true);   delay(120);
  xWrite(X_TP_INT, true);   delay(120);
  return ok;
}

// --- Display -----------------------------------------------------------------
// Porch and pulse values are the factory sketch's. The framebuffer (480*480*2 =
// 460KB) is allocated in PSRAM by Arduino_RGB_Display::begin(); every draw call
// writes straight into it and the panel DMA scans it out continuously.
inline Arduino_GFX *boardDisplay() {
  static Arduino_SWSPI spi(GFX_NOT_DEFINED /* DC */, LCD_SPI_CS, LCD_SPI_SCK, LCD_SPI_SDA, GFX_NOT_DEFINED);
  static Arduino_ESP32RGBPanel panel(
      40 /* DE */, 7 /* VSYNC */, 15 /* HSYNC */, 41 /* PCLK */,
      // The panel is wired BGR: the factory sketch passed a `bgr = true` flag
      // that the 1.6 API dropped. Feeding the B pins as R and vice versa is the
      // same fix with no custom init sequence. (Cyan drew yellow before this.)
      5 /* R0 = panel B0 */, 45 /* R1 */, 48 /* R2 */, 47 /* R3 */, 21 /* R4 */,
      14 /* G0 */, 13 /* G1 */, 12 /* G2 */, 11 /* G3 */, 10 /* G4 */, 9 /* G5 */,
      46 /* B0 = panel R0 */, 3 /* B1 */, 8 /* B2 */, 18 /* B3 */, 17 /* B4 */,
      1 /* hsync_polarity */, 10 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 20 /* hsync_back_porch */,
      1 /* vsync_polarity */, 10 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
      LCD_PCLK_NEG, LCD_PCLK_HZ);
  static Arduino_RGB_Display gfx(LCD_W, LCD_H, &panel, 0 /* rotation */, true /* auto_flush */, &spi,
                                 GFX_NOT_DEFINED /* RST: via PCF8574 */, st7701_type5_init_operations,
                                 sizeof(st7701_type5_init_operations));
  return &gfx;
}

// Backlight on PWM. 0-255. Factory default is 204.
inline void backlight(uint8_t level) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    attached = true;
  }
  ledcWrite(LCD_BL, level);
}

// --- Touch (CST816, raw I2C) ---------------------------------------------------
// Registers: 0x02 finger count, 0x03/0x04 X (12-bit), 0x05/0x06 Y (12-bit).
// Enough for "is a finger down, and where"; gestures are the app's problem.
inline bool touchRead(int16_t *x, int16_t *y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)5) != 5) return false;
  uint8_t n = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (n == 0) return false;
  *x = ((xh & 0x0F) << 8) | xl;
  *y = ((yh & 0x0F) << 8) | yl;
  return true;
}

// --- Encoder -------------------------------------------------------------------
// Interrupt-driven quadrature with a state table, not "read A, check B" in
// loop(): the factory sketch polls and drops steps whenever the UI is busy.
//
// This knob rests at BOTH stable states (A=B=1 and A=B=0) -- two detents per
// electrical cycle, which is why ESPHome's config says resolution: 2. Each
// detent is therefore two valid edges. A bounce is +1 then -1, netting zero,
// and an invalid transition (both lines flipping at once) scores zero: that is
// the whole debounce.
struct Enc {
  uint8_t last = 3;
  int8_t acc = 0;
  int32_t pos = 0;
};

// Pure, so the self-check can drive it. cur = (A << 1) | B.
inline void encFeed(Enc *e, uint8_t cur) {
  // index = (prev << 2) | cur. Sign chosen so clockwise counts up on this
  // knob (checked against the factory sketch and by hand, 2026-09-15).
  static const int8_t step[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
  e->acc += step[(e->last << 2) | cur];
  e->last = cur;
  if (cur == 3 || cur == 0) {  // at a detent: settle the count
    if (e->acc >= 2) e->pos++;
    else if (e->acc <= -2) e->pos--;
    e->acc = 0;
  }
}

namespace boarddetail {
inline volatile Enc enc;
// static, not inline: an inline IRAM_ATTR function in a header trips the Xtensa
// linker ("l32r: literal placed after use").
static void IRAM_ATTR encIsr() {
  uint8_t cur = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  encFeed((Enc *)&enc, cur);
}
}  // namespace boarddetail

inline void encoderBegin() {
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  boarddetail::enc.last = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A), boarddetail::encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), boarddetail::encIsr, CHANGE);
}

// Detents since boot. Positive = clockwise.
inline int32_t encoderPosition() { return boarddetail::enc.pos; }

// The decode is the only real logic in this file, so it gets the one check.
inline void encSelfCheck() {
  Enc e;
  // Two full detents clockwise: 3 -> 2 -> 0 -> 1 -> 3 (Gray code, one line per edge)
  for (uint8_t s : {2, 0, 1, 3}) encFeed(&e, s);
  assert(e.pos == 2);
  // Back the same way: 3 -> 1 -> 0 -> 2 -> 3
  for (uint8_t s : {1, 0, 2, 3}) encFeed(&e, s);
  assert(e.pos == 0);
  // A bounce on one line does nothing.
  for (uint8_t s : {2, 3, 2, 3}) encFeed(&e, s);
  assert(e.pos == 0);
  // An impossible jump (both lines at once) is ignored rather than counted.
  encFeed(&e, 0);
  assert(e.pos == 0 && e.acc == 0);
  // The 0 rest state is a detent too: one more from here counts.
  for (uint8_t s : {1, 3}) encFeed(&e, s);
  assert(e.pos == 1);
}
