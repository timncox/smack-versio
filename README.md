# Smack Versio

**Live loop capture, sliced and re-ordered, with seeded per-slice glitch.**
Alternative firmware for the Noise Engineering Versio platform (Daisy Seed).

**📖 [Operation manual](https://timncox.github.io/smack-versio/)** — the panel
drawn at true scale from the real hole coordinates, click any control, plus a
simulator for the boot-time CPU report.

**⬇ [Download v0.1.0](https://github.com/timncox/smack-versio/releases/latest)** —
firmware, manual, flashing guide and printable faceplate. Tagged a
*pre-release*, and that is meant literally: see the warning below.

> ### ⚠️ This has never run on hardware
>
> It compiles, its memory budget is measured, and everything testable without a
> module is tested — the clock adapter, the settings layer, the allocator and
> the DSP engine at 48 kHz all have passing native test suites. **But nobody
> has heard a single sound out of it.**
>
> If you flash this, you are performing its first hardware test. Please
> [open an issue](../../issues) and say what happened — especially the boot CPU
> reading (below). That number is the thing this project most needs.

---

## Not a Noise Engineering product

This is third-party firmware. NE did not write it, did not review it, and
cannot support it. **Do not contact Noise Engineering about it** — if something
is wrong, it is wrong here. Installing is fully reversible: flashing any
official NE firmware restores the module to stock.

Read [DISCLAIMER.md](DISCLAIMER.md) before flashing.

---

## What it does

Audio runs through the module continuously and is always being recorded into a
rolling buffer. Press **CAPTURE** and the last N steps of what you just played
become a loop — no arming, no deciding in advance that you wanted it.

That loop is cut into a grid of slices. The slices get re-ordered, and a
proportion of them get an effect. Which slices, which effects, and in what
order all follow from a single **SEED** — turn it and you get a different
arrangement of the same audio. **BLEND** crossfades clean against glitched.
Everything stays quantised to the rack's clock.

It is a port of [Smack](https://github.com/timncox/schwung-smack), a module for
the Ableton Move. The DSP engine is the same code, vendored unmodified.

Because the Versio has seven knobs and no screen, the Move version's per-slice
editing does not come across. What is left is a density-and-seed instrument:
you steer the statistics of the mangling rather than editing individual slices.

## The boot CPU reading

The one open question is whether the engine fits the CPU budget. You cannot
watch a status LED while playing with both hands, so the module measures its
own worst-case block load and replays it on the LEDs at the **next** power-up.

Play hard — FX and ORDER up, short SLICE, long LENGTH — then power-cycle and
read the bar across the four LEDs. One LED means it never passed 25%; all four
with the last one red means it did not fit.

## Building

Needs [libDaisy](https://github.com/electro-smith/libDaisy) and
`arm-none-eabi-gcc`.

```sh
make -f firmware/Makefile.test          # native tests, no hardware
make -C firmware                        # build the .bin
make -C firmware program-boot           # one-time: Daisy bootloader
make -C firmware program-dfu            # flash
```

Point `LIBDAISY_DIR` at your libDaisy checkout if it isn't at
`~/tim-os/daisy-sdk/libDaisy`.

This is an `APP_TYPE = BOOT_SRAM` build: the app is ~142 KB and the STM32H750
has only **128 KB of internal flash**, so it runs from SRAM via the Daisy
bootloader. That is why the bootloader step exists, and it is reversible.

`scripts/make-release.sh` assembles a distributable zip.

## Layout

| Path | |
|---|---|
| [`MANUAL.md`](MANUAL.md) | What the controls do |
| [`firmware/FLASHING.md`](firmware/FLASHING.md) | Installing, and going back to stock |
| [`DESIGN.md`](DESIGN.md) | Why it is built this way — hardware constraints, clock architecture, prior art |
| `firmware/` | The firmware. `vendor/` is the unmodified engine |
| `faceplate/` | Generates a printable panel overlay |
| [`faceplate/MEASURE.md`](faceplate/MEASURE.md) | Correcting the derived panel geometry |

`DESIGN.md` marks every claim **✅ verified** (with a source) or **⚠️ assumed**.
That distinction is the most useful thing in the repository — it tells you which
half of the design is real.

## Faceplate

Your module's panel says whatever it said before, so `faceplate/` generates an
overlay with the right words on it:

```sh
python3 faceplate/make_faceplate.py
```

Print the **print sheet** at 100% and check the 50 mm ruler on it with a real
ruler before cutting — that ruler exists because every way printing goes wrong
is invisible except as a ruler that is not 50 mm.

The geometry is derived rather than measured; see
[`faceplate/MEASURE.md`](faceplate/MEASURE.md) for what is solid (the panel
frame reproduces the Eurorack mounting standard to 0.01 mm), what is not (the
button position), and how to fix it.

## Prior art

Two firmwares proved this hardware can do this kind of work, and the design
owes a lot to both: **WTF!** by
[dubrussell](https://2020.dubrussell.com/resources/wtf/) and **FRGMNTS Versio**
by [Acidclank](https://acidclank.gumroad.com/l/frgmntsversio).

## License

MIT — see [LICENSE](LICENSE), which also covers the vendored components and the
provenance of the faceplate geometry.
