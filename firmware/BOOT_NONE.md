# Fitting in internal flash (and why that matters)

> **2026-08-20: the reason for wanting this has mostly gone away.** The premise
> below is that `BOOT_SRAM` costs the user a terminal and a button chord. It
> does not have to: the Daisy Web Programmer flashes both the bootloader and a
> `BOOT_SRAM` app from a browser, and works out the `0x90040000` app address by
> itself. See "flash it from a browser" in FLASHING.md.
>
> So a `BOOT_NONE` port would buy one less click, in exchange for rebuilding
> libDaisy at `-Os -flto` — which is exactly the change most likely to move CPU
> load, the one number this project still has not measured. Not worth it on
> those terms. The measurements below stay because they are real and because
> the trade may look different once the CPU bar has been read.

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

| Configuration | bytes | of 128 KB |
|---|---:|---:|
| `BOOT_SRAM` (what ships today) | 142,324 | — |
| `BOOT_NONE`, libDaisy stock `-O3`, no LTO | ~142,000 | ~108% |
| `BOOT_NONE`, libDaisy `-O3 -flto` | 134,116 | 102.3% |
| `BOOT_NONE`, libDaisy `-Os -flto` | **114,472** | **87.3%** |

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
