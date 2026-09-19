// Deep sleep, what woke us, and the snapshot that survives it.
//
// Lifted from apps/ticker, which is the only app that had the whole thing and
// learned every sharp edge below the hard way. The other three apps have a
// `shutDown()` that is the same idea with the sharp edges still in it.
//
// The snapshot deserves a word, because it is the part that is not obvious.
// RTC slow memory is a single fixed region at a fixed address: `RTC_DATA_ATTR`
// in two different firmwares puts two different layouts in the same bytes, so
// the worry was that swapping apps would have app B read app A's bytes through
// B's struct and believe them.
//
// MEASURED on the board, 2026-09-19 (apps/rtc-probe, since deleted): a boot
// counter in .rtc.data read 2 after a deep sleep and 1 after esp_restart().
// So .rtc.data survives deep sleep and IS reinitialised from the image on a
// plain restart -- and an app swap is a restart. Cross-app aliasing cannot
// happen.
//
// The header stays anyway, but be clear about which parts now earn their keep.
// The app-id and version fields are belt-and-braces against a case the chip
// already prevents. The CRC and the invalidate-in-rtcSnapshotBuffer() are the
// parts that still do real work: they catch a crash or a brownout part way
// through a write, which leaves a valid-looking header over a half-written
// payload. That is cheap enough to keep and awful enough to debug.
//
// Nothing here dictates the boot sequence. ticker decides whether to go back
// to sleep *before* it brings the panel up, which is the whole value of a
// timer wake inside the sleep window, and that ordering is tangled with its
// own config load. Primitives, not a framework, until a second app wants the
// same sequence.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_rom_crc.h>
#include <esp_sleep.h>
#include <string.h>
#include <time.h>

#include "board.h"

// ── the night window ─────────────────────────────────────────────────────
// Wraps midnight when from > to, which is the normal case (23 -> 7).
inline bool inNight(uint8_t h, uint8_t from, uint8_t to) {
  return from <= to ? (h >= from && h < to) : (h >= from || h < to);
}

// Six hours, so a long weekend still gets a look at the clock now and then --
// the RTC drifts, and a wake is the only chance to notice.
inline const uint32_t SLEEP_CAP_S = 6UL * 3600;

// Seconds until `hour` o'clock, +5s so the wake lands just past it rather
// than a tick short and immediately sleeping again.
inline uint32_t secondsUntilHour(const struct tm &t, uint8_t hour) {
  int32_t now = t.tm_hour * 60 + t.tm_min;
  int32_t mins = (hour * 60 - now + 1440) % 1440;
  if (mins == 0) mins = 1440;  // exactly on the hour means a whole day, not zero
  uint32_t secs = (uint32_t)mins * 60 - t.tm_sec + 5;
  return secs > SLEEP_CAP_S ? SLEEP_CAP_S : secs;
}

// ── what woke us ─────────────────────────────────────────────────────────
// Touch and Timer mean the app's state is probably still worth restoring.
// Shutdown is deliberately *not* one of those: "off" has to mean off, so the
// BOOT button after a shutdown is a fresh start, splash and all.
enum class Wake : uint8_t { Cold, Touch, Timer, Shutdown };

// ── the RTC region ───────────────────────────────────────────────────────
// Zero by default: most apps snapshot nothing, and RTC slow memory is only
// 8KB. An app that wants one sets -D RTC_SNAPSHOT_BYTES=N in platformio.ini.
#ifndef RTC_SNAPSHOT_BYTES
#define RTC_SNAPSHOT_BYTES 0
#endif

namespace sleepdetail {
inline const uint32_t MAGIC = 0x424153ADu;  // "BAS" + a byte that is not ASCII
struct Header {
  uint32_t magic, appId, crc;
  uint16_t version, len;
};
RTC_DATA_ATTR inline Header hdr;
RTC_DATA_ATTR inline bool wasShutdown;
#if RTC_SNAPSHOT_BYTES > 0
RTC_DATA_ATTR inline uint8_t payload[RTC_SNAPSHOT_BYTES];
#else
inline uint8_t *const payload = nullptr;
#endif

// FNV-1a over the app's id string. Only has to separate the handful of apps
// on one board from each other, so 32 bits with no collision handling is
// plenty -- a collision would need two app ids chosen to collide.
inline constexpr uint32_t idHash(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}
}  // namespace sleepdetail

// Classify the wake, and consume the shutdown flag so the next boot after this
// one reads Cold rather than Shutdown again.
inline Wake wakeCause() {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  bool slept = c == ESP_SLEEP_WAKEUP_EXT0 || c == ESP_SLEEP_WAKEUP_TIMER;
  if (!slept) return Wake::Cold;
  if (sleepdetail::wasShutdown) {
    sleepdetail::wasShutdown = false;
    return Wake::Shutdown;
  }
  return c == ESP_SLEEP_WAKEUP_TIMER ? Wake::Timer : Wake::Touch;
}

