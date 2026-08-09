#!/usr/bin/env python3
"""
Generate the Smack Versio faceplate overlay.

    python3 faceplate/make_faceplate.py [--light] [--outdir DIR]

Writes two SVGs, both in real millimetres:

    smack-versio-faceplate.svg   the panel alone, 50.44 x 128.34 mm
    smack-versio-printsheet.svg  the panel on A6, with trim marks, a 50 mm
                                 calibration ruler, and the fitting steps

Print the *print sheet*, not the panel. The ruler is the point: every way this
can go wrong -- "fit to page", "shrink to printable area", a driver that
silently scales to Letter -- shows up as a ruler that is not 50 mm long, and
none of them are visible any other way. Check the ruler before you cut.

Geometry and its provenance live in geometry.py. Read that file before
trusting a hole position; the switch and button are marked MEDIUM confidence.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geometry as G

# --- palette --------------------------------------------------------------

DARK = dict(
    bg="#141414", ink="#f2f2f2", dim="#8a8a8a",
    accent="#e8532a", hole="#000000", holeline="#5a5a5a",
)
LIGHT = dict(
    bg="#ffffff", ink="#111111", dim="#666666",
    accent="#c0341a", hole="#ffffff", holeline="#999999",
)

FONT = "Helvetica, Arial, sans-serif"


def circle(cx, cy, dia, fill, stroke, sw=0.18, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{dia/2:.2f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def text(x, y, s, size, fill, weight="normal", anchor="middle", spacing=0):
    ls = f' letter-spacing="{spacing}"' if spacing else ""
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-family="{FONT}" '
            f'font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}"{ls}>{s}</text>')


def panel_body(P):
    """The panel artwork itself, drawn at origin (0,0). Returns SVG fragment."""
    o = []
    a = o.append

    # plate
    a(f'<rect x="0" y="0" width="{G.PANEL_W}" height="{G.PANEL_H}" rx="1.2" '
      f'fill="{P["bg"]}"/>')

    # mounting slots
    for (x, y) in G.MOUNT_HOLES:
        a(f'<rect x="{x-G.MOUNT_W/2:.2f}" y="{y-G.MOUNT_H/2:.2f}" '
          f'width="{G.MOUNT_W}" height="{G.MOUNT_H}" rx="{G.MOUNT_H/2:.2f}" '
          f'fill="{P["hole"]}" stroke="{P["holeline"]}" stroke-width="0.15"/>')

    # title
    a(text(G.PANEL_W / 2, 9.2, "SMACK", 4.4, P["ink"], "bold", spacing=0.55))
    a(text(G.PANEL_W / 2, 12.4, "loop &#183; slice &#183; glitch", 1.9, P["dim"]))

    # LEDs, with a hairline bracket so their meaning is readable at a glance
    # 1.2 mm, not 1.4: at 1.4 "BLEND" and "CLOCK" touch, since the LEDs are
    # only 5.07 mm apart.
    for (x, y), lab in zip(G.LEDS, G.LED_LABELS):
        a(circle(x, y, G.LED_DIA, P["hole"], P["holeline"], 0.15))
        a(text(x, y + 3.8, lab, 1.2, P["dim"]))

    # knobs
    for (x, y), (name, sub) in zip(G.KNOBS, G.KNOB_LABELS):
        a(circle(x, y, G.KNOB_DIA, P["hole"], P["holeline"], 0.18, dash="0.6 0.5"))
        # label below the cap, which overhangs the bushing hole
        a(text(x, y + 7.4, name, 2.5, P["ink"], "bold"))
        a(text(x, y + 9.9, sub, 1.7, P["dim"]))

    # switches
    for (x, y), (name, poss) in zip(G.SWITCHES, G.SWITCH_LABELS):
        a(circle(x, y, G.SWITCH_DIA, P["hole"], P["holeline"], 0.15))
        a(text(x + 4.6, y - 1.6, name, 2.0, P["ink"], "bold", anchor="start"))
        a(text(x + 4.6, y + 0.8, " / ".join(poss), 1.5, P["dim"], anchor="start"))

    # button - centre column, so its label goes underneath like the knobs.
    # (Alongside would run into the PITCH knob, which is only ~13 mm away.)
    bx, by = G.BUTTON
    unver = not getattr(G, "BUTTON_VERIFIED", True)
    a(circle(bx, by, G.BUTTON_DIA, P["hole"], P["accent"] if unver else P["holeline"],
             0.2, dash="0.7 0.5" if unver else None))
    a(text(bx, by + G.BUTTON_DIA / 2 + 2.4, G.BUTTON_LABEL[0], 2.1, P["accent"], "bold"))
    a(text(bx, by + G.BUTTON_DIA / 2 + 4.5, G.BUTTON_LABEL[1], 1.5, P["dim"]))

    # jacks
    for (x, y), lab in zip(G.jacks(), G.JACK_LABELS):
        a(circle(x, y, G.JACK_DIA, P["hole"], P["holeline"], 0.18))
        a(text(x, y - 4.6, lab, 1.75, P["ink"], "bold"))

    # footer
    a(text(G.PANEL_W / 2, G.PANEL_H - 5.6, "third-party firmware", 1.4, P["dim"]))
    a(text(G.PANEL_W / 2, G.PANEL_H - 3.4, "not a Noise Engineering product",
           1.4, P["dim"]))
    return "\n".join(o)


def svg_panel(P):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{G.PANEL_W}mm" height="{G.PANEL_H}mm" '
        f'viewBox="0 0 {G.PANEL_W} {G.PANEL_H}">\n'
        f'{panel_body(P)}\n</svg>\n'
    )


def svg_printsheet(P):
    """A6 sheet: panel + trim marks + the calibration ruler."""
    W, H = 105.0, 148.0
    px = (W - G.PANEL_W) / 2      # centre horizontally
    py = 8.0
    o = []
    a = o.append
    a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
      f'viewBox="0 0 {W} {H}">')
    a(f'<rect x="0" y="0" width="{W}" height="{H}" fill="#ffffff"/>')

    # trim marks, outside the panel so cutting does not remove them early
    m = 2.5
    for (cx, cy) in [(px, py), (px + G.PANEL_W, py),
                     (px, py + G.PANEL_H), (px + G.PANEL_W, py + G.PANEL_H)]:
        a(f'<path d="M{cx-m:.2f},{cy:.2f} L{cx-0.8:.2f},{cy:.2f} '
          f'M{cx+0.8:.2f},{cy:.2f} L{cx+m:.2f},{cy:.2f} '
          f'M{cx:.2f},{cy-m:.2f} L{cx:.2f},{cy-0.8:.2f} '
          f'M{cx:.2f},{cy+0.8:.2f} L{cx:.2f},{cy+m:.2f}" '
          f'stroke="#000000" stroke-width="0.12" fill="none"/>')

    a(f'<g transform="translate({px:.2f},{py:.2f})">{panel_body(P)}</g>')

    # --- calibration ruler, 50 mm with 10 mm ticks ---
    ry = py + G.PANEL_H + 12.0
    rx = (W - 50.0) / 2
    a(f'<path d="M{rx:.2f},{ry:.2f} L{rx+50:.2f},{ry:.2f}" stroke="#000000" '
      f'stroke-width="0.25" fill="none"/>')
    for i in range(6):
        x = rx + i * 10.0
        a(f'<path d="M{x:.2f},{ry:.2f} L{x:.2f},{ry+2.6:.2f}" stroke="#000000" '
          f'stroke-width="0.25"/>')
        a(text(x, ry + 5.6, str(i * 10), 2.4, "#000000"))
    a(text(W / 2, ry - 2.4,
           "CHECK THIS FIRST &#8212; this line must measure exactly 50 mm",
           2.6, "#000000", "bold"))
    a(text(W / 2, ry + 9.4,
           "If it does not, reprint at 100% / &#8220;Actual size&#8221;. "
           "Do not cut until it does.", 2.2, "#444444"))

    # --- fitting notes ---
    ny = ry + 16.0
    notes = [
        "1.  Verify the ruler above. Everything else depends on it.",
        "2.  Cut the outline, then the holes. Dashed circles are knob bushings;",
        "     the knob caps are wider than the hole, so pull the caps first.",
        "3.  Hole positions come from a 1:1 template; the CAPTURE button was placed",
        "     from the hardware by eye. Good enough to fit — measure before you",
        "     machine anything. See MEASURE.md.",
        "4.  Fit behind the knobs and switches, or trim and use it as a bench card.",
        "5.  Turn each knob and watch which LED moves. If the legend is scrambled,",
        "     the knob-to-ADC order in smack_versio.cpp is wrong, not this print.",
    ]
    for i, line in enumerate(notes):
        a(text(6.0, ny + i * 3.3, line, 2.2, "#333333", anchor="start"))

    a('</svg>')
    return "\n".join(o) + "\n"


def check_geometry():
    """
    Refuse to emit a physically impossible panel.

    This exists because the first draft placed the button 0.6 mm inside the
    second switch -- two holes that cannot both be drilled. That was caught by
    looking at a render, which is not a thing that reliably happens. Overlap is
    cheap to test, so test it.

    Returns a list of complaints; empty means the panel could exist.
    """
    # (name, cx, cy, half_width, half_height). Mounting slots are ovals, not
    # circles -- treating them as circles of their long axis reports a false
    # edge violation, since they legitimately sit ~1.4 mm from the plate edge.
    holes = []
    holes += [(f"knob {i}", x, y, G.KNOB_DIA / 2, G.KNOB_DIA / 2)
              for i, (x, y) in enumerate(G.KNOBS)]
    holes += [(f"led {i}", x, y, G.LED_DIA / 2, G.LED_DIA / 2)
              for i, (x, y) in enumerate(G.LEDS)]
    holes += [(f"switch {i}", x, y, G.SWITCH_DIA / 2, G.SWITCH_DIA / 2)
              for i, (x, y) in enumerate(G.SWITCHES)]
    holes += [("button", G.BUTTON[0], G.BUTTON[1],
               G.BUTTON_DIA / 2, G.BUTTON_DIA / 2)]
    holes += [(f"jack {i}", x, y, G.JACK_DIA / 2, G.JACK_DIA / 2)
              for i, (x, y) in enumerate(G.jacks())]
    holes += [(f"mount {i}", x, y, G.MOUNT_W / 2, G.MOUNT_H / 2)
              for i, (x, y) in enumerate(G.MOUNT_HOLES)]

    EDGE = 1.0  # leave this much plate around every hole
    bad = []
    for i, (ni, xi, yi, wi, hi) in enumerate(holes):
        if (xi - wi < EDGE or xi + wi > G.PANEL_W - EDGE
                or yi - hi < EDGE or yi + hi > G.PANEL_H - EDGE):
            bad.append(f"{ni} is off the plate or within {EDGE} mm of its edge")
        for nj, xj, yj, wj, hj in holes[i + 1:]:
            # conservative: overlap only if they collide on *both* axes
            ox = (wi + wj) - abs(xi - xj)
            oy = (hi + hj) - abs(yi - yj)
            if ox > 0 and oy > 0:
                bad.append(f"{ni} and {nj} overlap by "
                           f"{min(ox, oy):.2f} mm")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--light", action="store_true",
                    help="white plate (saves ink); default matches the module")
    ap.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)))
    args = ap.parse_args()

    problems = check_geometry()
    if problems:
        print("geometry is not physically buildable:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    P = LIGHT if args.light else DARK
    os.makedirs(args.outdir, exist_ok=True)

    for name, body in [("smack-versio-faceplate.svg", svg_panel(P)),
                       ("smack-versio-printsheet.svg", svg_printsheet(P))]:
        path = os.path.join(args.outdir, name)
        with open(path, "w") as f:
            f.write(body)
        print(f"wrote {path}")

    print(f"\npanel {G.PANEL_W} x {G.PANEL_H} mm  "
          f"({len(G.KNOBS)} knobs, {len(G.LEDS)} LEDs, {len(G.jacks())} jacks, "
          f"{len(G.SWITCHES)} switches, 1 button)")
    print("print the PRINT SHEET at 100% and check the 50 mm ruler before cutting.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
