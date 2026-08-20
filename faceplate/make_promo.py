#!/usr/bin/env python3
"""
Build a one-page shareable promo sheet: the real panel on the left, what every
control does on the right.

    python3 faceplate/make_promo.py

Writes faceplate/smack-versio-promo.svg (and a PNG if a renderer is around).

The panel is not redrawn here -- it is the same SVG body make_faceplate.py
emits from the measured hole coordinates in geometry.py, translated into place.
Drawing it twice would let the promo sheet drift away from the panel people
actually cut, which is the whole failure mode this avoids.

Colour groups the text by what you reach for, in the order you reach for it:
capture, then the pattern knobs, then blend, then clock, then the button. It
is the same idea as the hand-drawn Eurorack sheets this imitates, minus the
handwriting -- these are meant to be read on a phone.
"""
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import geometry as G  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
PANEL_SVG = HERE / "smack-versio-faceplate.svg"
OUT_SVG = HERE / "smack-versio-promo.svg"
OUT_PNG = HERE / "smack-versio-promo.png"

# --- canvas ---------------------------------------------------------------
W, H = 2000, 1640
PANEL_SCALE = 11.2          # panel is 50.44 x 128.34 mm; this fills the height
PANEL_X, PANEL_Y = 62, 78

INK = "#f4f7f9"
DIM = "#8b98a3"
BG = "#0b0d10"

# One colour per group. Chosen to stay legible on near-black at small sizes --
# the reference sheet's pure red/blue on white does not survive that.
CY = "#4fd8e8"   # heading / capture
GR = "#5fe08a"   # pattern
AM = "#ffb454"   # blend + clock
VI = "#c08cff"   # button
PK = "#ff7ab8"   # accent


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def panel_body():
    """The faceplate SVG's contents, minus its own <svg> wrapper."""
    raw = PANEL_SVG.read_text()
    inner = raw.split(">", 1)[1].rsplit("</svg>", 1)[0]
    return inner.strip()


out = []
add = out.append

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}" font-family="Inter, Helvetica Neue, Helvetica, Arial, sans-serif">')
add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')

# faint grid, so it reads as a drafting sheet rather than a slide
add('<defs><pattern id="g" width="40" height="40" patternUnits="userSpaceOnUse">'
    '<path d="M40 0H0V40" fill="none" stroke="#7fd0e0" stroke-opacity="0.05" stroke-width="1"/>'
    '</pattern></defs>')
add(f'<rect width="{W}" height="{H}" fill="url(#g)"/>')

# --- panel ----------------------------------------------------------------
add(f'<g transform="translate({PANEL_X},{PANEL_Y}) scale({PANEL_SCALE})">')
add(panel_body())
add('</g>')

panel_w = 50.44 * PANEL_SCALE
cap_y = PANEL_Y + 128.34 * PANEL_SCALE + 34
add(f'<text x="{PANEL_X + panel_w/2:.0f}" y="{cap_y:.0f}" font-size="19" fill="{DIM}" '
    f'text-anchor="middle">10 HP &#183; panel drawn from the measured hole centres</text>')

# --- right column ---------------------------------------------------------
X = PANEL_X + panel_w + 92
y = 96

# The wordmark and the "for ..." line were on the same baseline, with the
# offset guessed from the glyph count. It collided. Stacked instead, which
# needs no measurement to stay correct.
add(f'<text x="{X}" y="{y}" font-size="82" font-weight="700" fill="{INK}" '
    f'letter-spacing="2">SMACK</text>')
y += 42
add(f'<text x="{X}" y="{y}" font-size="31" fill="{CY}" font-weight="600">'
    f'alternative firmware for the Noise Engineering Versio</text>')
y += 40
add(f'<text x="{X}" y="{y}" font-size="27" fill="{DIM}">'
    f'Catch what you just played. Cut it up. Let a seed decide the rest.</text>')

y += 62
for line, col in [
    ("Audio runs through constantly and is always being recorded. Hold CAPTURE and the", INK),
    ("last few bars become a loop — no arming, no deciding in advance that you wanted it.", INK),
    ("That loop is sliced on a grid, the slices are re-ordered, and some of them get an", DIM),
    ("effect. Which ones, which effects, and in what order all follow from one SEED.", DIM),
]:
    add(f'<text x="{X}" y="{y}" font-size="25" fill="{col}">{esc(line)}</text>')
    y += 33

y += 26
add(f'<line x1="{X}" y1="{y}" x2="{W-70}" y2="{y}" stroke="#243039" stroke-width="2"/>')
y += 42

