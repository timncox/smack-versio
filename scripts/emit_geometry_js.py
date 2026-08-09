#!/usr/bin/env python3
"""
Inject the panel geometry from faceplate/geometry.py into docs/index.html.

    python3 scripts/emit_geometry_js.py [--check]

The manual site draws the panel at true millimetre scale, which only works if
its coordinates are the same ones the faceplate generator uses. Hand-copying 24
hole positions into an HTML file is a guaranteed drift -- so the site carries a
generated block instead, between the markers below, and this script is the only
thing allowed to write it.

--check exits non-zero if the block is stale, so it can gate a release.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "faceplate"))
import geometry as G  # noqa: E402

BEGIN = "/* == BEGIN GENERATED GEOMETRY -- scripts/emit_geometry_js.py == */"
END = "/* == END GENERATED GEOMETRY == */"


def build():
    knobs = [
        {"x": x, "y": y, "d": G.KNOB_DIA, "name": n, "sub": s}
        for (x, y), (n, s) in zip(G.KNOBS, G.KNOB_LABELS)
    ]
    leds = [
        {"x": x, "y": y, "d": G.LED_DIA, "name": n}
        for (x, y), n in zip(G.LEDS, G.LED_LABELS)
    ]
    switches = [
        {"x": x, "y": y, "d": G.SWITCH_DIA, "name": n, "pos": p}
        for (x, y), (n, p) in zip(G.SWITCHES, G.SWITCH_LABELS)
    ]
    jacks = [
        {"x": x, "y": y, "d": G.JACK_DIA, "name": n}
        for (x, y), n in zip(G.jacks(), G.JACK_LABELS)
    ]
    data = {
        "panel": {"w": G.PANEL_W, "h": G.PANEL_H},
        "mounts": [
            {"x": x, "y": y, "w": G.MOUNT_W, "h": G.MOUNT_H} for x, y in G.MOUNT_HOLES
        ],
        "knobs": knobs,
        "leds": leds,
        "switches": switches,
        "button": {
            "x": G.BUTTON[0],
            "y": G.BUTTON[1],
            "d": G.BUTTON_DIA,
            "name": G.BUTTON_LABEL[0],
            "sub": G.BUTTON_LABEL[1],
            "verified": bool(getattr(G, "BUTTON_VERIFIED", True)),
        },
        "jacks": jacks,
        "jackPitchX": round(G.JACK_COLS[1] - G.JACK_COLS[0], 2),
        "jackPitchY": round(G.JACK_ROWS[1] - G.JACK_ROWS[0], 2),
    }
    return "%s\nconst PANEL = %s;\n%s" % (
        BEGIN,
        json.dumps(data, indent=2, sort_keys=False),
        END,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the page's block is stale")
    args = ap.parse_args()

    page = os.path.join(ROOT, "docs", "index.html")
    html = open(page).read()
    block = build()

    pat = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END), re.S)
    if not pat.search(html):
        print("error: markers not found in docs/index.html", file=sys.stderr)
        return 2

    updated = pat.sub(lambda _: block, html)
    if updated == html:
        print("geometry block is current")
        return 0
    if args.check:
        print("geometry block is STALE -- run scripts/emit_geometry_js.py",
              file=sys.stderr)
        return 1

    open(page, "w").write(updated)
    print(f"updated {page}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
