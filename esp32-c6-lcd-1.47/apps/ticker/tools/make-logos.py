#!/usr/bin/env python3
"""Fetch the watchlist's logos and convert them to raw RGB565 for LittleFS.

Run this on the Mac, then `uploadfs`. The firmware does no PNG decoding and no
logo fetching: it opens /logo/<LABEL>.565, reads 96*96*2 bytes, and blits them.
That keeps a PNG decoder, a second TLS endpoint and an 18KB decode buffer out of
a 512KB single-core part, and it means a logo that fails to download is a
missing file the firmware already handles rather than a runtime failure.

Sources, all tested, all keyless:
  stocks  https://financialmodelingprep.com/image-stock/<SYMBOL>.png
  coins   https://assets.coincap.io/assets/icons/<label lowercased>@2x.png

logo.clearbit.com is dead (DNS no longer resolves) and img.logo.dev wants a
token, so neither is used.

Stdlib only, plus macOS `sips` for the decode/resize -- Pillow is not installed
and this is not worth a pip install.

Usage:
    python3 tools/make-logos.py            # from apps/ticker/
    python3 tools/make-logos.py --size 64  # smaller, if flash gets tight
"""

import argparse
import json
import pathlib
import struct
import subprocess
import sys
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent.parent
WATCHLIST = HERE / "data" / "watchlist.json"
OUTDIR = HERE / "data" / "logo"

STOCK_URL = "https://financialmodelingprep.com/image-stock/{}.png"
COIN_URL = "https://assets.coincap.io/assets/icons/{}@2x.png"

# A glyph darker than this, sitting on a light background, is inverted: Apple's
# mark is solid black, and flattening black-on-white onto a black screen gives a
# perfectly valid file that draws nothing. Deciding it here costs the firmware
# zero bytes. WHITE_BG is the per-channel floor for "this pixel is background".
DARK_LUMA = 70
WHITE_BG = 200

UA = {"User-Agent": "Mozilla/5.0 (esp32-ticker logo fetch)"}


def fetch(url: str) -> bytes | None:
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=20) as r:
            return r.read()
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as e:
        print(f"    fetch failed: {e}")
        return None


def to_bmp(png: bytes, size: int, tmp: pathlib.Path) -> bytes:
    """Resize and decode via sips, preserving alpha (32bpp BI_BITFIELDS)."""
    src, dst = tmp / "in.png", tmp / "out.bmp"
    src.write_bytes(png)
    subprocess.run(
        ["sips", "-z", str(size), str(size), "-s", "format", "bmp", str(src), "--out", str(dst)],
        check=True, capture_output=True,
    )
    return dst.read_bytes()


def bmp_to_rgba(d: bytes):
    """Parse the BMP that sips emits. Returns (w, h, pixels).

    sips picks the depth from the source: 32bpp BI_BITFIELDS when the PNG has an
    alpha channel, plain 24bpp BI_RGB when it doesn't. Both turn up in one run
    of a normal watchlist, so both are handled.
    """
    if d[:2] != b"BM":
        raise ValueError("not a BMP")
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    bpp, comp = struct.unpack_from("<H", d, 28)[0], struct.unpack_from("<I", d, 30)[0]
    if bpp not in (24, 32):
        raise ValueError(f"expected 24 or 32bpp, got {bpp}")

    top_down = h < 0            # negative height means the first row is the top
    h = abs(h)
    px = []

    if bpp == 24:
        stride = (w * 3 + 3) // 4 * 4  # rows are padded to 4 bytes
        for y in range(h):
            base = off + (y if top_down else h - 1 - y) * stride
            for x in range(w):
                b, g, r = d[base + x * 3], d[base + x * 3 + 1], d[base + x * 3 + 2]
                px.append((r, g, b, 255))  # no alpha channel: fully opaque
        return w, h, px

    # BI_BITFIELDS (3) puts the channel masks in a V4/V5 header. BI_RGB (0) is
    # plain BGRA. Don't assume channel order -- derive it from the masks.
    if comp == 3:
        rm, gm, bm, am = struct.unpack_from("<IIII", d, 54)
    else:
        rm, gm, bm, am = 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
    shifts = [(m.bit_length() - 8) if m else 0 for m in (rm, gm, bm, am)]

    stride = w * 4
    for y in range(h):
        base = off + (y if top_down else h - 1 - y) * stride
        for x in range(w):
            v, = struct.unpack_from("<I", d, base + x * 4)
            r = (v & rm) >> shifts[0] if rm else 0
            g = (v & gm) >> shifts[1] if gm else 0
            b = (v & bm) >> shifts[2] if bm else 0
            a = ((v & am) >> shifts[3]) if am else 255
            px.append((r & 255, g & 255, b & 255, a & 255))
    return w, h, px