SECTIONS = [
    (CY, "CAPTURE", [
        ("hold ~1 s", "grab the last LENGTH steps of what you played"),
        ("tap", "re-roll — same loop, brand new pattern"),
        ("double-tap", "LIVE: re-captures itself once per loop pass"),
        ("hold 2 s", "drop the loop, back to dry passthrough"),
    ]),
    (GR, "THE PATTERN", [
        ("SEED", "which pattern. Same seed, same result, every time"),
        ("FX", "how many slices get an effect, from none to all"),
        ("ORDER", "how scrambled the slice order is"),
        ("SLICE", "how finely the loop is cut up"),
        ("LENGTH", "8 / 16 / 32 / 64 / 128 / 256 steps (256 = 16 bars)"),
        ("PITCH", "how far the pitch effects are allowed to move"),
    ]),
    (AM, "BLEND & CLOCK", [
        ("BLEND", "your live input vs the effected loop. Fully left is dry"),
        ("CLK jack", "clock in. Nothing patched? It free-runs, and that is fine"),
        ("CLK switch", "clock ratio: /2, =1, x2"),
        ("GATE switch", "clock, auto, or DUAL — two independent L/R lanes"),
    ]),
    (VI, "THE LIGHTS", [
        ("STATE", "blue idle · red recording · green looping · cyan LIVE"),
        ("PLAY", "ramps once per pass through the loop"),
        ("CLOCK", "clock source — and red if the CPU goes over 80%"),
    ]),
]

for col, title, rows in SECTIONS:
    add(f'<text x="{X}" y="{y}" font-size="27" font-weight="700" fill="{col}" '
        f'letter-spacing="1.2">{esc(title)}</text>')
    y += 36
    for name, desc in rows:
        add(f'<text x="{X + 12}" y="{y}" font-size="25" font-weight="700" fill="{col}">{esc(name)}</text>')
        add(f'<text x="{X + 232}" y="{y}" font-size="25" fill="{INK}">{esc(desc)}</text>')
        y += 32
    y += 22

# --- quick start ----------------------------------------------------------
# Numbered because this genuinely is a sequence -- each step depends on the one
# before it. The control lists above are deliberately not numbered.
y += 6
add(f'<line x1="{X}" y1="{y}" x2="{W-70}" y2="{y}" stroke="#243039" stroke-width="2"/>')
y += 40
add(f'<text x="{X}" y="{y}" font-size="27" font-weight="700" fill="{PK}" '
    f'letter-spacing="1.2">QUICK START</text>')
y += 36
for i, step in enumerate([
    "Patch audio to IN L / IN R. It passes through straight away.",
    "Play something. Hold CAPTURE for about a second.",
    "Bring up FX and ORDER, then turn SEED until you like it.",
    "BLEND decides how much of the mangling you hear.",
    "Tap CAPTURE for a new pattern. Hold 2 s to let the loop go.",
], start=1):
    add(f'<text x="{X + 12}" y="{y}" font-size="25" font-weight="700" fill="{PK}">{i}</text>')
    add(f'<text x="{X + 44}" y="{y}" font-size="25" fill="{INK}">{esc(step)}</text>')
    y += 32

# --- footer ---------------------------------------------------------------
y = max(y + 16, H - 104)
add(f'<line x1="{X}" y1="{y - 38}" x2="{W-70}" y2="{y - 38}" stroke="#243039" stroke-width="2"/>')
add(f'<text x="{X}" y="{y}" font-size="25" fill="{PK}" font-weight="700">'
    f'Free &amp; open source — timncox.github.io/smack-versio</text>')
y += 32
add(f'<text x="{X}" y="{y}" font-size="21" fill="{DIM}">'
    f'Third-party firmware. Not a Noise Engineering product and not supported by them. '
    f'Reversible: flash any official NE firmware to go back.</text>')

add('</svg>')

OUT_SVG.write_text("\n".join(out))
print(f"==> {OUT_SVG.relative_to(HERE.parent)}")

# The manual site embeds this sheet, and GitHub Pages only serves docs/. Copy
# on every build rather than by hand: the hand copy already went stale once,
# shipping a sheet whose wording the source had already corrected.
DOCS = HERE.parent / "docs"
if DOCS.is_dir():
    (DOCS / OUT_SVG.name).write_text(OUT_SVG.read_text())
    print(f"==> docs/{OUT_SVG.name}")

# --- optional PNG ---------------------------------------------------------
for cmd in (["rsvg-convert", "-w", "2000", "-o", str(OUT_PNG), str(OUT_SVG)],
            ["inkscape", str(OUT_SVG), "--export-type=png", "-w", "2000",
             f"--export-filename={OUT_PNG}"],
            ["magick", "-density", "144", str(OUT_SVG), str(OUT_PNG)]):
    try:
        subprocess.run(cmd, check=True, capture_output=True)
        print(f"==> {OUT_PNG.relative_to(HERE.parent)}")
        if DOCS.is_dir():
            (DOCS / OUT_PNG.name).write_bytes(OUT_PNG.read_bytes())
            print(f"==> docs/{OUT_PNG.name}")
        break
    except (FileNotFoundError, subprocess.CalledProcessError):
        continue
else:
    print("    (no SVG renderer found -- SVG only)")
