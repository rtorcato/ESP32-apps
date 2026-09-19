#!/usr/bin/env python3
"""Render each app's preview.svg to raw RGB565 for Home's detail pages.

Same trick as make-logos.py and for the same reason: the firmware decodes
nothing. It opens /preview/<id>.565, reads W*H*2 bytes and blits them. An SVG
parser, a PNG decoder and a scaler are all things a 240MHz part should not be
doing to draw a picture that never changes.

rsvg-convert does the SVG (brew install librsvg), then macOS `sips` decodes and
sizes it, the same path make-logos.py already uses -- Pillow is not installed
on this Mac and this is not worth a pip install for.

    python3 tools/make-previews.py          # from apps/home/
    python3 tools/make-previews.py --width 320

Missing rsvg-convert is not fatal: Home draws a placeholder for any app with no
preview file, so a board without these still works.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).absolute().parent
APPS = HERE.parent.parent  # apps/


def bmp_to_rgb565(data: bytes) -> tuple[int, int, bytes]:
    """Parse the BMP sips emits (24bpp BI_RGB or 32bpp BI_BITFIELDS).

    Rows are padded to 4 bytes. A NEGATIVE height in the header means the rows
    are stored top-down rather than the usual bottom-up, which is what sips
    actually writes here -- reading it as bottom-up silently yields nothing.
    Alpha is composited onto black; the panel has nothing behind the image.
    """
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError(f"unexpected {bpp}bpp")
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) & ~3
    out = bytearray()
    for y in (range(h) if top_down else range(h - 1, -1, -1)):
        row = off + y * stride
        for x in range(w):
            p = row + x * (bpp // 8)
            b, g, r = data[p], data[p + 1], data[p + 2]
            if bpp == 32:
                a = data[p + 3]
                if a != 255:
                    r, g, b = r * a // 255, g * a // 255, b * a // 255
            out += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return w, h, bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=400, help="pixels; height follows the 800x480 panel ratio")
    ap.add_argument("--out", default=str(HERE.parent / "data" / "preview"))
    args = ap.parse_args()
    height = args.width * 480 // 800

    if not subprocess.run(["which", "rsvg-convert"], capture_output=True).returncode == 0:
        print("make-previews: rsvg-convert not found -- brew install librsvg", file=sys.stderr)
        print("               Home draws a placeholder without these; not fatal.", file=sys.stderr)
        return 1

    outdir = pathlib.Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    made = 0
    for svg in sorted(APPS.glob("*/preview.svg")):
        app = svg.parent.name
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            png, bmp = tmp / "p.png", tmp / "p.bmp"
            subprocess.run(
                ["rsvg-convert", "-w", str(args.width), "-h", str(height), "-b", "#000000", str(svg), "-o", str(png)],
                check=True,
            )
            subprocess.run(["sips", "-s", "format", "bmp", str(png), "--out", str(bmp)], check=True, capture_output=True)
            w, h, px = bmp_to_rgb565(bmp.read_bytes())
        dst = outdir / f"{app}.565"
        dst.write_bytes(px)
        print(f"  {app:10} {w}x{h}  {len(px) // 1024}KB  -> {dst.relative_to(APPS.parent)}")
        made += 1

    if not made:
        print("make-previews: no apps/*/preview.svg found", file=sys.stderr)
        return 1
    print(f"make-previews: {made} previews at {args.width}x{height}. Now: ./push-config home")
    return 0


if __name__ == "__main__":
    sys.exit(main())
