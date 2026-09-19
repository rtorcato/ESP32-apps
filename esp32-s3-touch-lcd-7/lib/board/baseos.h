// The base OS: what belongs to the board rather than to any one app.
//
// One app runs at a time. The launcher lives in the `factory` partition and
// stays there; whichever app is loaded lives in `ota_0`. Switching is a boot
// partition change and a restart -- about two seconds, no network, no copying.
// apps/launcher/README.md has the reasoning and the partition table.
//
// Nothing here is loaded at runtime. Apps share this code at LINK time, the
// same as board.h and ui.h; the launcher only decides which firmware boots.
//
// What the base owns, and why it has to be the base that owns it: every
// setting below outlives the app that was running when it was changed.
// Settings kept per app would reset the theme every time you switched, and
// the Wi-Fi password would have to be typed once per app -- which is exactly
// the state the board is in today, where apps/sports reads credentials out of
// the ticker's NVS namespace and tells you to go and run the ticker first.
#pragma once

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <esp_sleep.h>

#include "board.h"

// Deliberately NOT ui.h. The base's job here is the settings STORE and which
// firmware boots; drawing is the caller's. Including ui.h would also have
// made this header unusable from apps/ticker, which predates ui.h and still
// carries its own THEMES and colour names -- they collide. Porting ticker to
// ui.h is worth doing, but nothing here should be what forces it.

// ── the shared settings ──────────────────────────────────────────────────
// NVS namespace "base". Apps read these and apply them; the launcher is where
// they are edited. Deliberately NOT here: anything only one app can mean --
// the ticker's refresh intervals, Game Day's favourite teams. Those stay in
// the app's own namespace.
//
// No brightness. This board's backlight is on/off through the CH422G with no
// PWM behind it (board.h X_LCD_BL), so a 0-255 setting would be a lie.
struct BaseSettings {
  uint8_t theme = 0;        // index into ui.h THEMES
  uint8_t rotation = 0;     // 0-3, applied at panelBegin
  uint8_t sleepMode = 0;    // 0 never, 1 night
  uint8_t sleepFrom = 23, sleepTo = 7;
  char tz[48] = "EST5EDT,M3.2.0/2,M11.1.0/2";
  char ssid[33] = "", pass[65] = "";
};
inline BaseSettings baseCfg;

inline void baseLoad() {
  Preferences p;
  p.begin("base", true);
  baseCfg.theme = p.getUChar("theme", baseCfg.theme);  // clamped by baseTheme() at the point of use
  baseCfg.rotation = p.getUChar("rot", baseCfg.rotation) % 4;
  baseCfg.sleepMode = p.getUChar("slp", baseCfg.sleepMode) % 2;
  baseCfg.sleepFrom = p.getUChar("slpf", baseCfg.sleepFrom) % 24;
  baseCfg.sleepTo = p.getUChar("slpt", baseCfg.sleepTo) % 24;
  p.getString("tz", baseCfg.tz, sizeof baseCfg.tz);
  p.getString("ssid", baseCfg.ssid, sizeof baseCfg.ssid);
  p.getString("pass", baseCfg.pass, sizeof baseCfg.pass);
  p.end();
}

// The stored index, clamped to however many themes the caller actually has.
// The base stores the number and has no opinion about what it means, which is
// what keeps ui.h out of this header:
//     sTheme = baseTheme(N_THEMES);
//     applyTheme();
inline uint8_t baseTheme(uint8_t n) { return n ? baseCfg.theme % n : 0; }

inline void baseSave() {
  Preferences p;
  p.begin("base", false);
  p.putUChar("theme", baseCfg.theme);
  p.putUChar("rot", baseCfg.rotation);
  p.putUChar("slp", baseCfg.sleepMode);
  p.putUChar("slpf", baseCfg.sleepFrom);
  p.putUChar("slpt", baseCfg.sleepTo);
  p.putString("tz", baseCfg.tz);
  p.end();
}

