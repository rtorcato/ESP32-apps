#!/usr/bin/env python3
"""Render each app's preview.svg to raw RGB565 for Home's detail pages.

Same trick as make-logos.py and for the same reason: the firmware decodes
nothing. It opens /preview/<id>.565, reads W*H*2 bytes and blits them. An SVG
parser, a PNG decoder and a scaler are all things a 240MHz part should not be
doing to draw a picture that never changes.

rsvg-convert does the SVG (brew install librsvg / apt install librsvg2-bin) and
the PNG it emits is decoded here in pure Python -- zlib plus forty lines of
un-filtering. make-logos.py shells out to macOS `sips` for the same job, which
works on this Mac and nowhere else; CI runs on Linux and has to render these
too, so this one carries its own decoder rather than growing a second
platform branch.

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
import zlib

HERE = pathlib.Path(__file__).absolute().parent
APPS = HERE.parent.parent  # apps/


def _unfilter(raw: bytes, w: int, h: int, bpp: int) -> bytearray:
    """Undo the per-scanline filter PNG applies before deflate.

    Five filter types, each predicting a byte from its left (a), above (b)
    and above-left (c) neighbours. This is the whole of PNG decoding that is
    not zlib.
    """
    stride = w * bpp
    out = bytearray(stride * h)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        up = out[(y - 1) * stride:y * stride] if y else bytearray(stride)
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = up[i]
            c = up[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out[y * stride:(y + 1) * stride] = line
    return out


def png_to_rgb565(data: bytes) -> tuple[int, int, bytes]:
    """Decode the PNG rsvg-convert writes: 8-bit RGB or RGBA, non-interlaced.

    Alpha is composited onto black because the panel has nothing behind the
    image to show through.
    """
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, bytearray(), None
    while pos < len(data):
        ln = struct.unpack_from(">I", data, pos)[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            if depth != 8 or colour not in (2, 6) or interlace:
                raise ValueError(f"unsupported PNG: depth {depth}, colour {colour}, interlace {interlace}")
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if w is None:
        raise ValueError("PNG had no IHDR")
    bpp = 4 if colour == 6 else 3
    px = _unfilter(zlib.decompress(bytes(idat)), w, h, bpp)
    out = bytearray()
    for i in range(0, len(px), bpp):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if bpp == 4:
            a = px[i + 3]
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

    if subprocess.run(["which", "rsvg-convert"], capture_output=True).returncode != 0:
        print("make-previews: rsvg-convert not found -- brew install librsvg (or apt install librsvg2-bin)",
              file=sys.stderr)
        print("               Home draws a placeholder without these; not fatal.", file=sys.stderr)
        return 1

    outdir = pathlib.Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    made = 0
    for svg in sorted(APPS.glob("*/preview.svg")):
        app = svg.parent.name
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            png = tmp / "p.png"
            subprocess.run(
                ["rsvg-convert", "-w", str(args.width), "-h", str(height), "-b", "#000000", str(svg), "-o", str(png)],
                check=True,
            )
            w, h, px = png_to_rgb565(png.read_bytes())
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
