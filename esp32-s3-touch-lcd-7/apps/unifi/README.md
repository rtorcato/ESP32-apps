# unifi — **Network**, built (first flash 2026-09-18, awaiting an API key)

A UniFi dashboard: how many clients are on, wired and wireless; the
devices, online and not; the gateway's WAN throughput now and over the
last half hour; then a page of the devices and a page of the clients.

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

Make the key in UniFi Network > Settings > Control Plane > Integrations >
Create API Key, and put it where the repo will not see it:

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
- **Settings.** Clock, Theme (the twelve), Info (the last error the console
  sent, among other things), Console, Shut down. A two-second hold on the
  header shuts down too.

The gateway is the device whose features name `gateway`, or whose model
starts with UDM, UDR, UDW, UCG, UXG or USG.
