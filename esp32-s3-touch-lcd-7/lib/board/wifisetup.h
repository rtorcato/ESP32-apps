// Wi-Fi setup: the board asks for its network.
//
// Lifted out of apps/ticker, where it was written and where it does not
// belong. Credentials outlive whichever app is running, so the base owns
// them -- and because it did not, the board is in a state today where
// apps/sports reads them out of the ticker's NVS namespace and prints
// "no network in NVS: run the ticker once and set it up there".
//
// No credentials (first boot, or after a wipe, or the Wi-Fi row): the panel
// shows three steps and the board raises an access point with an eight-digit
// PIN derived from its MAC, serving one form at 192.168.4.1. A DNS catch-all
// makes phones open it by themselves. The form lists the networks it can
// hear. Saving writes NVS and restarts.
//
// Needs ui.h, so only Home includes it. That is the intended shape: an app
// should never run this.
#pragma once

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "baseos.h"
#include "board.h"
#include "ui.h"

namespace wifisetup {

inline WebServer *web = nullptr;
inline DNSServer *dns = nullptr;
inline bool active = false;
inline char pin[9] = "", ssids[600] = "", err[80] = "";
inline const char *apName = "board-setup";

inline void page() {
  String html = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>setup</title><style>body{font-family:-apple-system,Helvetica,Arial;background:#000;color:#eee;"
                  "margin:0;padding:24px}h1{font-size:22px}label{display:block;margin:18px 0 6px;color:#9aa4ae}"
                  "select,input{width:100%;font-size:18px;padding:10px;border-radius:8px;border:1px solid #444;background:#111;color:#eee}"
                  "button{margin-top:24px;width:100%;font-size:18px;padding:12px;border:0;border-radius:8px;background:#22d05a;color:#000}"
                  "</style></head><body><h1>board setup</h1>");
  if (err[0]) {
    html += F("<p style='color:#f0473c'>");
    html += err;
    html += F("</p>");
  }
  html += F("<form method=post action=/save><label>Wi-Fi network</label><select name=s>");
  html += ssids;
  html += F("</select><label>or type its name</label><input name=o placeholder='hidden network'>"
            "<label>password</label><input type=password name=p id=p>"
            "<label style='display:flex;align-items:center;gap:8px;margin-top:10px'>"
            "<input type=checkbox style='width:auto' onchange=\"p.type=this.checked?'text':'password'\">show password</label>"
            "<button>save and restart</button></form></body></html>");
  web->send(200, "text/html", html);
}

inline void save() {
  String ssid = web->arg("o");
  if (!ssid.length()) ssid = web->arg("s");
  String pass = web->arg("p");
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
    web->send(400, "text/plain", "network name missing or too long");
    return;
  }
  // Try the network BEFORE keeping it. The access point stays up while the
  // station side joins, so a wrong password comes straight back to the phone
  // as an error rather than leaving a board stuck on NO WIFI with no way to
  // say why. Worth the fifteen seconds.
  drawHint("trying that network...");
  Serial.printf("setup: trying '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    snprintf(err, sizeof err, "Could not join \"%.32s\". Wrong password? Try again.", ssid.c_str());
    Serial.printf("setup: join failed (%s)\n", err);
    drawHint("that network did not let it in. wrong password?", C_WARN);
    page();  // the form again, with the reason at the top
    return;
  }
  baseSaveWifi(ssid.c_str(), pass.c_str());
  web->send(200, "text/html",
            F("<!doctype html><body style='font-family:-apple-system,Helvetica;background:#000;color:#eee;padding:24px'>"
              "<h1>connected</h1><p>The board joined your network and is restarting.</p></body>"));
  Serial.printf("setup: joined '%s' (%ddBm), saved, restarting\n", ssid.c_str(), WiFi.RSSI());
  delay(800);
  ESP.restart();
}

// The panel side: three steps, in order, large enough to read across a desk.
// The network name and its password sit on one line so a phone can be held
// up next to them.
inline void draw(const char *status) {
  gfx->fillScreen(C_BG);
  textAt(20, 20, 3, C_FG, "SETUP");
  gfx->drawFastHLine(20, 64, LCD_W - 40, C_RULE);
  textAt(20, 88, 2, C_GOOD, "1");
  textAt(56, 90, 1, C_MUTED, "on your phone, join the Wi-Fi network");
  textAt(56, 118, 3, C_FG, apName);
  char l[16];
  snprintf(l, sizeof l, "%.4s %.4s", pin, pin + 4);
  int16_t x = 56 + textWidth(3, apName) + 40;
  textAt(x, 126, 1, C_MUTED, "password");
  textAt(x + textWidth(1, "password") + 16, 118, 3, C_GOLD, l);

  textAt(20, 180, 2, C_GOOD, "2");
  textAt(56, 182, 1, C_MUTED, "a sign-in page opens by itself; if not, open this in the browser");
  textAt(56, 210, 3, C_FG, "192.168.4.1");

  textAt(20, 272, 2, C_GOOD, "3");
  textAt(56, 274, 1, C_MUTED, "pick your network, type its password, save. The board restarts and joins.");

  gfx->drawFastHLine(20, 330, LCD_W - 40, C_RULE);
  textAt(20, 346, 1, C_DIM, "nothing leaves this board: the password is kept in its own flash, and only there.");
  drawHint(status);
}

inline void begin() {
  active = true;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(pin, sizeof pin, "%08lu", (unsigned long)((mac ^ (mac >> 24)) % 100000000UL));
  if (strlen(pin) < 8) strcpy(pin, "12345678");
  draw("scanning for networks");
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks(false, false, false, 250);
  ssids[0] = '\0';
  for (int i = 0; i < n && i < 12; i++) {
    char opt[64];
    snprintf(opt, sizeof opt, "<option>%.32s</option>", WiFi.SSID(i).c_str());
    strlcat(ssids, opt, sizeof ssids);
  }
  WiFi.scanDelete();
  WiFi.softAP(apName, pin);
  web = new WebServer(80);
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());
  web->on("/", HTTP_GET, page);
  web->on("/save", HTTP_POST, save);
  web->onNotFound([] {  // the captive probes of every phone land here
    web->sendHeader("Location", "http://192.168.4.1/", true);
    web->send(302, "text/plain", "");
  });
  web->begin();
  Serial.printf("setup: AP %s, password %s, form at http://%s (%d networks heard)\n", apName, pin,
                WiFi.softAPIP().toString().c_str(), n);
  draw(baseCfg.ssid[0] ? "tap to keep the old network" : "waiting for you");
}

inline void tick() {
  if (!active) return;
  dns->processNextRequest();
  web->handleClient();
}

}  // namespace wifisetup
