// Installing an app, the way a person expects it to work: tap Install, watch
// a bar, the app starts. No laptop, no pio, no cable.
//
// The mechanics are not the hard part -- stream a .bin into ota_0, set the
// boot partition, restart. Two things about it are worth reading before
// changing anything.
//
// TRUST. This downloads code and then runs it, which makes it the most
// dangerous function on the board. So: the manifest is fetched over TLS
// verified against the Mozilla root bundle (not setInsecure, not one pinned
// CA that expires), every binary carries a sha256 in that manifest, and the
// hash is checked over the bytes actually written BEFORE the boot partition
// is moved. A binary that fails the hash is aborted and the slot is left
// unbootable rather than booted. Integrity comes from the manifest, so the
// binary itself may travel over plain HTTP -- but the manifest may not.
//
// What this does NOT defend against: a downgrade. Anyone who can serve the
// manifest can offer an older, genuine, vulnerable build and it will verify
// perfectly. Fixing that needs a signature and a monotonic version counter,
// which is a real feature rather than a line of code.
//
// DESTRUCTION. Installing overwrites ota_0, so the app that was there is
// gone the moment the first chunk lands. There is no undo and no second
// slot to fall back to. Home lives in `factory` and is untouched, so a
// failed install costs you the app, never the board.
#pragma once

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "baseos.h"

// The Mozilla roots IDF ships. Referencing the symbols is what links them.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace appstore {

struct Entry {
  char id[18], version[24], sha256[65];
  char url[200];
  char preview[200];  // optional: the screenshot, so it need not arrive by cable
  uint32_t size;
};

inline Entry entries[24];
inline uint8_t nEntries = 0;
inline char err[72] = "";
inline bool haveManifest = false;

inline void tlsVerified(NetworkClientSecure &c) {
  c.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
  c.setTimeout(12000);
}

// Where the manifest lives. Empty by default because it genuinely has
// nowhere to be yet: ESP32-apps is a private repo, so a device cannot read
// its releases, and a token compiled into flash is the thing SECURITY.md
// exists to forbid. Settable from NVS so it can be pointed at a host
// without a rebuild.
inline char manifestUrl[200] = "";

inline void loadUrl() {
  Preferences p;
  p.begin("base", true);
  p.getString("store", manifestUrl, sizeof manifestUrl);
  p.end();
}
inline void saveUrl(const char *u) {
  Preferences p;
  p.begin("base", false);
  p.putString("store", u);
  p.end();
  snprintf(manifestUrl, sizeof manifestUrl, "%s", u);
}

inline const Entry *find(const char *id) {
  for (uint8_t i = 0; i < nEntries; i++)
    if (strcmp(entries[i].id, id) == 0) return &entries[i];
  return nullptr;
}

// {"apps":[{"id":"ticker","version":"1.2.0","url":"https://...","sha256":"..","size":1332882}]}
inline bool fetchManifest() {
  nEntries = 0;
  haveManifest = false;
  if (!manifestUrl[0]) {
    snprintf(err, sizeof err, "no app source set");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(err, sizeof err, "no network");
    return false;
  }
  NetworkClientSecure client;
  tlsVerified(client);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, manifestUrl)) {
    snprintf(err, sizeof err, "bad app source URL");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    snprintf(err, sizeof err, "app source returned %d", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) {
    snprintf(err, sizeof err, "app list was not JSON");
    return false;
  }
  for (JsonObject a : doc["apps"].as<JsonArray>()) {
    if (nEntries >= 24) break;
    Entry &en = entries[nEntries];
    snprintf(en.id, sizeof en.id, "%s", a["id"] | "");
    snprintf(en.version, sizeof en.version, "%s", a["version"] | "");
    snprintf(en.sha256, sizeof en.sha256, "%s", a["sha256"] | "");
    snprintf(en.url, sizeof en.url, "%s", a["url"] | "");
    snprintf(en.preview, sizeof en.preview, "%s", a["preview"] | "");
    en.size = a["size"] | 0;
    if (en.id[0] && en.url[0] && strlen(en.sha256) == 64) nEntries++;
  }
  haveManifest = true;
  err[0] = '\0';
  Serial.printf("appstore: %u apps listed\n", nEntries);
  return nEntries > 0;
}

