# Correcting the faceplate geometry

Everything in `geometry.py` was derived from a 1:1 print template rather than
measured off a module. That is good enough to print and hold against the panel,
and not good enough to send to a fabricator. This file is how you upgrade it.

You need calipers and about ten minutes. There are only three things worth
checking, in this order.

---

## 1. The button position — corrected from the hardware, still not measured

**Status: observed 2026-08-09, by eye rather than with calipers.**

Every other hole came out of the vector geometry of a real 1:1 template. The
button did not, and the two attempts to derive it were both wrong: the only
button-sized candidate in the artwork was decoration, and placing a button
there put its edge 0.6 mm inside a switch; continuing the switch column by one
more pitch landed 0.07 mm inside the top jack row. `check_geometry()` caught
both.

Those failures were the useful part. The left column kept refusing to fit a
button **because there is no button in the left column** — it is in the centre,
level with the lower switch, which is what the hardware shows. A constraint
that will not close is sometimes telling you the premise is wrong rather than
the arithmetic.

What is in `geometry.py` now matches the module. It is good enough to print,
cut and fit. It is *not* good enough to send to a fabricator, because "centre
column, level with the lower switch" is a description, not a dimension. If you
are machining a panel:

```
measure from the TOP EDGE of the panel to the centre of the button
measure from the LEFT EDGE of the panel to the centre of the button
```

Then in `geometry.py`:

```python
BUTTON = (x_from_left, y_from_top)
BUTTON_SOURCE = "caliper-measured <date>"
```

## 2. The knob-to-ADC mapping — the one that actually ruins the legend

This is not a geometry problem and no amount of measuring fixes it, but it is
the failure you are most likely to hit, so it is here.

`geometry.py` lists the knobs in **panel reading order**. `smack_versio.cpp`
assumes libDaisy's `KNOB_0..KNOB_6` are wired in that same order. Nothing has
verified that assumption — it depends on how the Versio's ADC channels are
routed, which is not in any document we have.

The check, once the firmware is flashed:

1. Set every knob to minimum.
2. Turn **one** knob fully up and listen for / look at what changes.
   - `FX` and `ORDER` change the pattern density audibly.
   - `LENGTH` and `SLICE` change the loop's grid.
   - `BLEND` sweeps clean-to-glitched.
   - `SEED` re-rolls the pattern.
3. If the knob that changes is not the one the print says, the ADC order is
   different from panel order.

The fix is in `smack_versio.cpp`, not here — reorder the `P[]` table so its
entries match the physical panel. The array index *is* the ADC channel:

```c
static Param P[P_COUNT] = {
    { "fx_density",    0, 100, -32768, 0.0f },   /* <- KNOB_0 */
    { "order_density", 0, 100, -32768, 0.0f },   /* <- KNOB_1 */
    ...
```

Once you know the true order, update `KNOB_LABELS` in `geometry.py` to match
so the print and the firmware agree.

## 3. Everything else — spot-check, don't re-measure

The panel frame is self-validating and very unlikely to be wrong: the derived
outline is 50.44 × 128.34 mm (10 HP × 3 U) and the four mounting slots land
2.99 mm and 125.33 mm from the top edge, 7.60 mm from the left — which is the
Eurorack standard mounting pattern to a hundredth of a millimetre. That did not
come from fitting; it fell out of the same transform as every hole.

So rather than re-measure all 24 holes, check two numbers:

| Check | Expect |
|---|---|
| Panel width | 50.4–50.5 mm |
| Top mounting slot, centre to top edge | ~3.0 mm |
| Jack column pitch (row of 4) | ~13.3 mm |
| Jack row pitch (3 rows) | ~13.96 mm |

If those agree, the frame is right and so is everything positioned in it.

---

## Re-generating

```
python3 faceplate/make_faceplate.py            # dark, matches the module
python3 faceplate/make_faceplate.py --light    # white, saves ink
```

The generator refuses to emit a panel whose holes overlap or run off the plate
(`check_geometry()`), so a bad edit fails loudly rather than printing something
that cannot be drilled. That check has already caught two real errors; trust it
over a visual once-over.