def luma(r: int, g: int, b: int) -> int:
    return (299 * r + 587 * g + 114 * b) // 1000


def classify(px, w, h):
    """Decide how this logo has to be treated. Returns (white_bg, invert_glyph).

    Read the background from the BORDER, never from the whole-image average.
    Averaging was the first attempt and it was quietly wrong: Solana's PNG has
    no alpha channel, so its black background counted as visible pixels, the
    mean came out at luma 24, and the "too dark, invert it" rule flipped the
    entire image to a white square. A file that looks fine and renders wrong.

    Two real cases turn up in one ordinary watchlist:
      transparent/black border -> composite onto black, change nothing
      opaque white border      -> the logo was drawn for a light background, so
                                  knock the white out to black, and invert the
                                  glyph if it is too dark to show up (Apple's
                                  mark is solid black).
    """
    ring = []
    for x in range(w):
        ring += [px[x], px[(h - 1) * w + x]]
    for y in range(h):
        ring += [px[y * w], px[y * w + w - 1]]

    opaque = [p for p in ring if p[3] >= 32]
    if not opaque:
        return False, False  # fully transparent border: the ordinary case
    whiteish = sum(1 for r, g, b, _ in opaque if min(r, g, b) > WHITE_BG)
    if whiteish * 2 < len(opaque):
        return False, False

    glyph = [p for p in px if p[3] >= 32 and min(p[0], p[1], p[2]) <= WHITE_BG]
    if not glyph:
        return True, False  # all background, nothing to draw
    mean = sum(luma(r, g, b) for r, g, b, _ in glyph) / len(glyph)
    return True, mean < DARK_LUMA


def to_rgb565(px, w, h, white_bg: bool, invert_glyph: bool) -> bytes:
    """Flatten to a black background and pack little-endian RGB565.

    Little-endian because the ESP32 is, and the firmware casts the file straight
    to uint16_t* for draw16bitRGBBitmap. Byte-swapped here would show as a
    confetti-coloured logo, which is a maddening thing to debug on hardware.
    """
    out = bytearray(w * h * 2)
    for i, (r, g, b, a) in enumerate(px):
        if white_bg:
            if min(r, g, b) > WHITE_BG:
                r = g = b = 0  # the light background becomes the black screen
            elif invert_glyph:
                r, g, b = 255 - r, 255 - g, 255 - b
            a = 255
        # Alpha onto black, last, so a transparent pixel stays black instead of
        # becoming a white box.
        r, g, b = r * a // 255, g * a // 255, b * a // 255
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        struct.pack_into("<H", out, i * 2, v)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=96, help="square logo size in px (default 96)")
    args = ap.parse_args()

    wl = json.loads(WATCHLIST.read_text())
    targets = [(s, STOCK_URL.format(s)) for s in wl.get("stocks", [])]
    for c in wl.get("coins", []):
        label = c.get("label") or c["id"]
        # coincap keys on the ticker, not the CoinGecko id, so the label is what
        # we have to go on. A custom label that isn't a ticker simply won't
        # resolve, and the firmware falls back to drawing the text.
        targets.append((label, COIN_URL.format(label.lower())))

    OUTDIR.mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path("/tmp/ticker-logos")
    tmp.mkdir(exist_ok=True)

    ok = skipped = 0
    for label, url in targets:
        print(f"{label}:")
        png = fetch(url)
        if not png:
            skipped += 1
            continue
        try:
            w, h, px = bmp_to_rgba(to_bmp(png, args.size, tmp))
        except (subprocess.CalledProcessError, ValueError) as e:
            print(f"    convert failed: {e}")
            skipped += 1
            continue

        white_bg, invert = classify(px, w, h)
        dest = OUTDIR / f"{label}.565"
        dest.write_bytes(to_rgb565(px, w, h, white_bg, invert))
        note = ("white background knocked out" if white_bg and not invert
                else "white background knocked out, dark glyph inverted" if white_bg
                else "transparent, composited onto black")
        print(f"    {w}x{h}  {note}"
              f"  -> {dest.relative_to(HERE)} ({dest.stat().st_size} bytes)")
        ok += 1

    total = sum(f.stat().st_size for f in OUTDIR.glob("*.565"))
    print(f"\n{ok} logo(s), {skipped} skipped, {total} bytes total "
          f"({total * 100 // 0xE0000}% of the 896KB data partition)")
    print("now: PLATFORMIO_DATA_DIR=apps/ticker/data pio run -e ticker -t uploadfs")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
