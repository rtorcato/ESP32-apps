#pragma once
// Shared app configuration: /config.json on the app's LittleFS partition.
//
// The convention for every app on this board (see APP-CHECKLIST.md):
//
//   anything worth changing without a rebuild  ->  <app>/data/config.json
//   anything secret (Wi-Fi PSK, API keys)      ->  lib/board/secrets.h
//
// secrets.h is gitignored and compiled in; config.json is plain text sitting on
// a filesystem that anyone holding the board can dump, so a credential there
// would be a credential published. That split is the whole rule. See
// SECURITY.md for why flash encryption is not the answer here either.
//
// Reading this file is a TRUST BOUNDARY: it is hand-edited, so every accessor
// range-checks, keeps the caller's existing value on anything it doesn't like,
// and says on the serial log what it rejected. Clamping a typo to the nearest
// legal value silently produces a setting that "doesn't work" with no
// explanation; one printf turns that into a five-second fix.
//
// Every key is optional. An app must run on its compiled defaults with no
// config.json present at all -- the file tunes an app, it does not enable one.
// The one exception is an app whose config IS its data (ticker's watchlist),
// which says so itself.
//
// Usage:
//     cfgLoad();                                   // after gfx->begin()
//     blDay = cfgInt("brightness.day", blDay, 8, 255);
//     cfgStr("timezone", tzString, sizeof tzString);
//     for (JsonVariant v : cfgArr("stocks")) { ... }
//     cfgRelease();                                // frees the parsed tree

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <assert.h>

namespace cfgdetail {
inline JsonDocument doc;
inline bool present = false;
inline char err[64] = "";
// cfgSelfCheck() deliberately feeds bad values through every accessor, and
// those rejections printed straight into the boot log looked exactly like real
// config errors. Nothing else sets this.
inline bool quiet = false;
}  // namespace cfgdetail

inline bool cfgPresent() { return cfgdetail::present; }

// nullptr when the file loaded cleanly. Worth showing on screen for an app
// whose config is mandatory, and worth logging for one where it isn't.
inline const char *cfgError() { return cfgdetail::err[0] ? cfgdetail::err : nullptr; }

// Resolves a dotted path like "brightness.open". Returns a null variant for
// anything missing, so a partial path never throws and never half-matches.
inline JsonVariant cfgAt(const char *dotted) {
  JsonVariant v = cfgdetail::doc.as<JsonVariant>();
  if (!dotted || !*dotted) return v;
  char key[40];
  const char *p = dotted;
  while (*p) {
    size_t n = 0;
    while (*p && *p != '.' && n < sizeof key - 1) key[n++] = *p++;
    key[n] = '\0';
    if (*p == '.') p++;
    v = v[key];
    if (v.isNull()) break;
  }
  return v;
}

// One integer setting. Absent keeps `cur`; wrong type or out of range keeps
// `cur` and names itself on the log.
inline long cfgInt(const char *dotted, long cur, long lo, long hi) {
  JsonVariant v = cfgAt(dotted);
  if (v.isNull()) return cur;
  if (!v.is<long>()) {
    if (!cfgdetail::quiet) Serial.printf("config %s: not a number, keeping %ld\n", dotted, cur);
    return cur;
  }
  long x = v.as<long>();
  if (x < lo || x > hi) {
    if (!cfgdetail::quiet) Serial.printf("config %s: %ld out of range %ld..%ld, keeping %ld\n", dotted, x, lo, hi, cur);
    return cur;
  }
  return x;
}

inline float cfgFloat(const char *dotted, float cur, float lo, float hi) {
  JsonVariant v = cfgAt(dotted);
  if (v.isNull()) return cur;
  if (!v.is<float>()) {
    if (!cfgdetail::quiet) Serial.printf("config %s: not a number, keeping %g\n", dotted, (double)cur);
    return cur;
  }
  float x = v.as<float>();
  if (!(x >= lo && x <= hi)) {  // also rejects NaN
    if (!cfgdetail::quiet) Serial.printf("config %s: %g out of range %g..%g, keeping %g\n", dotted, (double)x, (double)lo,
                  (double)hi, (double)cur);
    return cur;
  }
  return x;
}

inline bool cfgBool(const char *dotted, bool cur) {
  JsonVariant v = cfgAt(dotted);
  if (v.isNull()) return cur;
  if (!v.is<bool>()) {
    if (!cfgdetail::quiet) Serial.printf("config %s: not true/false, keeping %s\n", dotted, cur ? "true" : "false");
    return cur;
  }
  return v.as<bool>();
}

// Copies into `out`, leaving it untouched when the key is absent.
//
// Deliberately not "return a const char *": the parsed document is released
// once boot is done, so a returned pointer would dangle into freed memory and
// work perfectly right up until it didn't.
inline bool cfgStr(const char *dotted, char *out, size_t n) {
  JsonVariant v = cfgAt(dotted);
  if (v.isNull()) return false;
  const char *s = v.as<const char *>();
  if (!s || !*s) {
    if (!cfgdetail::quiet) Serial.printf("config %s: not a string, keeping '%s'\n", dotted, out);
    return false;
  }
  snprintf(out, n, "%s", s);
  return true;
}

