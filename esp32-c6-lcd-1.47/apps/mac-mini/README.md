# mac-mini — idea

<img src="preview.svg" alt="mac-mini preview" width="172">


A always-on status panel for the Mac mini: uptime, load, memory pressure, disk
free, CPU temp, network throughput — plus a button to sleep it and wake it back
up.

**Why this board:** it's the ideal shape for a stat stack. Roughly 10 label/value
rows fit on 172×320 with no scrolling, so the whole machine's state is one
glance with nothing hidden. Always USB-powered, so it can sit next to the mini
permanently. And the RGB LED carries reachability without needing to read
anything: green = up, amber = asleep, red = unreachable.

## The part to get straight first

**You cannot power the mini *on* from off.** Wake-on-LAN on macOS wakes from
*sleep* only — cold boot over the network is unsupported on all Macs, and
Apple Silicon especially, for firmware reasons. So the honest feature is
**sleep / wake**, not power on/off:

| Action | How | Works when |
|---|---|---|
| Sleep | agent on the Mac runs `pmset sleepnow` | Mac is awake and the agent is running |
| Wake | C6 sends a WoL magic packet | Mac is **asleep**, not shut down |
| Shut down | agent runs `shutdown -h now` | Mac is awake — **and then only a physical press brings it back** |

Two consequences worth designing around:

- **Offer sleep, not shutdown.** Once it's shut down the panel can't recover it,
  which turns a button press into a trip to the desk. If you do include
  shutdown, put it behind a separate, harder confirmation than sleep.
- **WoL needs wired Ethernet** with `sudo pmset -a womp 1`. Wake-on-Wireless is
  unreliable on consumer hardware — use the mini's Ethernet MAC, not its Wi-Fi
  MAC, in the magic packet.
- **Darkwake:** a magic packet often wakes the Mac for only ~30–60s before it
  sleeps again, because network wakes enter a partial "darkwake" state and
  repeated packets don't extend it. If you want it to *stay* up, have the agent
  run `caffeinate` for a window after waking, or open a real session (SSH) right
  after the packet.

## Getting the stats

The C6 can't read macOS state on its own — something has to run on the mini. A
small LaunchAgent that serves one JSON blob is the whole backend:

```
uptime          sysctl kern.boottime      → seconds, format on the C6
load            sysctl vm.loadavg         → 1/5/15 min
memory pressure memory_pressure           → % free is misleading on macOS;
                                            pressure is the number that matters
disk free       statfs / df               → per-volume
cpu temp        powermetrics (needs root) → or skip; it's the fiddliest one
network         netstat -ib deltas        → bytes/s up and down
top process     ps -Aro pcpu,comm         → one line, name + %
```

Poll it from the C6 every 2–5s with `HTTPClient`. Don't push from the Mac —
polling means the C6 recovers on its own after either side restarts, with no
retry logic on the Mac.

## Security — do not skip this

**The agent exposes an endpoint that can sleep or shut down your Mac.** On any
shared or untrusted network that is a real handoff of control, so:

- Require a **shared secret** on every state-changing request (bearer token in a
  header, compared with a constant-time compare). Read-only `/stats` can be open
  if you like; `/sleep` must not be.
- **Bind the listener to the LAN interface**, never `0.0.0.0` with a router port
  forward. This should be unreachable from the internet.
- Keep the token in `secrets.h` (already gitignored) on the C6 side and in a
  file with `600` perms on the Mac side — not in the LaunchAgent plist, which is
  world-readable.
- Give the agent the **least privilege that works**. `pmset sleepnow` needs no
  root. `shutdown -h now` does — which is another reason to prefer sleep.
- Log every action request on the Mac side. If the panel ever sleeps the machine
  unexpectedly you want to know whether the request came from the C6.

## Input with one button

There is no touchscreen — GPIO9 is the only input, so the interaction has to be
a small state machine:

- **Short press** — cycle stat pages (overview / disks / network / processes).
- **Long press (2s)** — arm the sleep action; show a countdown and require a
  second press to confirm. A single long press must never sleep the machine, or
  you'll do it by accident while paging.
- **If unreachable** — long press sends the WoL packet instead, since sleeping an
  already-asleep Mac is meaningless. Same button, state-dependent meaning, shown
  on screen.

**Pieces:** `HTTPClient` + `ArduinoJson` (with a filter) on the C6, `WiFiUdp` for
the magic packet, `Preferences` for host/MAC/token, Arduino_GFX. On the Mac: a
LaunchAgent running a Python or shell HTTP server — ~60 lines.

**Effort:** medium, and it splits cleanly. **Build the read-only stats panel
first** — it's genuinely useful on its own, has no security surface, and gets
the layout right. Add the sleep/wake button only once that's solid.