// Credentials are written on their own so a settings save cannot clear them.
inline void baseSaveWifi(const char *ssid, const char *pass) {
  Preferences p;
  p.begin("base", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  snprintf(baseCfg.ssid, sizeof baseCfg.ssid, "%s", ssid);
  snprintf(baseCfg.pass, sizeof baseCfg.pass, "%s", pass);
}

// ── which app is in the slot ─────────────────────────────────────────────
// The app tells us, because nothing else can. An ESP-IDF app descriptor
// carries a project_name, but under Arduino every build says
// "arduino-lib-builder" -- checked, it is not usable. An app calling this in
// setup() also means a slot flashed by hand over USB identifies itself, which
// a launcher-side record would not.
inline void baseClaimSlot(const char *app) {
  Preferences p;
  p.begin("base", false);
  char was[24] = "";
  p.getString("slot", was, sizeof was);
  if (strcmp(was, app) != 0) p.putString("slot", app);
  p.end();
}

inline const char *baseSlotApp(char *out, size_t n) {
  Preferences p;
  p.begin("base", true);
  out[0] = '\0';
  p.getString("slot", out, n);
  p.end();
  return out;
}

// ── switching which firmware boots ───────────────────────────────────────
namespace baseosdetail {
inline const esp_partition_t *find(esp_partition_subtype_t sub) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
}
}  // namespace baseosdetail

inline bool runningInLauncher() {
  const esp_partition_t *r = esp_ota_get_running_partition();
  return r && r->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

// True if ota_0 holds something bootable. A blank slot is all 0xFF, and the
// image magic is the cheapest way to tell without reading the whole thing.
inline bool baseSlotFilled() {
  const esp_partition_t *p = baseosdetail::find(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (!p) return false;
  uint8_t magic = 0;
  return esp_partition_read(p, 0, &magic, 1) == ESP_OK && magic == 0xE9;
}

// Reboot into the other partition. Returns only on failure -- there is no
// success path, the chip restarts.
inline bool bootInto(esp_partition_subtype_t sub) {
  const esp_partition_t *p = baseosdetail::find(sub);
  if (!p) return false;
  esp_err_t e = esp_ota_set_boot_partition(p);
  if (e != ESP_OK) {
    Serial.printf("bootInto: %s\n", esp_err_to_name(e));
    return false;
  }
  Serial.printf("booting into %s\n", p->label);
  Serial.flush();
  delay(120);
  esp_restart();
  return true;  // unreachable
}

inline bool bootIntoApp() { return bootInto(ESP_PARTITION_SUBTYPE_APP_OTA_0); }
inline bool bootIntoLauncher() { return bootInto(ESP_PARTITION_SUBTYPE_APP_FACTORY); }

// ── who gets the screen on boot ──────────────────────────────────────────
// The board comes up in the base, not in whatever ran last. That is what you
// want on a desk: power on, see the picker, choose.
//
// Doing it needs care, because launching an app IS a restart. An app that
// simply bounced to the base on every cold boot would bounce straight back
// out of the launch that just started it, forever. So the launcher leaves a
// handoff flag in NVS and the app consumes it: flag present means "the base
// sent me here on purpose", absent on a cold boot means the power was cut or
// reset was pressed, and the base should have the screen.
//
// A deep-sleep wake is not a cold boot, so an app that sleeps at night still
// wakes up as itself. That is the case this must not break.
// sleep.h's wakeCause() CONSUMES the shutdown flag, and the boot policy runs
// before the app has looked at it. A plain read of the cause instead, so the
// app still gets a truthful answer from wakeCause() later.
inline bool wokeCold() {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  return !(c == ESP_SLEEP_WAKEUP_EXT0 || c == ESP_SLEEP_WAKEUP_TIMER);
}

inline void baseHandoff() {
  Preferences p;
  p.begin("base", false);
  p.putUChar("handoff", 1);
  p.end();
}

inline bool baseTakeHandoff() {
  Preferences p;
  p.begin("base", false);
  bool had = p.getUChar("handoff", 0) != 0;
  if (had) p.remove("handoff");
  p.end();
  return had;
}

// The first line of an app's setup(), before the panel. Three jobs: the way
// out of a crash-loop, the boot policy above, and telling the base what is in
// the slot.
//
// BOOT has to be read here specifically. GPIO0 is also the panel's green bit
// 0, so once the RGB scan-out is running it cannot be read as a button.
inline void baseAppBoot(const char *app) {
  if (runningInLauncher()) return;  // the launcher itself does none of this

  if (bootPressed()) {  // the way out of an app that hangs or crash-loops
    Serial.println("BOOT held at power-on: back to the base");
    baseTakeHandoff();
    bootIntoLauncher();
    return;
  }

  bool handed = baseTakeHandoff();
  if (!handed && wokeCold()) {
    Serial.println("cold boot with no handoff: the base gets the screen");
    bootIntoLauncher();
    return;
  }
  baseClaimSlot(app);
}