// Fetch an app's screenshot into LittleFS if it is not already there.
//
// Previews used to arrive only by `push-config`, which meant a board that
// had never been near the laptop showed placeholders -- and, worse, that
// pushing any app's config wiped the lot. If the app source can serve the
// binary it can serve the picture, and then neither depends on a cable.
//
// Failure is not an error: Home draws a labelled placeholder and everything
// else still works. A screenshot is not worth a dialog.
inline bool fetchPreview(const Entry &en, const char *path) {
  if (!en.preview[0] || WiFi.status() != WL_CONNECTED) return false;
  NetworkClientSecure tls;
  NetworkClient plain;
  bool https = strncmp(en.preview, "https:", 6) == 0;
  if (https) tlsVerified(tls);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.useHTTP10(true);
  if (!(https ? http.begin(tls, en.preview) : http.begin(plain, en.preview))) return false;
  if (http.GET() != 200) {
    http.end();
    return false;
  }
  // Written to a temporary name and renamed, so an interrupted download
  // cannot leave a half a picture that looks like a valid file.
  char tmp[64];
  snprintf(tmp, sizeof tmp, "%s.part", path);
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    http.end();
    return false;
  }
  int written = http.writeToStream(&f);
  f.close();
  http.end();
  if (written <= 0) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
  Serial.printf("appstore: preview for %s, %d bytes\n", en.id, written);
  return true;
}

// Progress is reported rather than drawn here: this header has no business
// knowing what the screen looks like.
using Progress = void (*)(uint8_t pct, const char *stage);

inline bool install(const Entry &en, Progress report) {
  const esp_partition_t *slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!slot) {
    snprintf(err, sizeof err, "no app slot in the partition table");
    return false;
  }
  if (en.size && en.size > slot->size) {
    snprintf(err, sizeof err, "%s is bigger than the app slot", en.id);
    return false;
  }
  if (report) report(0, "connecting");

  NetworkClientSecure tls;
  NetworkClient plain;
  bool https = strncmp(en.url, "https:", 6) == 0;
  if (https) tlsVerified(tls);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.useHTTP10(true);
  bool ok = https ? http.begin(tls, en.url) : http.begin(plain, en.url);
  if (!ok) {
    snprintf(err, sizeof err, "bad download URL");
    return false;
  }
  const char *hdrs[] = {"Location"};
  http.collectHeaders(hdrs, 1);
  int code = http.GET();
  if (code != 200) {
    snprintf(err, sizeof err, "download returned %d", code);
    http.end();
    return false;
  }
  int total = http.getSize();
  if (total <= 0) total = (int)en.size;

  esp_ota_handle_t h = 0;
  esp_err_t e = esp_ota_begin(slot, total > 0 ? (size_t)total : OTA_SIZE_UNKNOWN, &h);
  if (e != ESP_OK) {
    snprintf(err, sizeof err, "cannot open the slot (%s)", esp_err_to_name(e));
    http.end();
    return false;
  }
  // From here the old app is gone whatever happens, so the slot is marked
  // empty before a byte is written: a failure part way through must not
  // leave Home offering to run something that is half overwritten.
  baseClaimSlot("");

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t *chunk = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!chunk) chunk = (uint8_t *)malloc(4096);
  NetworkClient *st = http.getStreamPtr();
  int got = 0;
  uint32_t lastData = millis();
  bool failed = false;
  while (got < total || total <= 0) {
    int n = st->available();
    if (n <= 0) {
      if (!st->connected() && !st->available()) break;
      if (millis() - lastData > 15000) {
        snprintf(err, sizeof err, "download stalled at %d%%", total > 0 ? got * 100 / total : 0);
        failed = true;
        break;
      }
      delay(5);
      continue;
    }
    int r = st->read(chunk, n > 4096 ? 4096 : n);
    if (r <= 0) continue;
    lastData = millis();
    if (esp_ota_write(h, chunk, r) != ESP_OK) {
      snprintf(err, sizeof err, "write to the slot failed");
      failed = true;
      break;
    }
    mbedtls_sha256_update(&sha, chunk, r);
    got += r;
    if (report && total > 0) report((uint8_t)(got * 100L / total), "downloading");
  }
  free(chunk);
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (uint8_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

  if (!failed && total > 0 && got != total) {
    snprintf(err, sizeof err, "got %d of %d bytes", got, total);
    failed = true;
  }
  if (!failed && strcasecmp(hex, en.sha256) != 0) {
    // The whole reason the hash is here. Not a warning, not a prompt.
    snprintf(err, sizeof err, "checksum did not match -- refusing to run it");
    Serial.printf("appstore: sha256 expected %s got %s\n", en.sha256, hex);
    failed = true;
  }
  if (failed) {
    esp_ota_abort(h);
    return false;
  }
  if (report) report(100, "verifying");

  e = esp_ota_end(h);  // checks the image header and its own checksum too
  if (e != ESP_OK) {
    snprintf(err, sizeof err, "image rejected (%s)", esp_err_to_name(e));
    return false;
  }
  if (esp_ota_set_boot_partition(slot) != ESP_OK) {
    snprintf(err, sizeof err, "could not set the boot partition");
    return false;
  }
  baseClaimSlot(en.id);  // the app confirms this itself on first run
  err[0] = '\0';
  Serial.printf("appstore: installed %s %s (%d bytes, sha ok)\n", en.id, en.version, got);
  return true;
}

}  // namespace appstore
