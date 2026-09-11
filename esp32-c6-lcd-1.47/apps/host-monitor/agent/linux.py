#!/usr/bin/env python3
"""Serve Linux host stats as JSON for the esp32-c6 host-monitor panel.

Same JSON contract as macos.py -- the firmware doesn't know or care which OS
produced it, so the contract is the portable boundary. Standard library only.

    python3 linux.py                          # foreground, port 8787
    python3 linux.py --disk /volume1          # Synology: report the data volume

GET /stats -> the JSON the firmware parses
GET /       -> the same, so a browser shows something useful

Reads /proc directly rather than shelling out to ps/netstat/ip: fewer processes
per poll, no output-format differences between distributions, and the parsing
functions stay pure so they can be tested on any machine (see --selftest).
"""

import argparse
import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── pure parsers ─────────────────────────────────────────────────────────
# Each takes file contents as text and returns a value. Keeping them pure is
# what makes them testable without a Linux host -- run --selftest anywhere.


def parse_uptime(proc_uptime: str) -> int:
    """/proc/uptime is "seconds idle_seconds"."""
    try:
        return int(float(proc_uptime.split()[0]))
    except (ValueError, IndexError):
        return 0


def parse_loadavg(proc_loadavg: str) -> tuple[float, float, float]:
    """/proc/loadavg is "1min 5min 15min running/total lastpid"."""
    f = proc_loadavg.split()
    try:
        return float(f[0]), float(f[1]), float(f[2])
    except (ValueError, IndexError):
        return 0.0, 0.0, 0.0


def parse_meminfo(proc_meminfo: str) -> int:
    """Percent of RAM in use.

    Uses MemAvailable, which is the kernel's own estimate of what a workload
    could claim -- MemFree alone ignores reclaimable cache and reads alarmingly
    low on any healthy machine. Same reasoning as preferring used-percent over
    free on macOS.
    """
    vals = {}
    for line in proc_meminfo.splitlines():
        m = re.match(r"(\w+):\s+(\d+)", line)
        if m:
            vals[m.group(1)] = int(m.group(2))
    total = vals.get("MemTotal", 0)
    if not total:
        return 0
    avail = vals.get("MemAvailable")
    if avail is None:  # kernels before 3.14
        avail = vals.get("MemFree", 0) + vals.get("Cached", 0) + vals.get("Buffers", 0)
    return max(0, min(100, round(100 * (total - avail) / total)))


def parse_cpu_times(proc_stat: str) -> tuple[int, int]:
    """(busy, total) jiffies from the aggregate "cpu" line of /proc/stat."""
    for line in proc_stat.splitlines():
        if line.startswith("cpu "):
            f = [int(x) for x in line.split()[1:]]
            total = sum(f)
            idle = f[3] + (f[4] if len(f) > 4 else 0)  # idle + iowait
            return total - idle, total
    return 0, 0


def parse_net_bytes(proc_net_dev: str, iface: str) -> tuple[int, int]:
    """(rx, tx) byte counters for one interface from /proc/net/dev."""
    for line in proc_net_dev.splitlines():
        if ":" not in line:
            continue
        name, rest = line.split(":", 1)
        if name.strip() != iface:
            continue
        f = rest.split()
        try:
            return int(f[0]), int(f[8])
        except (ValueError, IndexError):
            return 0, 0
    return 0, 0


def parse_default_iface(proc_net_route: str) -> str:
    """Interface with the default route (destination 00000000)."""
    for line in proc_net_route.splitlines()[1:]:
        f = line.split()
        if len(f) > 1 and f[1] == "00000000":
            return f[0]
    return ""


