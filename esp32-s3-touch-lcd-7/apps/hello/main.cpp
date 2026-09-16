// hello: does the 7" panel light, are the colours the right way round, and
// does the touch land where the finger is. Serial says what it checked.
#include <board.h>

static Arduino_GFX *gfx;

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 8; i++) {  // a banner the monitor can catch after the USB port comes back
    Serial.printf("hello: booting %d\n", i);
    delay(400);
  }
  Serial.println("hello: step 1 expander");
  bool xp = boardBegin();
  Serial.printf("hello: step 2 panel (expander %s)\n", xp ? "ok" : "NO ACK");
  gfx = boardDisplay();
  bool ok = gfx->begin();
  Serial.println("hello: step 3 drawing");
  Serial.printf("hello: expander %s, panel %s, touch at 0x%02x, psram %u free\n", xp ? "ok" : "NO ACK",
                ok ? "ok" : "FAILED", boarddetail::gtAddr, ESP.getFreePsram());
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, LCD_W, LCD_H, RGB565_WHITE);
  gfx->drawRect(1, 1, LCD_W - 2, LCD_H - 2, RGB565_WHITE);
  // Test colours are cyan and yellow, never white or green: a swapped R and
  // B pin group turns cyan into yellow, which white would hide.
  gfx->fillRect(40, 40, 200, 120, RGB565_CYAN);
  gfx->fillRect(280, 40, 200, 120, RGB565_YELLOW);
  gfx->fillRect(520, 40, 200, 120, RGB565_RED);
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(40, 200);
  gfx->print("cyan  yellow  red  -- touch anywhere");
  gfx->setCursor(40, 240);
  gfx->print("ESP32-S3-Touch-LCD-7  800x480");
  backlight(255);
}

void loop() {
  static uint32_t last = 0, report = 0;
  if (millis() - report > 3000) {  // repeat: the one at boot prints before the USB port is open
    report = millis();
    Serial.printf("hello: expander shadow 0x%02x, touch at 0x%02x, heap %u, psram %u free, up %lus\n",
                  boarddetail::xShadow, boarddetail::gtAddr, ESP.getFreeHeap(), ESP.getFreePsram(),
                  (unsigned long)(millis() / 1000));
  }
  int16_t x, y;
  if (touchRead(&x, &y)) {
    gfx->fillCircle(x, y, 6, RGB565_GREEN);
    if (millis() - last > 200) {
      last = millis();
      Serial.printf("touch %d,%d\n", x, y);
    }
  }
  delay(10);
}
