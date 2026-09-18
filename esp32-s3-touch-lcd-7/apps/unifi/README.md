# unifi — **Network**, built and verified against a UDM SE (2026-09-18)

A UniFi dashboard: how many clients are on, wired and wireless; the
devices, online and not; the gateway's WAN throughput now and over the
last half hour; a page of the devices, a page of the clients, and the
Protect cameras as live snapshots.

```sh
pio run -e unifi -t upload
./push-config unifi            # config.json + config.local.json
```

## The API

The Network application's local **Integration API** on the console itself:
`https://<console>/proxy/network/integration/v1`, one `X-API-Key` header,
JSON, the console's own certificate. (Not the cloud Site Manager API, which
needs the internet and a different key, and not the old cookie-and-CSRF
controller API.) Read only: `/sites`, `/sites/{id}/devices`,
`/sites/{id}/devices/{id}/statistics/latest`, `/sites/{id}/clients`.

Make the key in the Network application: the **plug icon at the bottom of
the left rail** opens Integrations, where "Create New API Key" is (Network
10.6; older builds had it under Settings > Control Plane > Integrations).
Name it for the board; it shows once. Then either type it on the board -- the app boots into a setup page
with an on-screen keyboard until it has a key, and Settings > Console opens
that page again; the key goes to NVS -- or put it where the repo will not
see it:

```json
// data/config.local.json  (gitignored)
{ "unifi": { "host": "10.0.10.1", "apiKey": "xxxxxxxx" } }
```

The board must be able to reach the console on port 443. On a network with
an IoT VLAN that usually means a firewall rule from the IoT network to the
gateway's address, or putting the board on the main network.

## config.json

| key | what |
|---|---|
| `unifi.host` | the console's address or name (`unifi` resolves on most UniFi networks) |
| `unifi.apiKey` | leave empty here; it goes in `config.local.json` |
| `refresh` | `statsSeconds` (10), `devicesSeconds` (30), `clientsSeconds` (60) |
| `tz` | POSIX zone for the clock and the ages |

The Wi-Fi network is the ticker's (NVS `ticker`).

## On the panel

- **Overview.** Three tiles: CLIENTS (with the wired / wireless split),
  DEVICES (online / all), WAN DOWN (the gateway's rate, its upload, uptime,
  CPU); under them the gateway's throughput for the last thirty minutes,
  download in green, upload in the accent.
- **Devices.** A row each: an online dot, the name, the model, the address,
  uptime, CPU and memory; offline ones in red.
- **Clients.** Newest connection first: wired or wireless mark, the name
  (or MAC), the address, which device it hangs off, how long it has been
  on. A drag scrolls.
- **Cameras.** Four tiles of 320x180 from Protect's snapshot endpoint
  (`/proxy/protect/integration/v1/cameras/{id}/snapshot?highQuality=false`,
  a 640x360 JPEG of 20-45KB, the same key), the stalest tile refreshed every
  two seconds while the page is open, decoded by JPEGDEC at half size into
  PSRAM; the name and a state dot under each, offline ones say so. Swipe up
  and down through pages of four. Tap a tile for that camera at 640x360,
  refreshed every couple of seconds; left and right step through them.
- **Settings.** Clock, Theme (the twelve), Info (the last error the console
  sent, among other things), Console, Shut down. A two-second hold on the
  header shuts down too.

The gateway is the device whose features name `gateway`, or whose model
mentions Dream Machine, Dream Router, Dream Wall, Cloud Gateway, UDM, UDR,
UDW, UCG, UXG, USG or EFG (the UDM SE reports "UniFi Dream Machine PRO SE").

Verified 2026-09-18 on a UDM SE (UniFi OS 5.1, Network 10.6, Protect 7.2):
site, 9 devices with statistics, 26 clients, 5 cameras, snapshots at 20-45KB
decoding in well under a second, heap steady at 86KB.