// The snapshot is built and read IN PLACE: the payload is the RTC region
// itself, so a 3.4KB app snapshot costs no DRAM to stage. Staging through a
// DRAM copy cost ticker 6.6KB, which is the opposite of the point.
//
//   memcpy(rtcSnapshotBuffer(), &mine, sizeof mine);   // or build it field by field
//   rtcSnapshotCommit("ticker", 1, sizeof mine);
//
// rtcSnapshotBuffer() invalidates the header before handing the pointer out,
// so a crash half way through writing leaves "no snapshot" rather than a
// valid-looking header over a half-written payload.
inline void *rtcSnapshotBuffer() {
  sleepdetail::hdr.magic = 0;
  return sleepdetail::payload;
}

// Seal what was written. Call it once the payload is complete.
inline bool rtcSnapshotCommit(const char *appId, uint16_t version, size_t n) {
  if (n > RTC_SNAPSHOT_BYTES) return false;  // built with too small a region: say so, do not truncate
  sleepdetail::hdr.appId = sleepdetail::idHash(appId);
  sleepdetail::hdr.version = version;
  sleepdetail::hdr.len = (uint16_t)n;
  sleepdetail::hdr.crc = esp_rom_crc32_le(0, sleepdetail::payload, n);
  sleepdetail::hdr.magic = sleepdetail::MAGIC;  // last: a half-written header is not a valid one
  return true;
}

// A pointer to the stored payload, or nullptr. Non-null only if the region
// holds exactly this app's snapshot, at this layout version, intact. Any
// doubt reads as "no snapshot" -- there is always a fetch that can rebuild
// the state, and a wrong restore is worse than a slow one.
inline const void *rtcSnapshotPeek(const char *appId, uint16_t version, size_t n) {
  const sleepdetail::Header &h = sleepdetail::hdr;
  if (h.magic != sleepdetail::MAGIC) return nullptr;
  if (h.appId != sleepdetail::idHash(appId) || h.version != version || h.len != n) return nullptr;
  if (n > RTC_SNAPSHOT_BYTES) return nullptr;
  if (esp_rom_crc32_le(0, sleepdetail::payload, n) != h.crc) return nullptr;
  return sleepdetail::payload;
}

// Throw the snapshot away. For a device wipe: the settings that produced the
// stored state are gone, so restoring it would show numbers for a watchlist
// that no longer exists.
inline void rtcSnapshotClear() { sleepdetail::hdr.magic = 0; }

// ── going to sleep ───────────────────────────────────────────────────────
// secs > 0: wakes on a touch or the timer, whichever comes first.
// secs == 0: a shutdown. The BOOT button alone, because "off" has to mean off
// and a screen that comes back when brushed is not off.
inline void goToSleep(uint32_t secs) {
  Serial.printf(secs ? "deep sleep for up to %lus, or a touch\n" : "shutdown: deep sleep until the BOOT button\n",
                (unsigned long)secs);
  sleepdetail::wasShutdown = secs == 0;
  backlight(0);  // a bare RGB panel has no sleep command; dark is dark, and deep sleep stops the scan-out
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // The finger that tapped "shut down" is still on the panel, and a finger on
  // the panel is the wake signal: wait for it to lift and the pen line to
  // settle, or the chip wakes before it has slept. The three apps that skipped
  // this and just delayed instead are the reason it is here and not there.
  uint32_t t0 = millis();
  int16_t tx, ty;
  while (touchRead(&tx, &ty) && millis() - t0 < 10000) delay(20);
  delay(400);
  if (secs) {
    // The GT911 pulses INT high on every report, finger or lift, and keeps
    // running through the chip's deep sleep: a level-high wake on it.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 1);
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  } else {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_BOOT, 0);  // low while pressed
  }
  Serial.flush();
  esp_deep_sleep_start();
}

// One runnable check for the arithmetic above; the rest needs a panel and a
// finger. Called from an app's own selfCheck(). Carried over from ticker,
// which is where these cases were originally written.
inline void sleepSelfCheck() {
  assert(inNight(23, 23, 7) && inNight(3, 23, 7) && !inNight(7, 23, 7) && !inNight(12, 23, 7));
  assert(inNight(1, 0, 6) && !inNight(6, 0, 6) && !inNight(5, 23, 23));
  struct tm w = {};
  w.tm_hour = 23, w.tm_min = 30;
  assert(secondsUntilHour(w, 7) == SLEEP_CAP_S);  // 7.5h away, capped
  w.tm_hour = 5, w.tm_min = 0;
  assert(secondsUntilHour(w, 7) == 2UL * 3600 + 5);
  w.tm_hour = 7, w.tm_min = 0;  // exactly on the hour: a whole day, then capped
  assert(secondsUntilHour(w, 7) == SLEEP_CAP_S);
  assert(sleepdetail::idHash("ticker") != sleepdetail::idHash("sports"));
}
