#!/usr/bin/env python3
"""Team logos for the sports board, as raw RGB565 files on LittleFS.

Reads the leagues from data/config.json, asks ESPN's header scoreboard for
each (the feed the board itself reads), collects every team playing this
week with its logo URL, and converts each through the ticker's make-logos
pipeline (decode, classify the background, crop to the mark, box-filter
down, pack RGB565). Files are named <league>_<ABBR>.565 -- TOR is a
different team in the NHL and the NBA.

    python3 tools/make-team-logos.py --size 32 --out data/logo/32
    python3 tools/make-team-logos.py --size 128 --out data/logo/128
    ./push-config sports
"""
import argparse
import importlib.util
import json
import pathlib
import urllib.request

HERE = pathlib.Path(__file__).absolute().parent.parent
ML = importlib.util.spec_from_file_location("ml", HERE.parent / "ticker" / "tools" / "make-logos.py")
ml = importlib.util.module_from_spec(ML)
ML.loader.exec_module(ml)

HEADER = "https://site.web.api.espn.com/apis/v2/scoreboard/header?sport={}&league={}"
# The sports' own pictures, for the splash and the tabs: Twemoji (CC-BY 4.0),
# a coloured graphic each, converted like a logo. --sports makes these.
TWEMOJI = "https://cdn.jsdelivr.net/gh/twitter/twemoji@14.0.2/assets/72x72/{}.png"
SPORT_EMOJI = {"football": "1f3c8", "hockey": "1f3d2", "basketball": "1f3c0", "baseball": "26be", "soccer": "26bd",
               "tennis": "1f3be", "golf": "26f3", "rugby": "1f3c9", "cricket": "1f3cf", "volleyball": "1f3d0"}


def teams(sport: str, league: str):
    req = urllib.request.Request(HEADER.format(sport, league), headers={"User-Agent": "Mozilla/5.0 (esp32-sports logo fetch)"})
    with urllib.request.urlopen(req, timeout=20) as r:
        d = json.load(r)
    out = {}
    for sp in d.get("sports", []):
        for lg in sp.get("leagues", []):
            for e in lg.get("events", []):
                for c in e.get("competitors", []):
                    ab, logo = c.get("abbreviation"), c.get("logo")
                    if ab and logo:
                        out[ab] = logo
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=32)
    ap.add_argument("--out", type=pathlib.Path, default=HERE / "data" / "logo")
    ap.add_argument("--sports", action="store_true", help="the sports' pictures (sport_<name>.565) instead of the teams")
    args = ap.parse_args()
    cfg = json.loads((HERE / "data" / "config.json").read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path("/tmp/sports-logos")
    tmp.mkdir(exist_ok=True)
    ok = skipped = 0
    if args.sports:
        sports = sorted({lg["sport"] for lg in cfg.get("leagues", [])})
        for sp in sports:
            code = SPORT_EMOJI.get(sp)
            if not code:
                print(f"{sp}: no picture known, the board draws its own")
                continue
            png = ml.fetch(TWEMOJI.format(code))
            if not png:
                skipped += 1
                continue
            w, h, px = ml.bmp_to_rgba(ml.to_bmp(png, ml.WORK, tmp))
            px, _ = ml.crop_fit(px, w, h, "alpha", False, args.size)
            (args.out / f"sport_{sp}.565").write_bytes(ml.to_rgb565(px, args.size, args.size, "alpha", False))
            print(f"{sp}: {args.size}px")
            ok += 1
        print(f"\n{ok} picture(s), {skipped} skipped -> {args.out}")
        return 0 if ok else 1
    for lg in cfg.get("leagues", []):
        try:
            tm = teams(lg["sport"], lg["id"])
        except Exception as e:  # noqa: BLE001
            print(f"{lg['id']}: feed failed: {e}")
            continue
        print(f"{lg['id']}: {len(tm)} teams this week")
        for ab, url in sorted(tm.items()):
            dest = args.out / f"{lg['id']}_{ab}.565"
            png = ml.fetch(url)
            if not png:
                skipped += 1
                continue
            try:
                w, h, px = ml.bmp_to_rgba(ml.to_bmp(png, ml.WORK, tmp))
            except Exception as e:  # noqa: BLE001
                print(f"    {ab}: convert failed: {e}")
                skipped += 1
                continue
            mode, invert = ml.classify(px, w, h)
            px, _ = ml.crop_fit(px, w, h, mode, invert, args.size)
            dest.write_bytes(ml.to_rgb565(px, args.size, args.size, mode, invert))
            ok += 1
    total = sum(f.stat().st_size for f in args.out.glob("*.565"))
    print(f"\n{ok} logo(s), {skipped} skipped, {total} bytes in {args.out}")
    print("now: ./push-config sports")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