# ── host access ──────────────────────────────────────────────────────────
def read(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""  # a missing stat beats a 500 that tells the panel nothing


# Which filesystem to report. Not always "/": on a Synology, root is a ~2GB
# system partition, so reporting it would confidently show the wrong number --
# pass --disk /volume1 there.
DISK_PATH = "/"


def disk_root() -> tuple[int, int]:
    try:
        s = os.statvfs(DISK_PATH)
        return s.f_bavail * s.f_frsize, s.f_blocks * s.f_frsize
    except OSError:
        return 0, 0


def busiest_process() -> tuple[str, float]:
    """Highest-CPU process, by sampling /proc/<pid>/stat twice.

    ps would be one line, but its flags and column names differ across
    distributions and busybox. /proc is stable.
    """
    clk = os.sysconf("SC_CLK_TCK") or 100

    def snapshot() -> dict[str, tuple[int, str]]:
        out = {}
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            stat = read(f"/proc/{pid}/stat")
            if not stat:
                continue
            # comm is parenthesised and may contain spaces, so split on the
            # last ')' rather than on whitespace.
            try:
                lp = stat.rindex(")")
                name = stat[stat.index("(") + 1 : lp]
                f = stat[lp + 2 :].split()
                out[pid] = (int(f[11]) + int(f[12]), name)  # utime + stime
            except (ValueError, IndexError):
                continue
        return out

    a = snapshot()
    time.sleep(0.25)
    b = snapshot()

    best_name, best_pct = "-", 0.0
    for pid, (jiffies, name) in b.items():
        if pid not in a:
            continue
        delta = jiffies - a[pid][0]
        pct = 100.0 * delta / clk / 0.25
        if pct > best_pct:
            best_pct, best_name = pct, name[:14]
    return best_name, round(best_pct, 1)


_prev: dict[str, float] = {}


def collect() -> dict:
    now = time.time()
    iface = parse_default_iface(read("/proc/net/route"))
    rx, tx = parse_net_bytes(read("/proc/net/dev"), iface) if iface else (0, 0)
    busy, total = parse_cpu_times(read("/proc/stat"))

    down = up = 0.0
    cpu_pct = 0
    if "t" in _prev:
        dt = now - _prev["t"]
        if dt > 0:
            # Counters reset when an interface bounces, so clamp: a negative
            # delta means reset, not negative traffic.
            down = max(0, rx - _prev.get("rx", 0)) / dt
            up = max(0, tx - _prev.get("tx", 0)) / dt
        dtotal = total - _prev.get("total", 0)
        if dtotal > 0:
            cpu_pct = max(0, min(100, round(100 * (busy - _prev.get("busy", 0)) / dtotal)))
    _prev.update({"t": now, "rx": rx, "tx": tx, "busy": busy, "total": total})

    free, cap = disk_root()
    l1, l5, l15 = parse_loadavg(read("/proc/loadavg"))
    top_name, top_pct = busiest_process()

    return {
        "host": os.uname().nodename.split(".")[0],
        "uptime_s": parse_uptime(read("/proc/uptime")),
        "load": [l1, l5, l15],
        "cpu_pct": cpu_pct,
        "mem_pct": parse_meminfo(read("/proc/meminfo")),
        "disk_free_gb": round(free / 1e9, 1),
        "disk_total_gb": round(cap / 1e9, 1),
        "net_down_bps": round(down, 1),
        "net_up_bps": round(up, 1),
        "top_name": top_name,
        "top_pct": top_pct,
        # Thermal zones vary wildly by board, so this stays null and the panel
        # shows "--" rather than reporting a zone that might be the wrong one.
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


# ── self-test ────────────────────────────────────────────────────────────
def selftest() -> int:
    """Check the parsers against captured /proc samples.

    Runs on any OS, which is the point: the Linux-only paths are the file reads,
    and the parsing -- where the bugs live -- is testable from a Mac.
    """
    assert parse_uptime("427174.15 3392081.55") == 427174
    assert parse_uptime("") == 0
    assert parse_loadavg("2.07 2.10 2.06 3/1234 56789") == (2.07, 2.10, 2.06)
    assert parse_loadavg("garbage") == (0.0, 0.0, 0.0)

    mem = "MemTotal: 16384000 kB\nMemFree: 500000 kB\nMemAvailable: 4096000 kB\nCached: 900000 kB\n"
    assert parse_meminfo(mem) == 75  # (16384000-4096000)/16384000
    # Pre-3.14 kernels have no MemAvailable; fall back to free+cached+buffers.
    assert parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nCached: 150 kB\nBuffers: 50 kB\n") == 70
    assert parse_meminfo("") == 0

    busy, total = parse_cpu_times("cpu  100 20 30 800 50 0 0 0 0 0\ncpu0 1 2 3 4 5\n")
    assert (busy, total) == (100 + 20 + 30, 1000), (busy, total)

    dev = (
        "Inter-|   Receive                    |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets\n"
        "    lo:  1000      10    0    0    0     0          0         0     1000      10\n"
        "  eth0:  555555    100    0    0    0     0          0         0   777777     200\n"
    )
    assert parse_net_bytes(dev, "eth0") == (555555, 777777)
    assert parse_net_bytes(dev, "wlan0") == (0, 0)

    route = (
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
        "eth0\t00000000\t0102A8C0\t0003\t0\t0\t100\t00000000\n"
        "eth0\t0002A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\n"
    )
    assert parse_default_iface(route) == "eth0"
    assert parse_default_iface("Iface\tDestination\n") == ""

    print("linux agent selftest: all parsers ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--disk", default="/", help="filesystem to report (e.g. /volume1)")
    ap.add_argument("--serial", action="store_true",
                    help="feed the board over USB instead of serving HTTP (no network at all)")
    ap.add_argument("--port-path", default=None, help="serial device, if auto-detect picks wrong")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between samples")
    ap.add_argument("--selftest", action="store_true", help="test parsers and exit")
    args = ap.parse_args()

    global DISK_PATH
    DISK_PATH = args.disk

    if args.selftest:
        raise SystemExit(selftest())

    if not os.path.isdir("/proc"):
        raise SystemExit("no /proc -- this is the Linux agent; use macos.py on a Mac")

    if args.serial:
        raise SystemExit(serial_feed(args.port_path, args.interval))

    collect()  # prime the CPU and net counters so the first poll has deltas
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"host-monitor linux agent on http://{args.bind}:{args.port}/stats"
          f"  disk={DISK_PATH}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
