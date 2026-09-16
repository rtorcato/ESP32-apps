// Waveshare ESP32-S3-Touch-LCD-7 -- board pins and setup.
//
// 7" 800x480 ST7262 on a 16-bit RGB parallel bus (no register init: it is a
// bare TTL panel, so there is no SPI side channel and no init sequence),
// GT911 capacitive touch over I2C, and a CH422G I/O expander because the RGB
// bus eats the GPIOs: the backlight, the panel reset and the touch reset are
// expander pins, so nothing lights until the expander has been talked to.
//
// Pinout from Espressif's ESP32_Display_Panel board file for this device
// (src/board/waveshare/ESP32_S3_Touch_LCD_7.h, v0.2.1) and the ESPHome
// package for it (github.com/inytar/waveshare-esp32-s3-touch-lcd-7-esphome),
// which agree on every GPIO. Not yet verified by eye on this unit.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#define LCD_W 800
#define LCD_H 480

// Calibration knobs for the RGB timing. 16MHz with an inverted pixel clock is
// what both sources run. If the picture shifts or tears, lower the clock
// first; if it is mirrored in colour (cyan draws yellow), swap the R and B
// pin groups below, as the rotary needed.
#define LCD_PCLK_HZ 16000000
#define LCD_PCLK_NEG 1

// The panel DMA reads its framebuffer from PSRAM; a CPU flooding PSRAM (a
// full-width scroll) starves it and the picture glitches. A bounce buffer
// in internal RAM is Espressif's fix. 10 lines: 2 x 16KB of SRAM.
#define LCD_BOUNCE_PX (LCD_W * 10)

#define I2C_SDA 8
#define I2C_SCL 9
#define TP_INT 4  // GT911 interrupt, low while a finger is down; RTC-capable, so it can wake the chip

// CH422G: a strange little expander with a register per I2C address.
#define CH422G_MODE 0x24  // write 0x01: EXIO0-7 are push-pull outputs
#define CH422G_OUT 0x38   // write: one bit per EXIO pin
#define X_TP_RST 1
#define X_LCD_BL 2   // on/off only: this board has no PWM on the backlight
#define X_LCD_RST 3
#define X_SD_CS 4
#define X_USB_SEL 5  // LOW keeps the USB-C on the chip's own USB; high hands it to the CAN transceiver

#define GT911_ADDR 0x5D  // 0x14 on units whose INT was high at reset; probed at begin
#define BTN_BOOT 0

// --- CH422G ------------------------------------------------------------------
namespace boarddetail {
inline uint8_t xShadow = 0;
inline uint8_t gtAddr = GT911_ADDR;
}

inline bool xWrite(uint8_t bit, bool hi) {
  if (hi) boarddetail::xShadow |= (1 << bit);
  else boarddetail::xShadow &= ~(1 << bit);
  Wire.beginTransmission(CH422G_OUT);
  Wire.write(boarddetail::xShadow);
  return Wire.endTransmission() == 0;
}

// Backlight is a switch here, not a level: anything above 0 is on.
inline void backlight(uint8_t level) { xWrite(X_LCD_BL, level > 0); }

// Expander to output mode, backlight off, both resets pulsed. Returns whether
// the expander acknowledged -- the first thing to check when the screen
// stays dark. Call before boardDisplay()->begin().
inline bool boardBegin() {
  // Serial is UART0 through the board's CH343 bridge ("USB Single Serial" on
  // the host): no CDC backpressure to guard against, unlike the native-USB boards.
  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(TP_INT, INPUT);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Wire.beginTransmission(CH422G_MODE);
  Wire.write(0x01);
  bool ok = Wire.endTransmission() == 0;
  // SD chip-select idle high; USB_SEL LOW -- high switched the USB-C over to
  // the CAN side and the serial monitor turned to garbage mid-boot.
  boarddetail::xShadow = (1 << X_SD_CS);
  xWrite(X_LCD_RST, false);
  xWrite(X_TP_RST, false);
  delay(20);
  xWrite(X_LCD_RST, true);
  delay(120);
  xWrite(X_TP_RST, true);
  delay(120);
  // GT911 answers at 0x5D or 0x14 depending on INT at reset; take whichever acks.
  for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      boarddetail::gtAddr = a;
      break;
    }
  }
  return ok;
}

// --- Display -----------------------------------------------------------------
// The framebuffer (800*480*2 = 768KB) is allocated in PSRAM by
// Arduino_RGB_Display::begin(); every draw call writes straight into it and
// the panel DMA scans it out continuously. No bus, no init operations: a
// bare RGB panel.
inline Arduino_GFX *boardDisplay() {
  static Arduino_ESP32RGBPanel panel(
      5 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 7 /* PCLK */,
      1 /* R0 */, 2 /* R1 */, 42 /* R2 */, 41 /* R3 */, 40 /* R4 */,
      39 /* G0 */, 0 /* G1 */, 45 /* G2 */, 48 /* G3 */, 47 /* G4 */, 21 /* G5 */,
      14 /* B0 */, 38 /* B1 */, 18 /* B2 */, 17 /* B3 */, 10 /* B4 */,
      0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
      0 /* vsync_polarity */, 16 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 16 /* vsync_back_porch */,
      LCD_PCLK_NEG, LCD_PCLK_HZ, false /* useBigEndian */, 0 /* de_idle_high */, 0 /* pclk_idle_high */,
      LCD_BOUNCE_PX);
  static Arduino_RGB_Display gfx(LCD_W, LCD_H, &panel, 0 /* rotation */, true /* auto_flush */);
  return &gfx;
}

// --- Touch (GT911, raw I2C) --------------------------------------------------
// 0x814E: bit 7 says a report is ready, low nibble is the finger count; the
// first point sits at 0x8150 (x lo, x hi, y lo, y hi). The status byte must
// be cleared after a read or the chip holds the same report. Capacitive, so
// no calibration: coordinates are panel pixels. First finger only.
// Reports come every ~10ms while a finger is down, and one last report with
// no fingers when it lifts; between reports the last state stands, so a
// caller polling every pass sees a steady "down at x,y" rather than a
// flicker of misses.
namespace boarddetail {
inline bool touched = false;
inline int16_t tx = 0, ty = 0;
}
inline bool touchRead(int16_t *x, int16_t *y) {
  *x = boarddetail::tx;
  *y = boarddetail::ty;
  Wire.beginTransmission(boarddetail::gtAddr);
  Wire.write(0x81);
  Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0) return boarddetail::touched;
  if (Wire.requestFrom(boarddetail::gtAddr, (uint8_t)1) != 1) return boarddetail::touched;
  uint8_t st = Wire.read();
  if (!(st & 0x80)) return boarddetail::touched;  // nothing new: the last report stands
  uint8_t n = st & 0x0F;
  bool down = false;
  if (n) {
    Wire.beginTransmission(boarddetail::gtAddr);
    Wire.write(0x81);
    Wire.write(0x50);
    Wire.endTransmission(false);
    if (Wire.requestFrom(boarddetail::gtAddr, (uint8_t)4) == 4) {
      uint8_t xl = Wire.read(), xh = Wire.read(), yl = Wire.read(), yh = Wire.read();
      *x = boarddetail::tx = (int16_t)((xh << 8) | xl);
      *y = boarddetail::ty = (int16_t)((yh << 8) | yl);
      down = true;
    }
  }
  Wire.beginTransmission(boarddetail::gtAddr);  // clear the report
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(0);
  Wire.endTransmission();
  boarddetail::touched = down;
  return down;
}
inline bool touchDown() { return boarddetail::touched; }

inline bool bootPressed() { return digitalRead(BTN_BOOT) == LOW; }
