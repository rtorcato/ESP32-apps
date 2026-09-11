#!/usr/bin/env python3
"""Serve macOS host stats as JSON for the esp32-c6 host-monitor panel.

Read-only and deliberately boring: it exposes no secrets and cannot change
anything, so it needs no authentication. That is the whole reason to build the
read-only panel first -- the sleep/wake endpoint is what needs a token, a
bound interface and least privilege, and none of that has to exist yet.

Standard library only, so there is nothing to install and nothing to keep
updated.

    python3 macos.py                          # foreground, port 8787
    python3 macos.py --port 9000 --disk /Volumes/Data

GET /stats -> the JSON the firmware parses
GET /       -> the same, so a browser shows something useful
"""

import argparse
import json
import os
import re
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE_RE = re.compile(r"page size of (\d+)")


def sh(*cmd: str) -> str:
    """Run a command, return stdout, never raise. A missing stat is better than
    a 500 that tells the panel nothing."""
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=4).stdout
    except Exception:
        return ""


def uptime_seconds() -> int:
    # kern.boottime looks like: { sec = 1757500000, usec = 0 } Tue Sep ...
    m = re.search(r"sec\s*=\s*(\d+)", sh("sysctl", "-n", "kern.boottime"))
    return int(time.time()) - int(m.group(1)) if m else 0


def memory_used_pct() -> int:
    """Percentage of physical memory in use.

    Deliberately not "free memory": macOS uses everything it can for cache, so
    free is always low and means nothing. active + wired + compressed over the
    total is the number that tracks how squeezed the machine actually is.
    """
    out = sh("vm_stat")
    if not out:
        return 0
    page = int(m.group(1)) if (m := PAGE_RE.search(out)) else 4096

    def pages(label: str) -> int:
        m = re.search(rf"{label}:\s+(\d+)", out)
        return int(m.group(1)) if m else 0

    free = pages("Pages free") + pages("Pages speculative")
    inactive = pages("Pages inactive")
    used = pages("Pages active") + pages("Pages wired down") + pages(
        "Pages occupied by compressor"
    )
    total = used + free + inactive
    _ = page  # page size only matters if we reported bytes; percentages don't need it
    return round(100 * used / total) if total else 0


# Which filesystem to report. Not always "/": on a NAS the root is a small
# system partition and the interesting number is the data volume.
DISK_PATH = "/"


def disk_root() -> tuple[int, int]:
    try:
        st = os.statvfs(DISK_PATH)
        return st.f_bavail * st.f_frsize, st.f_blocks * st.f_frsize
    except OSError:
        return 0, 0


def cpu_and_top() -> tuple[int, str, float]:
    """Aggregate CPU percent plus the busiest process.

    Uses ps rather than `top -l 1`, which takes about a second and would make
    every poll slow. ps percentages are per-core, so divide by core count.
    """
    out = sh("ps", "-Aro", "pcpu,comm")
    lines = [l for l in out.splitlines()[1:] if l.strip()]
    total = 0.0
    top_name, top_pct = "-", 0.0
    for i, line in enumerate(lines):
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        try:
            pct = float(parts[0])
        except ValueError:
            continue
        total += pct
        if i == 0:
            top_pct = pct
            # Just the executable name, not the whole path.
            top_name = parts[1].strip().split("/")[-1][:14]
    cores = os.cpu_count() or 1
    return min(100, round(total / cores)), top_name, round(top_pct, 1)


_net_prev: dict[str, float] = {}


