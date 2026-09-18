#!/usr/bin/env python3
"""The services' logos for the social board, as raw RGB565 files on LittleFS.

Simple Icons (CC0) serves each brand mark as an SVG in a colour of your
choosing; macOS renders it to PNG (qlmanage) and the ticker's make-logos
pipeline packs it. Every mark is white: the board puts it on a rounded
plate in the brand's colour, an app icon, which reads on any theme.

    python3 tools/make-service-logos.py --size 96 --out data/logo/96
    python3 tools/make-service-logos.py --size 32 --out data/logo/32
    ./push-config social
"""
import argparse
import importlib.util
import pathlib
import subprocess
import urllib.request

HERE = pathlib.Path(__file__).absolute().parent.parent
ML = importlib.util.spec_from_file_location("ml", HERE.parent / "ticker" / "tools" / "make-logos.py")
ml = importlib.util.module_from_spec(ML)
ML.loader.exec_module(ml)

SERVICES = ["bluesky", "mastodon", "github", "npm", "youtube"]  # every mark white: the board draws it on a plate in the brand colour


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=96)
    ap.add_argument("--out", type=pathlib.Path, default=HERE / "data" / "logo")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path("/tmp/social-logos")
    tmp.mkdir(exist_ok=True)
    ok = 0
    for name in SERVICES:
        svg = tmp / f"{name}.svg"
        req = urllib.request.Request(f"https://cdn.simpleicons.org/{name}/ffffff", headers={"User-Agent": "Mozilla/5.0 (esp32-social logo fetch)"})
        with urllib.request.urlopen(req, timeout=20) as r:
            svg.write_bytes(r.read())
        subprocess.run(["qlmanage", "-t", "-s", "256", "-o", str(tmp), str(svg)], check=True, capture_output=True)
        png = (tmp / f"{name}.svg.png").read_bytes()
        w, h, px = ml.bmp_to_rgba(ml.to_bmp(png, ml.WORK, tmp))
        px, _ = ml.crop_fit(px, w, h, "alpha", False, args.size)
        (args.out / f"svc_{name}.565").write_bytes(ml.to_rgb565(px, args.size, args.size, "alpha", False))
        print(f"{name}: {args.size}px")
        ok += 1
    print(f"\n{ok} logo(s) -> {args.out}\nnow: ./push-config social")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
