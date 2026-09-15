// Smoke test: proves the panel, backlight, touch, RGB LED, LDR, BOOT button
// and speaker header all work. If the white frame hugs the bezel on all four
// sides, the panel driver and offsets are right.
#include <appcfg.h>
#include <board.h>

static Arduino_GFX *gfx;

static uint8_t bl = 255;
static uint16_t tickMs = 500;

// Exact-box field helper. Built-in font: 6*size x 8*size per glyph.
static void field(int16_t x, int16_t y, uint8_t size, uint8_t chars, uint16_t fg, const char *s) {
  gfx->fillRect(x, y, 6 * size * chars, 8 * size, RGB565_BLACK);
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// No framebuffer on an SPI panel, so the touch dot lives in its own empty box
// and is erased to black. Text never goes inside PAD_*.
static const int16_t PAD_Y0 = 160, PAD_Y1 = 300, DOT_R = 5;
static int16_t dotX = -1, dotY = -1;

static void dotHide() {
  if (dotX < 0) return;
  gfx->fillCircle(dotX, dotY, DOT_R, RGB565_BLACK);
  dotX = -1;
}

static void dotShow(int16_t x, int16_t y) {
  dotX = constrain(x, 2 + DOT_R, LCD_W - 3 - DOT_R);
  dotY = constrain(y, PAD_Y0 + 2 + DOT_R, PAD_Y1 - 3 - DOT_R);
  gfx->fillCircle(dotX, dotY, DOT_R, RGB565_YELLOW);
}

void setup() {
  Serial.begin(115200);
  boardBegin();
  gfx = boardDisplay();
  bool gok = gfx->begin();

  cfgSelfCheck();
  touchSelfCheck();
  if (cfgLoad()) {
    bl = (uint8_t)cfgInt("brightness", bl, 8, 255);
    tickMs = (uint16_t)cfgInt("tickMs", tickMs, 50, 5000);
    touchCal.xMin = cfgInt("touch.xMin", touchCal.xMin, 0, 4095);
    touchCal.xMax = cfgInt("touch.xMax", touchCal.xMax, 0, 4095);
    touchCal.yMin = cfgInt("touch.yMin", touchCal.yMin, 0, 4095);
    touchCal.yMax = cfgInt("touch.yMax", touchCal.yMax, 0, 4095);
    cfgRelease();
  }
  Serial.printf("config: brightness %u, tick %ums, touch x %u-%u y %u-%u\n", bl, tickMs, touchCal.xMin,
                touchCal.xMax, touchCal.yMin, touchCal.yMax);
  Serial.printf("panel %s, heap %u KB free\n", gok ? "OK" : "FAILED", ESP.getFreeHeap() / 1024);

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, LCD_W, LCD_H, RGB565_WHITE);  // must touch the bezel on all four sides
  field(12, 16, 3, 7, RGB565_WHITE, "CYD 2.8");
  field(12, 44, 2, 7, RGB565_WHITE, "240x320");
  // Colour-order check: these swap if R and B are crossed, which white hides.
  field(12, 72, 2, 4, RGB565_CYAN, "cyan");
  field(80, 72, 2, 6, RGB565_YELLOW, "yellow");
  field(12, 100, 2, 3, RGB565_LIGHTGREY, "ldr");
  field(12, 124, 2, 4, RGB565_LIGHTGREY, "boot");
  gfx->drawRect(0, PAD_Y0, LCD_W, PAD_Y1 - PAD_Y0, RGB565_DARKGREY);
  field(12, 306, 1, 20, RGB565_LIGHTGREY, "touch inside the box");
  backlight(bl);
}

void loop() {
  static int16_t tx = -1, ty = -1;
  static bool lastBoot = false;
  static uint32_t lastTick = 0;
  static uint8_t ledStep = 0;

  int16_t x, y;
  uint16_t rx, ry;
  if (touchRaw(&rx, &ry)) {
    touchMap(touchCal, rx, ry, &x, &y);
    if (x != tx || y != ty) {
      if (tx < 0) tone(SPK, 880, 40);  // touch-down: a click if a speaker is fitted
      dotHide();
      dotShow(x, y);
      char s[24];
      snprintf(s, sizeof s, "%3d,%3d  %4u,%4u", x, y, rx, ry);
      field(12, 144, 1, 20, RGB565_YELLOW, s);
      Serial.printf("touch %d,%d raw %u,%u\n", x, y, rx, ry);
      tx = x; ty = y;
    }
  } else if (tx >= 0) {
    dotHide();
    tx = ty = -1;
  }

  bool boot = bootPressed();
  if (boot != lastBoot) {
    gfx->fillRect(80, 124, 60, 16, boot ? RGB565_GREEN : RGB565_DARKGREY);
    Serial.printf("boot %d\n", boot);
    lastBoot = boot;
  }

  if (millis() - lastTick >= tickMs) {
    lastTick = millis();
    int ldr = analogRead(LDR);
    char s[8];
    snprintf(s, sizeof s, "%4d", ldr);
    field(80, 100, 2, 4, RGB565_CYAN, s);
    // LED walks R, G, B, off so a stuck or miswired channel is obvious.
    ledStep = (ledStep + 1) & 3;
    led(ledStep == 1, ledStep == 2, ledStep == 3);
    Serial.printf("tick %lu ldr=%d led=%u\n", millis(), ldr, ledStep);
  }
  delay(10);
}
