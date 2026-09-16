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
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#define LCD_W 800
#define LCD_H 480

// Calibration knobs for the RGB timing. Both sources run 16MHz with an
// inverted pixel clock; that flickered here even on a still page, so 12MHz
// (~28 frames a second, plenty for a ticker) asks a quarter less of the
// PSRAM the panel scans out of. If it still flickers, go lower; if the
// colours are mirrored (cyan draws yellow), swap the R and B pin groups.
#define LCD_PCLK_HZ 12000000
#define LCD_PCLK_NEG 1

// The panel DMA reads its framebuffer from PSRAM; whenever the CPU competes
// for it (a full-width scroll, Wi-Fi, a flash cache miss) the DMA starves
// and the picture glitches. A bounce buffer in internal RAM is Espressif's
// fix: the DMA reads SRAM and an interrupt refills it in bursts. 20 lines:
// 2 x 32KB of SRAM, and worth every byte on a 768KB framebuffer.
#define LCD_BOUNCE_PX (LCD_W * 20)

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
// The framebuffer is OURS: an Arduino_Canvas whose 768KB buffer lives in
// PSRAM, and an esp_lcd RGB panel created with no framebuffer of its own
// (no_fb) that asks for every chunk of scan-out through on_bounce_empty.
// That callback copies lines from our buffer into the driver's bounce
// buffer in internal RAM -- exactly what the driver does itself when it
// owns the framebuffer -- except that it maps the lines of one region
// through a ring offset. That gives an app a hardware-style vertical
// scroll over a band of the screen (the CYD's ILI9341 has one; a bare RGB
// panel does not) for the price of writing one number: no memmove of
// 700KB per pixel, which is what starved the scan-out into flicker.
namespace boarddetail {
inline uint16_t *fb = nullptr;
inline esp_lcd_panel_handle_t panelHandle = nullptr;
inline volatile int16_t scrollY0 = 0, scrollH = 0, scrollOff = 0;

// Runs in the LCD DMA interrupt: one bounce buffer (LCD_BOUNCE_PX pixels,
// whole lines) to fill from our framebuffer. static, not inline: an inline
// IRAM_ATTR function in a header trips the Xtensa linker.
static bool IRAM_ATTR onBounceEmpty(esp_lcd_panel_handle_t, void *buf, int pos_px, int len_bytes, void *) {
  uint16_t *dst = (uint16_t *)buf;
  int line = pos_px / LCD_W, n = len_bytes / (LCD_W * 2);
  int16_t y0 = scrollY0, h = scrollH, off = scrollOff;
  for (int i = 0; i < n; i++, line++) {
    int src = line;
    if (h > 0 && line >= y0 && line < y0 + h) {
      src = line - y0 + off;
      if (src >= h) src -= h;
      src += y0;
    }
    memcpy(dst + (size_t)i * LCD_W, fb + (size_t)src * LCD_W, LCD_W * 2);
  }
  return false;
}

inline bool panelBegin() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = LCD_W;
  cfg.timings.v_res = LCD_H;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 16;
  cfg.timings.vsync_front_porch = 16;
  cfg.timings.flags.hsync_idle_low = 1;
  cfg.timings.flags.vsync_idle_low = 1;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = LCD_PCLK_NEG;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 0;
  cfg.bounce_buffer_size_px = LCD_BOUNCE_PX;
  cfg.hsync_gpio_num = 46;
  cfg.vsync_gpio_num = 3;
  cfg.de_gpio_num = 5;
  cfg.pclk_gpio_num = 7;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  const int pins[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};  // B0-4, G0-5, R0-4
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = pins[i];
  cfg.flags.disp_active_low = 1;
  cfg.flags.no_fb = 1;
  if (esp_lcd_new_rgb_panel(&cfg, &panelHandle) != ESP_OK) return false;
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_bounce_empty = onBounceEmpty;
  esp_lcd_rgb_panel_register_event_callbacks(panelHandle, &cbs, nullptr);
  if (esp_lcd_panel_reset(panelHandle) != ESP_OK) return false;
  return esp_lcd_panel_init(panelHandle) == ESP_OK;
}
}  // namespace boarddetail

// An Arduino_Canvas that puts its buffer in PSRAM and starts the panel
// scanning it. Everything an app draws lands in the buffer directly.
class PsramCanvas : public Arduino_Canvas {
 public:
  PsramCanvas() : Arduino_Canvas(LCD_W, LCD_H, nullptr) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override {
    if (!_framebuffer) _framebuffer = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    if (!_framebuffer) return false;
    memset(_framebuffer, 0, (size_t)LCD_W * LCD_H * 2);
    boarddetail::fb = _framebuffer;
    return boarddetail::panelBegin();
  }
};
inline Arduino_GFX *boardDisplay() {
  static PsramCanvas gfx;
  return &gfx;
}
inline uint16_t *boardFramebuffer() { return boarddetail::fb; }
// The scroll band: screen lines y0..y0+h show buffer lines rotated by off.
inline void boardScrollArea(int16_t y0, int16_t h) {
  boarddetail::scrollY0 = y0;
  boarddetail::scrollH = h;
}
inline void boardScroll(int16_t off) { boarddetail::scrollOff = off; }

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
