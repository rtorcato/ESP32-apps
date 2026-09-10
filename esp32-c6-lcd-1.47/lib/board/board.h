// Waveshare ESP32-C6-LCD-1.47 -- board pins and display setup.
// Pinout verified against the Waveshare wiki and the Spotpear mirror.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// LCD (ST7789, 172x320 IPS). No MISO -- the panel has no read line.
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS   14
#define LCD_DC   15
#define LCD_RST  21
#define LCD_BL   22
#define LCD_W    172
#define LCD_H    320

// microSD shares MOSI/SCLK with the LCD -- only one CS may be low at a time.
#define SD_MISO 5
#define SD_CS   4

#define RGB_LED  8
#define BTN_BOOT 9  // active low, also the strapping pin -- do not drive it

// The 172px-wide panel sits on a 240-wide ST7789 controller, so both column
// offsets must be 34 or every pixel lands 34px to the left.
inline Arduino_GFX *boardDisplay() {
  static Arduino_ESP32SPI bus(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
  static Arduino_ST7789 gfx(&bus, LCD_RST, 0 /* rotation */, true /* IPS */,
                            LCD_W, LCD_H, 34, 0, 34, 0);
  return &gfx;
}

// Backlight on PWM so apps can dim rather than just toggle. 0-255.
inline void backlight(uint8_t level) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    attached = true;
  }
  ledcWrite(LCD_BL, level);
}

// ponytail: core 3.x drives the WS2812 natively, so no NeoPixel/FastLED dep.
// The LED is bright enough to be annoying; scale values down, not up.
inline void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_LED, r, g, b); }
