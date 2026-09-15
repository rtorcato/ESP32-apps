# Porsche apps — notes

**Status: notes, no code.** What an ESP32 display can and cannot read from a
Porsche Macan (95B, 2014–), and which app is worth building first. Written
2026-09-15 before owning any car-side hardware; everything marked *unverified*
needs a dongle and a driveway to confirm.

## Which bus you actually get

A Macan has several CAN buses (drive, comfort, infotainment, diagnostics) and
a **gateway** between them and the OBD-II socket. Plugging into the OBD port
does not put you on the drive bus; it puts you on the diagnostic side of the
gateway, which answers **requests** (ISO 15765-4 / UDS) rather than streaming
the raw traffic. That shapes everything below:

| Path | What you see | Hardware | Verdict |
|---|---|---|---|
| **OBD-II port, BLE ELM327 dongle** | Standard PIDs on request: RPM, speed, coolant, intake pressure, throttle, battery voltage, fuel level, ambient temp | Any board in this repo as a BLE client; ~$15–30 dongle | **Start here.** No wiring, read-only, works today. |
| OBD-II port, own CAN transceiver | Same PIDs, plus faster polling and full control of the request framing | ESP32-S3 + SN65HVD230 on two GPIOs, OBD plug and cable | Only if the dongle's latency annoys you. The rotary 2.1" exposes almost no GPIO; the 7" board has CAN on-board. |
| Raw drive-bus tap | Everything, continuously: wheel speeds, steering, gear, oil temp, boost | Splice behind a module or at the gateway connector | **Not on a daily driver.** Warranty, insurance, and a bad crimp can take the car down. |

## Data worth having on a dial

Standard mode-01 PIDs (documented, every OBD car): `0C` RPM, `0D` speed, `05`
coolant, `0B` manifold absolute pressure (**boost** = MAP − ambient), `11`
throttle, `42` control-module voltage, `2F` fuel level, `46` ambient air, `5C`
oil temperature (*unverified on the Macan*, many VAG/Porsche ECUs do answer it).

Porsche-specific values (oil temp if `5C` is silent, gearbox temp, tyre
pressures, individual wheel speeds) live behind **manufacturer UDS services**
(mode `22` with a 16-bit DID). The Macan shares its platform with the Audi Q5
(8R), so the Audi/VAG DID lists in the open-source OBD community are the place
to look — *unverified*, and expect to sniff a dealer tool or an app like
Carista/OBDeleven to confirm IDs.

## App ideas, in order

1. **boost-gauge** — one round dial, MAP minus ambient, peak-hold on press.
   Standard PIDs only, so it works on day one. The rotary 2.1" is the right
   face for it; the knob pages to oil temp and voltage.
2. **warm-up** — coolant and oil temperature side by side with a "go" ring
   that closes when both are up. The genuinely useful one for a turbo engine.
3. **battery-monitor** — the Macan's biggest owner complaint is 12V drain.
   Voltage at rest, cranking dip, and charging. Also standard PIDs.
4. **track-page** — RPM bar, gear (derived from RPM/speed ratio, *unverified*
   ratios), lap timer from the knob press. Needs the fast path, not a dongle.

## Rules

- **Read-only, always.** Requests only; never write to a module. An ESP32
  sending on the drive bus of a moving car is not a hobby project.
- **Don't leave a dongle in the port.** They drain the battery and a Bluetooth
  OBD dongle with a default PIN is a wireless door into the car. Unplug it, or
  wire it through the ignition-switched 12V.
- Poll gently. The gateway answers a handful of PIDs per 100ms; asking for
  more just queues.
- Nothing here needs an internet connection. Keep these apps off Wi-Fi.

## What to buy first

A **BLE (not classic Bluetooth) ELM327-compatible dongle** — the ESP32 core's
BLE client is far easier than classic SPP, and the S3 has no classic Bluetooth
at all. Vgate iCar Pro BLE 4.0 and OBDLink CX are the two that come up as
reliable; the $8 clones often fake their ELM version and drop frames.
*Unverified which the Macan's gateway prefers.*
