# Fitting in internal flash (and why that matters)

> **2026-08-20: the reason for wanting this has mostly gone away.** The premise
> below is that `BOOT_SRAM` costs the user a terminal and a button chord. It
> does not have to: the Daisy Web Programmer flashes both the bootloader and a
> `BOOT_SRAM` app from a browser, and works out the `0x90040000` app address by
> itself. See "flash it from a browser" in FLASHING.md.
>
> **2026-08-20, later the same day: the case is back on, for a better reason.**
> The reference third-party firmwares are internal-flash builds — WTF! at 91 KB,
> FRGMNTS at 85 KB — which is why they install from Noise Engineering's own
> portal. That portal takes a user-supplied file; what it will not do is write
> QSPI, so a `BOOT_SRAM` app at `0x90040000` is out of its reach. Matching the
> shape WTF! and FRGMNTS take is the way onto that path, and `BOOT_NONE` is
> that shape.
>
> The blocker named below is also gone: CPU was measured on hardware at a
> **50–75% peak** (DESIGN.md §8), so a `-Os -flto` libDaisy rebuild now has a
> baseline to be compared against rather than being a leap in the dark. Read
> the bar before and after; if it climbs a band, that is the answer.
>
> Sequence still matters — keep a known-good `BOOT_SRAM` build to fall back to.

**Result, measured 2026-08-19: it fits, with 16 KB to spare.** A `BOOT_NONE`
build lands at **114,472 bytes of 131,072 — 87.3%.** The Makefile's standing
claim that a plain internal-flash build is impossible is now out of date; it
was true only for the toolchain configuration it was written against.

## Why this is worth wanting

`APP_TYPE = BOOT_SRAM` costs the user a real install step. They have to put the
module in DFU, flash the Daisy bootloader once, then flash the app into QSPI in
a 2000 ms window. That means installing `dfu-util` and learning a button chord
before hearing anything.

A `BOOT_NONE` build is an ordinary firmware image in internal flash at
`0x08000000` — exactly where stock Noise Engineering firmware lives, and
plausibly something NE's own web uploader at
<https://noiseengineering.us/portal/firmware/> could write. **Unverified.** No
one has fed this image to that page yet.

## The measurements

Rebuilt 2026-08-20 against the current source. **It still fits, but the margin
is much thinner than it was**, because the code has grown since the first
measurement — the ring-stall fix, LIVE mode and the float-printf link flag all
landed in between.

| Configuration | bytes | of 128 KB |
|---|---:|---:|
| `BOOT_SRAM` (what ships today) | 158,468 | — |
| `BOOT_NONE`, libDaisy `-Os -flto`, **2026-08-19** | 114,472 | 87.3% |
| `BOOT_NONE`, libDaisy `-Os -flto`, **2026-08-20** | 130,880 | **99.85%** |
| `BOOT_NONE`, same, **without `-u _printf_float`** | **126,312** | **96.37%** |

99.85% is 192 bytes of headroom, which is not a configuration anyone should
ship — it is one edit away from failing to link, which is precisely the failure
this file warned about at the bottom.

Dropping `-u _printf_float` buys back 4,568 bytes and takes it to 96.37%, with
4,760 spare. The Makefile now omits that flag for `BOOT_NONE` only. That is
safe **because this firmware reads exactly three engine params and all three
are integer-formatted** — `run_state` `"%d"`, `loop_frames` `"%u"`,
`play_frame` `"%d"`. Read any of the engine's six float-formatted params from a
`BOOT_NONE` build and it will silently come back 0, exactly as `play_frame`
did before v0.2.0. The Makefile says so at the flag.

Even 96.37% is tight for something to build on. The next lever, if it is
needed, is the USB serial logger — the Makefile's own notes put that at
-7.1 KB, which would land around 91%.

### What this build has not done

**Run.** These are link-time numbers. Nothing has been flashed, and the whole
reason for wanting `-Os` on libDaisy is also the reason to be careful with it:
it changes the driver and interrupt paths. CPU is now measured at a 50-75% peak
under `-O3` libDaisy (DESIGN.md §8), so there is finally a baseline — flash a
`BOOT_NONE` build, drive it equally hard, power-cycle and compare the bar. If
it climbs a band, `-Os` is the cost.

Two separate levers, and the order matters:

1. **LTO across libDaisy: −8,208 bytes.** The Makefile already noted that
   `USE_LTO=1` on our own code saves only 1.7 KB "because libDaisy.a carries no
   LTO bytecode". That was the whole point — the archive has to be built with
   `-flto` too, or there is nothing for the link-time pass to eliminate. Doing
   that got it to 3,044 bytes over, which is agonisingly close and still a
   failure.
2. **`-Os` on libDaisy: another −19,644 bytes.** This is the one that actually
   wins, and it is close to free here: our own Makefile already builds
   *everything of ours* at `-Os`, including `smack_core`. libDaisy is drivers
   and HAL — SAI, ADC, I2C, USB setup — not the audio inner loop. The DSP's
   optimisation level does not change.

## Reproducing

libDaisy has to be rebuilt with both flags, and with `gcc-ar` so the archive
carries the LTO plugin symbols:

```
cd ~/tim-os/daisy-sdk/libDaisy
make clean
make OPT="-Os -flto" AR=arm-none-eabi-gcc-ar -j8
```

Then, from this repo:

```
make -C firmware clean
make -C firmware APP_TYPE=BOOT_NONE USE_LTO=1
```

**Back up `libDaisy/build/libdaisy.a` first.** That archive is shared with every
other Daisy project on the machine, and this rebuild replaces it. Restoring it
is a file copy; forgetting to is a confusing afternoon in some unrelated repo.

The `-O3` archive from before the 2026-08-20 rebuild is kept at
`~/tim-os/scratch/libdaisy.a.O3-backup`:

```
cp ~/tim-os/scratch/libdaisy.a.O3-backup ~/tim-os/daisy-sdk/libDaisy/build/libdaisy.a
```

This is not academic. With libDaisy at `-Os -flto`, the ordinary `BOOT_SRAM`
build drops from 158,468 to 132,492 bytes — so **a rebuild today does not
reproduce the published v0.2.0 binary.** Restore the archive before cutting a
release, or rebuild libDaisy at stock `-O3` first.

## What is not established

- **Nothing here has run on hardware.** This is a link-time result only.
- **CPU cost is unmeasured.** `-Os` on libDaisy could plausibly slow the driver
  and interrupt paths. Since CPU headroom is already DESIGN.md §8's open
  question, moving to this configuration without reading the boot CPU bar first
  would be trading a known question for two unknown ones.
- **LTO can expose latent undefined behaviour** that `-O3` per-file happened to
  tolerate. A build that links is not a build that works.
- **Whether NE's uploader accepts a third-party `.bin` at all** is untested, and
  it is the entire reason to want this.

## The order to do this in

Get the panel and audio working under the current `BOOT_SRAM` build first, and
read the CPU bar. Only then port to `BOOT_NONE` — with a known-good reference
to compare against, so if the LTO build misbehaves it is obvious that it did.
