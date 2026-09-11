# Credentials on a device that can be stolen

`secrets.h` is gitignored, which keeps credentials out of the repo. It does
**nothing** for a board someone walks off with. This is the honest version of
what that costs and what actually helps.

## The threat

Anyone holding the board can plug in USB and dump flash (`esptool read_flash`).
There is no login, no disk encryption, no attestation by default. On these apps
the Wi-Fi PSK currently sits in **two** places:

| Where | Why |
|---|---|
| App partition | `secrets.h` is compiled into the firmware binary |
| NVS | Arduino's `WiFi` class defaults to `_persistent = true`, so `WiFi.begin()` writes the credential to flash storage as well |

The same applies to any API key an app holds — the UniFi key in
[unifi-status](esp32-c6-lcd-1.47/apps/unifi-status/), an MQTT password, a bearer
token for the [host-monitor](esp32-c6-lcd-1.47/apps/host-monitor/) agent.

## What does not help

Skip these. They feel like progress and cost real time:

- **Obfuscating or XOR-ing the string in source.** The key to decode it ships in
  the same binary. This is a speed bump measured in minutes.
- **Moving it from `secrets.h` into NVS, SPIFFS or LittleFS.** Unencrypted
  partitions are in the same flash dump.
- **Splitting it across several constants.** Same binary.
- **Hardcoding a MAC allowlist on the AP.** MACs are spoofable and it's in the
  dump too.

If the flash is readable, an unencrypted secret in it is readable. The only real
options are *encrypt the flash* or *make the stolen secret not worth much*.

## What helps, cheapest first

### 1. Put the device on an isolated IoT network — do this one

A separate SSID on its own VLAN, with client isolation on and no route to your
LAN. Then a stolen board yields credentials to a network that can reach the
internet and nothing else, and you can rotate that PSK whenever you like without
touching anything you care about.

This is the only mitigation on this page that **still works after a full flash
dump**, it takes about five minutes on UniFi, and it cannot brick anything.
For a desk clock, it is the correct answer and the list could stop here.

Note the apps that need to reach *inward* — `unifi-status`, `host-monitor`,
`poe-cycle` — need a firewall rule allowing that one destination and port. Keep
those rules narrow; that's the whole point of the VLAN.

### 2. Prefer revocable credentials over hidden ones

Being able to cut a stolen credential off matters more than hiding it:

- A **per-device PSK** on the IoT SSID, or **WPA2-Enterprise (EAP-TLS)** with a
  per-device certificate against UniFi's RADIUS. The cert is just as extractable
  from flash — the win is that you revoke *that device* rather than rotating a
  secret shared with every other device.
- For app credentials, a **scoped token that only the device needs**, not an
  admin key. `poe-cycle` already argues for this: a proxy that exposes only
  `POST /cycle/<allowed-port>` means a dumped board yields the ability to bounce
  a port you'd already whitelisted, and nothing else.

### 3. `WiFi.persistent(false)` — cheap and partial

Call it before `WiFi.begin()` and the core uses `WIFI_STORAGE_RAM`, so the
credential stops being copied into NVS. It's one line and removes one of the two
copies. It does **not** remove the copy compiled into the app partition, so
treat it as tidying, not a fix.

### 4. Flash encryption + Secure Boot v2 — the real fix, with real costs

This is the genuine cryptographic answer. The key lives in eFuse where software
can't read it, and `read_flash` returns ciphertext. The ESP32-C6 supports both.

**But price it honestly before starting:**

- **PlatformIO doesn't support it natively.** The request has been open since
  2020 ([platform-espressif32#305](https://github.com/platformio/platform-espressif32/issues/305)).
  You'd build the Arduino firmware here, then drive keys and flashing from a
  separate ESP-IDF workspace and shell scripts.
- **eFuse burns are permanent.** A wrong setup bricks the board, and the
  chicken-and-egg of encryption-plus-flashing is where people get stuck.
- **Order matters:** enable flash encryption *before* Secure Boot. Enabling
  Secure Boot write-protects `RD_DIS`, after which the encryption key can no
  longer be read-protected.
- **It disables the ROM USB-OTG stack.** This board is flashed over native USB —
  that's how everything in this repo gets uploaded. Losing it is a significant
  practical hit.
- **Release mode ends casual reflashing**, which is a bad trade for a board
  you're actively developing on. Development mode keeps limited reflashes.
- Don't lose the Secure Boot signing key, or you can never ship a trusted update
  to that board again.

**Verdict for this repo:** not worth it for a clock on a desk in your house.
Worth it for a board that lives somewhere you don't control — a device in a
rental, at a client site, or anything shipped to someone else.

## Also

- **The firmware binary contains the secret.** Don't post a `.bin`, share a
  `.pio/build/` directory, or attach one to an issue. `.pio/` is gitignored,
  which handles the accidental-commit case only.
- **`secrets.h` is per-board, not per-app** — every app on a device shares it, so
  one leak is all of them.
- **Rotate after a board goes missing.** That's the plan that actually matters,
  and it's much easier if step 1 is already done.

## Current state

| Item | Status |
|---|---|
| `secrets.h` gitignored | Yes |
| `secrets.h.example` tracked, no real values | Yes |
| `WiFi.persistent(false)` | Set in `desk-clock` |
| Isolated IoT VLAN | **Your call — recommended** |
| Flash encryption / Secure Boot | Not enabled, and deliberately so |
