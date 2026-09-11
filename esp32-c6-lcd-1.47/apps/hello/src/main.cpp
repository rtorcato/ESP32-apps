// Smoke test: proves the display offset, backlight, RGB LED and button all work.
// If the border touches all four screen edges, the 34px column offset is right.
#include <appcfg.h>
#include <board.h>

static Arduino_GFX *gfx;

// hello is the reference for the config convention (APP-CHECKLIST.md): every
// app reads apps/<app>/data/config.json, every key is optional, and an app must
// run on its compiled defaults with the file absent. There is genuinely little
// to configure in a smoke test, so this is about as small as a config gets --
// which is the point. Secrets never go here; hello needs none.
static uint8_t bl = 200;  // bright on purpose: this screen is for checking pixels
static uint16_t tickMs = 500;

void setup() {
  Serial.begin(115200);

  gfx = boardDisplay();
  gfx->begin();

  cfgSelfCheck();  // the shared accessors, asserted before anything reads them
  if (cfgLoad()) {
    bl = (uint8_t)cfgInt("brightness", bl, 8, 255);
    tickMs = (uint16_t)cfgInt("tickMs", tickMs, 50, 5000);
    cfgRelease();
  }
  Serial.printf("config: brightness %u, tick %ums\n", bl, tickMs);
  backlight(bl);

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, LCD_W, LCD_H, RGB565_WHITE);  // must hug all four edges
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 40);
  gfx->println("ESP32-C6");
  gfx->setCursor(12, 64);
  gfx->println("172x320");

  pinMode(BTN_BOOT, INPUT_PULLUP);
}

void loop() {
  // Cycle R/G/B so a dead channel is obvious. Dim -- 32 is already bright.
  static const uint8_t rgb[3][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}};
  static uint8_t i = 0;
  led(rgb[i][0], rgb[i][1], rgb[i][2]);
  i = (i + 1) % 3;

  bool pressed = digitalRead(BTN_BOOT) == LOW;
  gfx->fillRect(12, 110, LCD_W - 24, 20, pressed ? RGB565_GREEN : RGB565_DARKGREY);
  Serial.printf("tick %lu boot=%d\n", millis(), pressed);

  delay(tickMs);
}
