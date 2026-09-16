// Joining Wi-Fi reliably on this board. Shared because every networked app
// needs the same thing and each one getting it subtly wrong is how the same bug
// gets fixed twice.
//
// All of this is the result of the panel repeatedly showing NO WIFI on a link
// that was actually fine. The measurements are on this hardware.
#pragma once

#include <WiFi.h>

// ── radio tuning ─────────────────────────────────────────────────────────
// Call once before joining.
//
// Modem sleep is the interesting setting, and the obvious choices are both
// wrong:
//
//   MAX_MODEM     parks the radio between DTIM intervals. Saves ~2 C, and on a
//                 marginal link it drops the very ACKs that then show up as
//                 disconnect reason 34 (MISSING_ACKS) and repeated 204
//                 (HANDSHAKE_TIMEOUT). Reads exactly like a wrong password.
//   sleep(false)  runs the receiver continuously. Rock solid, and cost 12 C on
//                 this board: 43 C connected became 55 C.
//   MIN_MODEM     wakes for every beacon, sleeps in between. Nothing missed,
//                 most of the power saving. It is the Arduino default for a
//                 reason.
//
// Note the trap in reasoning about this: an earlier measurement here showed
// "the radio only costs about 1 C", but that compared radio *off* against
// MAX_MODEM -- not against no-sleep. Don't reuse that number to justify
// disabling sleep.
inline void netTune() {
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
}

// ── joining ──────────────────────────────────────────────────────────────
// Scan, pick the strongest BSSID for this SSID, and pin to it.
//
// Letting the stack choose is the problem: where two APs broadcast the same
// SSID (measured here: -57 dBm on one channel, -82 on another), it can latch
// onto the far one or bounce between them. Pinning took a link that sat at
// -76/-86 dBm with constant reconnects to a steady -53/-59 dBm that connects
// first try.
//
// Re-scanning on every call means a genuinely better AP still wins later, so
// this is not a permanent lock -- call it again on each retry.
// Given a finished scan of n networks, pin to the strongest AP for ssid
// and begin. Shared by the blocking and the non-blocking joins.
inline bool netBeginBest(int n, const char *ssid, const char *pass) {
  int best = -1, bestRssi = -127;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != ssid) continue;
    if (WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      best = i;
    }
  }
  if (best < 0) {
    Serial.printf("join: %s not on the air (%d APs seen)\n", ssid, n);
    WiFi.scanDelete();
    WiFi.begin(ssid, pass);  // try anyway: it may be hidden
    return false;
  }
  uint8_t bssid[6];
  memcpy(bssid, WiFi.BSSID(best), 6);
  int ch = WiFi.channel(best);
  Serial.printf("join: ch%d %ddBm %02x:%02x:%02x:%02x:%02x:%02x\n", ch, bestRssi, bssid[0],
                bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  WiFi.scanDelete();
  WiFi.begin(ssid, pass, ch, bssid);
  return true;
}
inline bool netJoinBest(const char *ssid, const char *pass) {
  return netBeginBest(WiFi.scanNetworks(false /*async*/, false /*hidden*/, false /*passive*/, 250), ssid, pass);
}
// The same join without the ~3s the scan blocks for: netJoinStart() kicks
// off an async scan, then netJoinTick() each loop pass issues the begin()
// once the scan is in and returns true that once. The panel stays alive
// throughout, which matters after a deep-sleep wake with prices to show.
inline void netJoinStart() { WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*passive*/, 250); }
inline bool netJoinTick(const char *ssid, const char *pass) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  if (n < 0) {  // WIFI_SCAN_FAILED: no scan to pick from, plain join
    WiFi.begin(ssid, pass);
    return true;
  }
  netBeginBest(n, ssid, pass);
  return true;
}

// How long to wait before the next retry, given how many have already failed.
// Doubles to a 2 minute ceiling: retrying every 20s forever at full TX power is
// its own heat source, and the board measured ~9 C hotter while failing than
// while connected.
inline uint32_t netRetryDelay(uint16_t retries, uint32_t base = 20000) {
  uint32_t wait = base << (retries < 3 ? retries : 3);
  return wait > 120000 ? 120000 : wait;
}
