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


def _unpack565(buf: bytes, i: int):
    """One RGB565 pixel back to 8-bit rgb, for the visibility check."""
    v = struct.unpack_from("<H", buf, i)[0]
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def luma(r: int, g: int, b: int) -> int:
    return (299 * r + 587 * g + 114 * b) // 1000


def is_glyph(p, mode: str) -> bool:
    """Is this pixel part of the mark, as opposed to its background?

    Getting this predicate right is the whole job, because every decision below
    depends on separating mark from background -- and the background is encoded
    three different ways across one ordinary watchlist.
    """
    r, g, b, a = p
    if mode == "white":
        return min(r, g, b) <= WHITE_BG  # anything not near-white
    if mode == "alpha":
        return a >= 32  # the alpha channel says so
    return max(r, g, b) > 24  # opaque, no alpha: near-black IS the background


def classify(px, w, h):
    """Decide how this logo has to be treated. Returns (mode, invert_glyph).

    Read the background from the BORDER, never from the whole-image average.
    Three cases turn up across one normal watchlist, and a black screen
    punishes two of them:

      white   opaque light background (Apple). Knock it out to black.
      alpha   transparent background (most). Composite onto black.
      opaque  no alpha channel at all (Solana). Near-black is the background.

    Then, independently: if the mark itself is too dark, invert it. Both halves
    are needed and I got this wrong twice in opposite directions. First I
    averaged luminance over the whole image, which flipped Solana to a white
    square -- its baked-in black background counted as mark. Then I replaced
    the luminance test with border detection alone, which left Palantir, Joby,
    Rocket Lab and SpaceX invisible: transparent border, so no knock-out, and a
    solid black mark composited onto a black screen. The background tells you
    WHICH pixels are the mark; the mark's own luminance tells you whether it
    needs inverting. Two questions, not one.
    """
    ring = []
    for x in range(w):
        ring += [px[x], px[(h - 1) * w + x]]
    for y in range(h):
        ring += [px[y * w], px[y * w + w - 1]]

    opaque_ring = [p for p in ring if p[3] >= 32]
    whiteish = sum(1 for r, g, b, _ in opaque_ring if min(r, g, b) > WHITE_BG)
    # The border must be mostly OPAQUE before its colour means anything. Testing
    # only the opaque border pixels was the third version of this bug: Joby's
    # mark is near-white on a transparent background and it touches the frame
    # edge, so the handful of opaque border pixels were all pale, the image was
    # read as "white background", and the mark itself got knocked out to black.
    # A transparent border is a transparent background, whatever colour the few
    # opaque pixels in it happen to be.
    border_painted = len(opaque_ring) * 2 >= len(ring)
    if border_painted and whiteish * 2 >= len(opaque_ring):
        mode = "white"
    elif any(p[3] < 250 for p in px):
        mode = "alpha"
    else:
        mode = "opaque"

    glyph = [p for p in px if is_glyph(p, mode)]
    if not glyph:
        return mode, False  # all background, nothing to draw
    mean = sum(luma(r, g, b) for r, g, b, _ in glyph) / len(glyph)
    return mode, mean < DARK_LUMA


def to_rgb565(px, w, h, mode: str, invert_glyph: bool) -> bytes:
    """Flatten to a black background and pack little-endian RGB565.

    Little-endian because the ESP32 is, and the firmware casts the file straight
    to uint16_t* for draw16bitRGBBitmap. Byte-swapped here would show as a
    confetti-coloured logo, which is a maddening thing to debug on hardware.

    Inversion applies to the mark ONLY. Inverting the background too is exactly
    how a transparent logo turns into a white square.
    """
    out = bytearray(w * h * 2)
    for i, p in enumerate(px):
        r, g, b, a = p
        if not is_glyph(p, mode):
            r = g = b = 0  # background becomes the black screen
            a = 255
        elif invert_glyph:
            r, g, b = 255 - r, 255 - g, 255 - b
        # Alpha onto black, last, so a part-transparent edge fades to the
        # background rather than to a bright fringe.
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

    ok = skipped = faint = 0
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

        mode, invert = classify(px, w, h)
        dest = OUTDIR / f"{label}.565"
        data = to_rgb565(px, w, h, mode, invert)
        dest.write_bytes(data)

        # Verify rather than assume: the whole point of the invert rule is that
        # the mark survives on a black screen, so check that it actually did.
        # A logo that converts "successfully" into 18KB of near-black is the
        # failure this tool exists to prevent.
        vis = sum(1 for i in range(0, len(data), 2)
                  if max(_unpack565(data, i)) >= 40) * 100 // (w * h)
        warn = "  <-- WARNING: nearly invisible on black" if vis < 6 else ""
        print(f"    {w}x{h}  bg={mode}{', mark inverted' if invert else ''}"
              f"  {vis}% visible  -> {dest.relative_to(HERE)}{warn}")
        ok += 1
        if vis < 6:
            faint += 1

    total = sum(f.stat().st_size for f in OUTDIR.glob("*.565"))
    if faint:
        print(f"\n{faint} logo(s) came out nearly invisible on black -- the border "
              f"detection in classify() did not catch their background shape.")
    print(f"\n{ok} logo(s), {skipped} skipped, {total} bytes total "
          f"({total * 100 // 0xE0000}% of the 896KB data partition)")
    print("now: PLATFORMIO_DATA_DIR=apps/ticker/data pio run -e ticker -t uploadfs")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
