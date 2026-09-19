// rtc-probe: does RTC slow memory survive a deep sleep, and does it survive a
// plain restart?
//
// The second question is the one that matters. Swapping apps means an
// esp_restart into a different firmware, and `.rtc.data` is PROGBITS at a
// fixed address (0x50000200 in ticker's build). If a non-deep-sleep boot does
// NOT reinitialise it, then app B reads app A's bytes through B's layout, and
// every snapshot in the repo needs its header checked before it is believed.
// If it DOES reinitialise it, we are safe by construction and the header in
// sleep.h is cheap insurance rather than load-bearing.
//
// Written as a probe rather than reasoned about because the answer decides a
// design and the docs are easy to misread. Delete it once the answer is
// written down.
//
//   pio run -e rtc-probe -t upload -t monitor
//
// Then: tap RESTART twice, tap SLEEP once. The counter tells you which
// survives what.
#include <board.h>
#include <sleep.h>
#include <ui.h>

// A pattern chosen to be obviously not zero and obviously not ASCII, so a
// partial survival is visible rather than looking like a fresh boot.
struct Probe {
  uint32_t boots;
  uint8_t pattern[64];
};
static const uint16_t VERSION = 1;

static Probe probe;
static bool restored;
static Wake woke;

static const char *wakeName(Wake w) {
  return w == Wake::Cold ? "COLD (power-on or restart)" : w == Wake::Timer ? "TIMER" : w == Wake::Touch ? "TOUCH" : "SHUTDOWN+BOOT";
}

// Two big buttons, nothing clever.
static const int16_t BX = 80, BY = 300, BW = 300, BH = 96, BGAP = 340;
static void button(int16_t x, const char *label, uint16_t c) {
  gfx->fillRoundRect(x, BY, BW, BH, 12, c);
  textAt(x + (BW - textWidth(3, label)) / 2, BY + (BH - FACES[2].cap) / 2, 3, RGB565_BLACK, label);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bool xp = boardBegin();

  // Read BEFORE writing: this is the whole measurement.
  woke = wakeCause();
  restored = rtcSnapshotLoad("rtc-probe", VERSION, &probe, sizeof probe);

  bool patternOk = restored;
  for (uint8_t i = 0; i < 64 && patternOk; i++) patternOk = probe.pattern[i] == (uint8_t)(i * 7 + 1);

  Serial.printf("\nrtc-probe: wake=%s  snapshot=%s  pattern=%s  boots=%lu\n", wakeName(woke),
                restored ? "RESTORED" : "absent", restored ? (patternOk ? "intact" : "CORRUPT") : "-",
                (unsigned long)(restored ? probe.boots : 0));

  if (!restored) probe.boots = 0;
  probe.boots++;
  for (uint8_t i = 0; i < 64; i++) probe.pattern[i] = (uint8_t)(i * 7 + 1);
  if (!rtcSnapshotSave("rtc-probe", VERSION, &probe, sizeof probe))
    Serial.println("rtc-probe: SAVE FAILED -- RTC_SNAPSHOT_BYTES too small");

  sleepSelfCheck();
  Serial.println("rtc-probe: sleepSelfCheck passed");

  gfx = boardDisplay();
  bool panelOk = gfx->begin();
  Serial.printf("rtc-probe: expander %s, panel %s\n", xp ? "ok" : "NO ACK", panelOk ? "ok" : "FAILED");
  applyTheme();
  gfx->fillScreen(C_BG);
  gfx->setTextWrap(false);

  pageHeader("rtc-probe", "does RTC memory survive a restart?");
  char line[80];
  snprintf(line, sizeof line, "wake: %s", wakeName(woke));
  textAt(40, 110, 2, C_FG, line);
  snprintf(line, sizeof line, "snapshot: %s%s", restored ? "RESTORED" : "absent",
           restored ? (patternOk ? ", pattern intact" : ", PATTERN CORRUPT") : "");
  textAt(40, 150, 2, restored ? (patternOk ? C_GOOD : C_BAD) : C_WARN, line);
  snprintf(line, sizeof line, "boots since the counter last reset: %lu", (unsigned long)probe.boots);
  textAt(40, 190, 2, C_FG, line);
  textAt(40, 240, 1, C_MUTED,
         restored ? "the counter survived -- RTC memory persisted across whatever just happened"
                  : "the counter reset -- RTC memory did NOT persist");

  button(BX, "RESTART", C_WARN);
  button(BX + BGAP, "SLEEP 10s", C_GOOD);
  drawHint("RESTART answers the app-swap question; SLEEP is the control");
  backlight(255);
}

void loop() {
  int16_t x, y;
  if (touchRead(&x, &y) && y >= BY && y < BY + BH) {
    if (x >= BX && x < BX + BW) {
      Serial.println("rtc-probe: esp_restart()");
      delay(300);
      esp_restart();
    } else if (x >= BX + BGAP && x < BX + BGAP + BW) {
      goToSleep(10);
    }
  }
  delay(20);
}