def net_rates() -> tuple[float, float]:
    """Bytes/sec in and out on the default interface, from counter deltas.

    Counters reset when an interface bounces, so a negative delta means reset,
    not negative traffic -- clamp it rather than reporting nonsense.
    """
    iface = ""
    for line in sh("route", "-n", "get", "default").splitlines():
        if "interface:" in line:
            iface = line.split(":", 1)[1].strip()
            break
    if not iface:
        return 0.0, 0.0

    ib = ob = 0
    for line in sh("netstat", "-ibn").splitlines():
        f = line.split()
        # Pick the <Link#> row: it carries the byte counters for the interface.
        if len(f) > 9 and f[0] == iface and f[2].startswith("<Link"):
            try:
                ib, ob = int(f[6]), int(f[9])
            except (ValueError, IndexError):
                pass
            break

    now = time.time()
    prev = _net_prev.get("t")
    down = up = 0.0
    if prev:
        dt = now - prev
        if dt > 0:
            down = max(0, ib - _net_prev.get("ib", 0)) / dt
            up = max(0, ob - _net_prev.get("ob", 0)) / dt
    _net_prev.update({"t": now, "ib": ib, "ob": ob})
    return round(down, 1), round(up, 1)


def collect() -> dict:
    free, total = disk_root()
    cpu, top_name, top_pct = cpu_and_top()
    down, up = net_rates()
    load = os.getloadavg()
    return {
        "host": os.uname().nodename.split(".")[0],
        "uptime_s": uptime_seconds(),
        "load": [round(l, 2) for l in load],
        "cpu_pct": cpu,
        "mem_pct": memory_used_pct(),
        "disk_free_gb": round(free / 1e9, 1),
        "disk_total_gb": round(total / 1e9, 1),
        "net_down_bps": down,
        "net_up_bps": up,
        "top_name": top_name,
        "top_pct": top_pct,
        # powermetrics needs root, so no die temperature. The panel shows "--"
        # rather than inventing a number.
        "cpu_temp_c": None,
    }



# ── serial feed ──────────────────────────────────────────────────────────
# Writing to the board over USB instead of serving HTTP.
#
# This is the zero-configuration path: no Wi-Fi, no IP address, no URL, no
# firewall rule. Plug the board in, run this, and it works -- and it follows
# whatever machine it is plugged into, because the port *is* the identity.
#
# The port is a character device, so plain file I/O is enough and pyserial is
# not needed. Baud rate is meaningless over USB CDC.
SERIAL_GLOBS = ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*")


def find_port() -> str | None:
    import glob as _glob
    for pattern in SERIAL_GLOBS:
        hits = sorted(_glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def serial_feed(port: str | None, interval: float) -> int:
    """Write one JSON line per interval to the board, reopening if it vanishes.

    Reopening matters: the port disappears when the board is reflashed or
    replugged, and a feed that dies on the first disconnect is a feed you have
    to remember to restart.
    """
    import time as _time

    collect()  # prime the counters so the first line has real deltas
    fh = None
    while True:
        try:
            if fh is None:
                p = port or find_port()
                if not p:
                    print("waiting for the board to appear...", flush=True)
                    _time.sleep(2)
                    continue
                fh = open(p, "wb", buffering=0)
                print(f"feeding {p}", flush=True)
            fh.write((json.dumps(collect()) + "\n").encode())
        except (OSError, BrokenPipeError) as e:
            print(f"port closed ({e}); will reopen", flush=True)
            try:
                if fh:
                    fh.close()
            except OSError:
                pass
            fh = None
            _time.sleep(2)
            continue
        _time.sleep(interval)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path.split("?")[0] not in ("/", "/stats"):
            self.send_error(404)
            return
        body = json.dumps(collect()).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass  # a poll every few seconds would otherwise fill the log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--disk", default="/", help="filesystem to report (e.g. /volume1)")
    ap.add_argument("--serial", action="store_true",
                    help="feed the board over USB instead of serving HTTP (no network at all)")
    ap.add_argument("--port-path", default=None, help="serial device, if auto-detect picks wrong")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between samples")
    args = ap.parse_args()

    global DISK_PATH
    DISK_PATH = args.disk

    if args.serial:
        raise SystemExit(serial_feed(args.port_path, args.interval))

    net_rates()  # prime the counters so the first poll has a delta to report
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"host-monitor macos agent on http://{args.bind}:{args.port}/stats"
          f"  disk={DISK_PATH}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
