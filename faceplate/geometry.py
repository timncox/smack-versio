"""
Versio panel geometry, in millimetres, origin at the panel's top-left corner.

PROVENANCE — read this before trusting a number in here
-------------------------------------------------------
These coordinates were *derived*, not measured with calipers. The method:

  1. Page 5 of the FRGMNTS Versio manual (Acidclank) is a 1:1 faceplate print
     template — it says so in its own footer.
  2. `pdftocairo -svg` converts that page to vector paths.
  3. Circles survive as 4-arc bezier paths whose control points share the
     circle's bounding box, so a path bbox gives an exact hole centre and
     diameter.

The frame is self-validating, which is why it is trustworthy at all: the
derived panel outline comes out at 50.44 x 128.34 mm (10 HP x 3 U) and the
four mounting slots land 2.99 mm and 125.33 mm from the top edge and 7.60 mm
from the left. The Eurorack standard is a 3.0 mm inset with the first hole at
7.5 mm. Those four numbers were not fitted to anything — they fell out of the
same transform as everything else, and they agree with the spec to a hundredth
of a millimetre. An origin that reproduces the standard mounting pattern is an
origin that is placed correctly.

CONFIDENCE, PER GROUP
  panel, mounting  HIGH   - matches the Eurorack mechanical standard exactly
  jacks            HIGH   - 12 holes, one diameter, a clean 3x4 grid
  knobs            HIGH   - 7 holes at 8.99 mm, the standard pot bushing
  leds             HIGH   - 4 holes at 3.0 mm in a row, centred on the panel
  switches, button MEDIUM - inferred by elimination from the remaining shapes;
                            the FRGMNTS artwork has decorative rings that are
                            hard to tell apart from real hardware. VERIFY.

We are extracting mechanical facts about Noise Engineering's hardware in order
to draw original artwork. The FRGMNTS template's own footer restricts
*manufacturing or selling panels* from their data; nothing here reproduces
their graphics, and this overlay is not for sale. See MEASURE.md if you want to
replace every number below with your own caliper readings.
"""

# --- panel ----------------------------------------------------------------
PANEL_W = 50.44
PANEL_H = 128.34

# Oval M3 slots. Eurorack standard: 3.0 mm from the top and bottom edges.
MOUNT_HOLES = [(7.60, 2.99), (43.11, 2.99), (7.60, 125.33), (43.11, 125.33)]
MOUNT_W, MOUNT_H = 5.47, 3.20

# --- controls -------------------------------------------------------------
KNOB_DIA = 8.99  # bushing hole; the knob cap is wider

# Reading order, top-left to bottom-right. The three columns land on
# x = 7.72 / 25.10 / 43.30, symmetric about the panel centre (25.22).
#
# WARNING, and this is the one that will actually bite: this is *panel* order.
# Which physical knob is libDaisy's KNOB_0 is a separate question that no
# amount of PDF archaeology can answer -- it depends on the Versio's ADC
# channel wiring. smack_versio.cpp assumes panel reading order. If the legend
# comes out scrambled on the bench, that assumption is what is wrong, not this
# file. MEASURE.md has the two-minute check.
KNOBS = [
    (7.72, 18.50),
    (43.30, 18.50),
    (25.10, 28.65),
    (7.72, 39.43),
    (43.30, 39.43),
    (25.10, 49.31),
    (43.30, 60.36),
]

LED_DIA = 3.00
LED_Y = 19.77
LEDS = [(17.23, LED_Y), (22.30, LED_Y), (29.28, LED_Y), (34.35, LED_Y)]

# MEDIUM confidence - see the module docstring.
SWITCH_DIA = 5.10
SWITCHES = [(8.35, 57.82), (8.35, 67.34)]
SWITCH_PITCH = SWITCHES[1][1] - SWITCHES[0][1]  # 9.52 mm

# LOW confidence, and the print says so.
#
# The obvious candidate in the artwork -- an 18.52 mm circle centred at
# (9.30, 72.27) -- cannot be the button hole: an 18.5 mm hole is far too big
# for a momentary, and placing a 6 mm button at that centre puts its edge
# 0.6 mm *inside* the second switch. That is physically impossible, so the
# circle is decoration (a printed ring), not hardware, and the extraction
# gives us no other candidate.
#
# The first guess -- continue the switch column by one more SWITCH_PITCH --
# does not fit either: it lands 0.07 mm inside the top jack row. (The overlap
# check in make_faceplate.py found that; it is exactly the kind of error that
# survives a visual once-over.)
#
# What is left is a constraint, not a measurement. In this column the button
# must clear the switch above (67.34 + 2.55) and the jack row below
# (83.19 - 3.40), which pins its centre to the range 72.9 .. 76.8 mm. We take
# the middle of that window. It is the only region the *derived* hardware
# leaves open, so it is a reasoned placement -- but nothing here observed a
# button, and it is drawn dashed and flagged on the print for that reason.
BUTTON_DIA = 6.00
BUTTON = (8.35, 74.80)
BUTTON_VERIFIED = False

# --- jacks ----------------------------------------------------------------
JACK_DIA = 6.79
JACK_COLS = [5.18, 18.50, 31.82, 44.50]
JACK_ROWS = [83.19, 97.15, 111.10]


def jacks():
    """The 12 jacks, in reading order: row 0 left to right, then row 1, row 2."""
    return [(x, y) for y in JACK_ROWS for x in JACK_COLS]


# --- what each hole does in Smack Versio ----------------------------------
#
# The panel roles come from DESIGN.md §4 and are what smack_versio.cpp
# actually dispatches. The bottom jack row is the Versio's fixed audio I/O;
# the top two rows are the 7 CV inputs plus the gate, which is the layout the
# jack grid above independently confirms (12 holes = 7 CV + gate + 4 audio).

KNOB_LABELS = [
    ("FX", "density"),
    ("ORDER", "density"),
    ("LENGTH", "8/16/32/64"),
    ("SLICE", "resolution"),
    ("BLEND", "clean/glitch"),
    ("SEED", "pattern"),
    ("PITCH", "range"),
]

# Jack legend, same order as jacks(). The first seven mirror the knobs; CLK is
# the gate input; the last row is fixed hardware I/O.
JACK_LABELS = [
    "FX", "ORDER", "LENGTH", "SLICE",
    "BLEND", "SEED", "PITCH", "CLK",
    "IN L", "IN R", "OUT L", "OUT R",
]

SWITCH_LABELS = [
    ("CLK", ["/2", "=1", "x2"]),
    ("GATE", ["CLK", "AUTO", "TRIG"]),
]

BUTTON_LABEL = ("CAPTURE", "hold = re-roll")

LED_LABELS = ["STATE", "PLAY", "BLEND", "CLOCK"]
