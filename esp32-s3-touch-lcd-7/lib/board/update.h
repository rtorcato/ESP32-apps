// Is Home up to date?
//
// Two things here, and only one of them can be finished today.
//
// CHECKING works: fetch a small JSON manifest, compare its version string to
// the one git stamped into this build, say so on the About page.
//
// UPGRADING does not, and the reason is structural rather than unfinished
// work. Home runs from `factory`. esp_ota cannot write the partition it is
// running from, and `factory` is not an OTA target in the first place. The
// only route is: write the new Home into ota_0, boot it, have it copy itself
// into factory, boot back -- which DESTROYS the app installed in ota_0 on the
// way past. That is a real cost to pay for a firmware that changes rarely,
// and it needs deciding rather than assuming, so the button says what it
// would do instead of doing it.
//
// The manifest also needs somewhere to live. ESP32-apps is a private repo, so
// a device cannot fetch from it unauthenticated, and a token compiled into
// flash is exactly what SECURITY.md says not to do. Until a host is chosen
// the URL is empty and the check reports "no update source", which is the
// truth rather than a failure.
#pragma once

#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "baseos.h"

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif
#ifndef FW_BUILT
#define FW_BUILT "unknown"
#endif

namespace update {

enum class State : uint8_t { Unknown, NoSource, Offline, Checking, UpToDate, Available, Failed };

inline State state = State::Unknown;
inline char latest[24] = "";
inline char note[64] = "";

// Set this to a manifest of the shape {"home":{"version":"1.2.0"}} once it has
// somewhere public to live. Empty until then, deliberately.
inline const char *MANIFEST_URL = "";

inline const char *stateWord() {
  switch (state) {
    case State::NoSource: return "no update source set";
    case State::Offline: return "cannot check, no network";
    case State::Checking: return "checking...";
    case State::UpToDate: return "up to date";
    case State::Available: return latest;
    case State::Failed: return note[0] ? note : "check failed";
    default: return "not checked";
  }
}

// A build that git could not name cannot be compared to anything, and a
// -dirty one is by definition not any released version. Both report as
// "not checked" rather than inventing an answer.
inline bool comparable() {
  const char *v = FW_VERSION;
  return strcmp(v, "unknown") != 0 && !strstr(v, "-dirty");
}

inline void check() {
  if (!MANIFEST_URL[0]) {
    state = State::NoSource;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    state = State::Offline;
    return;
  }
  state = State::Checking;

  NetworkClientSecure client;
  client.setInsecure();  // ponytail: pin the CA before this ever drives a flash
  client.setTimeout(8000);
  HTTPClient http;
  if (!http.begin(client, MANIFEST_URL)) {
    state = State::Failed;
    snprintf(note, sizeof note, "bad manifest URL");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    state = State::Failed;
    snprintf(note, sizeof note, "manifest HTTP %d", code);
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) {
    state = State::Failed;
    snprintf(note, sizeof note, "manifest not JSON");
    return;
  }
  const char *v = doc["home"]["version"] | "";
  if (!v[0]) {
    state = State::Failed;
    snprintf(note, sizeof note, "manifest has no home.version");
    return;
  }
  snprintf(latest, sizeof latest, "%s", v);
  state = strcmp(v, FW_VERSION) == 0 ? State::UpToDate : State::Available;
}

}  // namespace update
