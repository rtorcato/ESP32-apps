// Smoke test: proves the expander, panel timing, backlight, encoder, knob
// switch and touch all work. If the white circle hugs the bezel all the way
// round, the RGB timing is right.
#include <appcfg.h>
#include <board.h>

static Arduino_GFX *gfx;

// Same convention as every app in this repo: apps/<app>/data/config.json,
// every key optional, compiled defaults with the file absent.
static uint8_t bl = 204;  // factory default, bright: this screen is for checking pixels
static uint16_t tickMs = 500;

// Exact-box field helper. Built-in font: 6*size x 8*size per glyph, so a box
// sized by character count is exact and nothing outside it is touched.
static void field(int16_t x, int16_t y, uint8_t size, uint8_t chars, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, 6 * size * chars, 8 * size, RGB565_BLACK);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void setup() {
  Serial.begin(115200);

  // Order matters: expander first (LCD power + reset live behind it), then the
  // panel, then the backlight so the first frame you see is a drawn one.
  bool xok = boardBegin();
  gfx = boardDisplay();
  bool gok = gfx->begin();

  cfgSelfCheck();
  encSelfCheck();
  if (cfgLoad()) {
    bl = (uint8_t)cfgInt("brightness", bl, 8, 255);
    tickMs = (uint16_t)cfgInt("tickMs", tickMs, 50, 5000);
    cfgRelease();
  }
  Serial.printf("config: brightness %u, tick %ums\n", bl, tickMs);
  Serial.printf("pcf8574 %s, panel %s, psram %u KB free\n", xok ? "OK" : "MISSING", gok ? "OK" : "FAILED",
                ESP.getFreePsram() / 1024);

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(LCD_W / 2, LCD_H / 2, LCD_W / 2 - 1, RGB565_WHITE);  // must touch the bezel all round
  field(123, 120, 3, 13, RGB565_WHITE, "CrowPanel 2.1");
  field(177, 152, 3, 7, RGB565_WHITE, "480x480");
  field(132, 196, 2, 18, RGB565_LIGHTGREY, "turn knob / press");
  field(150, 344, 2, 15, xok ? RGB565_LIGHTGREY : RGB565_RED, xok ? "pcf8574 ok" : "pcf8574 MISSING");
  backlight(bl);

  encoderBegin();
}

void loop() {
  static int32_t lastPos = INT32_MIN;
  static bool lastPressed = false;
  static int16_t tx = -1, ty = -1;
  static uint32_t lastTick = 0;

  int32_t pos = encoderPosition();
  if (pos != lastPos) {
    char s[8];
    snprintf(s, sizeof s, "%6ld", (long)pos);
    field(150, 230, 5, 6, RGB565_CYAN, s);
    Serial.printf("knob %ld\n", (long)pos);
    lastPos = pos;
  }

  bool pressed = knobPressed();
  if (pressed != lastPressed) {
    gfx->fillRect(140, 300, 200, 28, pressed ? RGB565_GREEN : RGB565_DARKGREY);
    Serial.printf("press %d\n", pressed);
    lastPressed = pressed;
  }

  // Touch: a dot follows the finger, and the previous one is erased. Anything
  // it leaves behind on the circle or text is cosmetic -- it's a smoke test.
  int16_t x, y;
  if (touchRead(&x, &y)) {
    if (x != tx || y != ty) {
      if (tx >= 0) gfx->fillCircle(tx, ty, 6, RGB565_BLACK);
      gfx->fillCircle(x, y, 6, RGB565_YELLOW);
      Serial.printf("touch %d,%d\n", x, y);
      tx = x; ty = y;
    }
  }

  if (millis() - lastTick >= tickMs) {
    lastTick = millis();
    Serial.printf("tick %lu knob=%ld press=%d\n", millis(), (long)pos, pressed);
  }
  delay(10);  // input latency budget; the encoder itself is on interrupts
}