// "HH:MM" -> minutes past midnight. Rejects anything it doesn't fully
// understand, including trailing junk, so a typo leaves the default standing
// rather than silently shifting a window.
inline bool cfgParseHhMm(const char *s, uint16_t *out) {
  if (!s) return false;
  int h = -1, m = -1;
  char extra = 0;
  if (sscanf(s, "%d:%d%c", &h, &m, &extra) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  *out = (uint16_t)(h * 60 + m);
  return true;
}

inline bool cfgHhMm(const char *dotted, uint16_t *out) {
  JsonVariant v = cfgAt(dotted);
  if (v.isNull()) return false;
  if (!cfgParseHhMm(v.as<const char *>(), out)) {
    if (!cfgdetail::quiet) Serial.printf("config %s: not HH:MM, ignored\n", dotted);
    return false;
  }
  return true;
}

// Empty array when absent, so `for (auto v : cfgArr(...))` is always safe.
inline JsonArray cfgArr(const char *dotted) { return cfgAt(dotted).as<JsonArray>(); }

inline bool cfgLoad(const char *path = "/config.json") {
  cfgdetail::present = false;
  cfgdetail::err[0] = '\0';

  if (!LittleFS.begin()) {
    // Not formatting on failure is deliberate: an app must not silently wipe a
    // partition because one boot could not mount it.
    snprintf(cfgdetail::err, sizeof cfgdetail::err, "no filesystem (run uploadfs)");
    Serial.printf("config: %s -- using compiled defaults\n", cfgdetail::err);
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    snprintf(cfgdetail::err, sizeof cfgdetail::err, "%s missing", path);
    Serial.printf("config: %s -- using compiled defaults\n", cfgdetail::err);
    return false;
  }
  DeserializationError e = deserializeJson(cfgdetail::doc, f);
  f.close();
  if (e) {
    snprintf(cfgdetail::err, sizeof cfgdetail::err, "%s is not valid json (%s)", path, e.c_str());
    Serial.printf("config: %s -- using compiled defaults\n", cfgdetail::err);
    return false;
  }
  cfgdetail::present = true;
  Serial.printf("config: %s loaded\n", path);
  return true;
}

// Frees the parsed tree. Call once the settings have been copied out -- the
// document is several KB on a 512KB part, and no app needs it after boot.
inline void cfgRelease() { cfgdetail::doc.clear(); }

// The accessors are the one piece of shared logic here with real branching, so
// they get a check. Call from an app's selfCheck(); it leaves the document
// empty, so run it before cfgLoad().
inline void cfgSelfCheck() {
  cfgdetail::quiet = true;  // the rejections below are the point, not a problem
  cfgdetail::doc.clear();
  deserializeJson(cfgdetail::doc,
                  "{\"a\":{\"b\":7,\"s\":\"hi\",\"f\":1.5,\"t\":true},"
                  "\"arr\":[1,2,3],\"hm\":\"09:30\",\"bad\":\"x\"}");

  assert(cfgInt("a.b", 0, 0, 10) == 7);
  assert(cfgInt("a.b", 3, 0, 5) == 3);       // out of range -> keeps cur
  assert(cfgInt("a.missing", 42, 0, 99) == 42);
  assert(cfgInt("missing.entirely", 42, 0, 99) == 42);
  assert(cfgInt("bad", 42, 0, 99) == 42);    // wrong type -> keeps cur
  assert(cfgInt("a.b.c.d", 42, 0, 99) == 42);  // path through a scalar
  assert(cfgBool("a.t", false));
  assert(cfgBool("a.missing", true));
  assert(cfgFloat("a.f", 0.0f, 0.0f, 2.0f) == 1.5f);
  assert(cfgFloat("a.f", 9.0f, 0.0f, 1.0f) == 9.0f);  // out of range

  char buf[8] = "def";
  assert(cfgStr("a.s", buf, sizeof buf) && !strcmp(buf, "hi"));
  assert(!cfgStr("a.nope", buf, sizeof buf) && !strcmp(buf, "hi"));  // untouched

  uint16_t hm = 0;
  assert(cfgHhMm("hm", &hm) && hm == 9 * 60 + 30);
  assert(!cfgHhMm("bad", &hm) && hm == 9 * 60 + 30);  // untouched
  assert(cfgParseHhMm("0:0", &hm) && hm == 0);
  assert(cfgParseHhMm("23:59", &hm) && hm == 23 * 60 + 59);
  assert(!cfgParseHhMm("24:00", &hm));
  assert(!cfgParseHhMm("09:60", &hm));
  assert(!cfgParseHhMm("-1:00", &hm));
  assert(!cfgParseHhMm("0930", &hm));
  assert(!cfgParseHhMm("09:30x", &hm));  // trailing junk
  assert(!cfgParseHhMm("", &hm));
  assert(!cfgParseHhMm(nullptr, &hm));

  int sum = 0;
  for (JsonVariant v : cfgArr("arr")) sum += v.as<int>();
  assert(sum == 6);
  int n = 0;
  for (JsonVariant v : cfgArr("missing")) { (void)v; n++; }
  assert(n == 0);  // absent array iterates zero times rather than crashing

  cfgdetail::doc.clear();
  cfgdetail::present = false;
  cfgdetail::err[0] = '\0';
  cfgdetail::quiet = false;
}
