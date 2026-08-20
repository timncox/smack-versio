# Flashing Smack Versio

**Built and unvalidated.** Nobody has run this on hardware yet. It compiles,
the native tests pass, and the memory budget fits — but no one has heard it.
The first flash is the acceptance test for M1, and its real job is answering
one question: **does the engine fit in the CPU budget?** LED 3 answers that
(see below).

## What you need

- The Versio out of the case — the micro-USB is on the **back** of the Daisy
  Seed, not the panel.
- `dfu-util` (already installed on this machine: 0.10).
- A USB cable that carries data, not just power.

## One-time: install the Daisy bootloader

This build is `APP_TYPE = BOOT_SRAM`, because the app is ~142 KB and the
STM32H750 has only **128 KB of internal flash**. The bootloader lives in
internal flash and loads the app from QSPI into SRAM.

**This is reversible.** Flashing any official Noise Engineering firmware
writes internal flash and removes the bootloader, putting the module back to
stock. You are not modifying anything you can't undo.

1. Put the Daisy in DFU mode: **hold BOOT, tap RESET, release BOOT.**
2. Confirm the computer sees it:
   ```
   dfu-util -l
   ```
   You want a device at `0483:df11`.
3. Flash the bootloader:
   ```
   make -C firmware program-boot
   ```

## Every time: flash the app

1. Power-cycle the module. The bootloader waits **2000 ms** at boot — that is
   the `-2000ms` in `dsy_bootloader_v6_4-intdfu-2000ms.bin` — and the Daisy
   enumerates as a DFU device for that window only. Run the next command
   inside it; if you miss it the app has already started, so power-cycle and
   try again.
2. ```
   make -C firmware program-dfu
   ```
   This writes `build/smack_versio.bin` to QSPI at `0x90040000` and leaves.
3. Reinstall the module and power up.

## What you should see and hear

**On boot, with nothing patched:** audio passes through. A Eurorack effect
that is silent until you press a button reads as broken, so the module starts
in passthrough (`monitor=1`).

**For the first ~2.5 seconds, the LEDs are not showing normal status** — they
are replaying the worst CPU load from your *previous* session, as a bar. On the
very first boot after flashing there is no data yet, so you get a single dim
blue LED 0. Audio is already passing through during the readout.

LEDs are named left to right as STATE · PLAY · BLEND · CLOCK (LED 0–3 in the
source; the same four everywhere else in these docs).

| Lit | Last session's peak |
|---|---|
| **STATE** dim blue | no data — first boot after a flash |
| **STATE** dim green | measured, under 25% |
| **STATE** | over 25% |
| **STATE PLAY** | over 50% |
| **STATE PLAY BLEND** | over 75% |
| **STATE PLAY BLEND CLOCK**, last red | over 90% — it did not fit |

**This is the intended way to answer the CPU question.** Play hard for a few
minutes with FX and Order Density up, a short Slice Res and a long loop; then
power down and power up and read the bar. You cannot watch LED 3 while playing
with both hands, and this does not ask you to.

The peak is kept in the last 4 KB sector of the QSPI chip, which a reflash does
not touch — so a stale reading from an older build would be a real hazard. The
settings carry a magic + version word specifically so a new build refuses to
adopt an old one's numbers and shows "no data" instead.

**LED 0 — engine state:** dim blue idle, amber armed, red recording, green
looping.
**LED 1 — playhead:** ramps once per loop pass. If this is moving, a loop is
captured and playing.
**LED 2 — blend:** the Blend knob's position.
**LED 3 — clock source, and the CPU alarm:**

| LED 3 | Meaning |
|---|---|
| **red** | **CPU above 80% — this is the number M1 exists to find** |
| blue | locked to an external clock at the gate jack |
| purple | tempo inferred from trigger intervals |
| white | free-running, nothing patched |

**Red LED 3 is the one result worth reporting back.** If it never goes red
under heavy settings (FX Density and Order Density high, short Slice Res,
long loop), the CPU question is settled and DESIGN.md §8 can drop its ⚠️.

**All four LEDs solid red** = the engine failed to allocate. That should be
impossible (12.98 MB of a 16 MB pool, measured natively), but it fails loudly
rather than running silent.

## Controls

| Control | Does |
|---|---|
| Knob 1 | FX Density — how many slices get an effect |
| Knob 2 | Order Density — how much slice reordering |
| Knob 3 | Loop Length — 8 / 16 / 32 / 64 steps |
| Knob 4 | Slice Res |
| Knob 5 | Blend — dry input ↔ effected loop |
| Knob 6 | Seed — the "I don't like this pattern" knob |
| Knob 7 | Pitch Range |
| SW_0 | Clock ratio: right `/2`, centre `=1`, left `x2` |
| SW_1 | Gate role: right CLK, centre AUTO, left TRIG |
| **Button, tap** | **Re-roll** — new pattern, same loop |
| **Button, hold** (>600 ms) | **Capture** — grab the last N steps |
| **Button, hold** (>2 s) | **Clear** — drop the loop, back to passthrough |
| Gate in | Clock and/or capture trigger, per SW_1 |
| CV in ×7 | Sums with its knob (analog, always active) |

Every knob has a CV input, including Blend. FX Density and Order Density
under CV is the thing this module gains over the Move version.

## If it doesn't work

- **No DFU device**: it's a power-only USB cable, or the BOOT/RESET sequence
  didn't take. Retry step 1.
- **Nothing happens after flashing**: the bootloader may not have been
  installed — but **you cannot tell that from the PID.** libDaisy sets
  `DAISY_PID = df11` and `STM_PID = df11` in `core/Makefile`, so the STM32's
  built-in DFU and the Daisy bootloader both enumerate as `0483:df11`. Two
  things do discriminate:
  - **Timing.** The bootloader's DFU window closes after 2000 ms and the
    device drops off `dfu-util -l`. The STM32's built-in DFU — entered by
    holding BOOT — stays enumerated indefinitely. A device that vanishes
    ~2 s after power-up is the bootloader doing its job.
  - **The alt-setting `name=` string** in `dfu-util -l`, which names the
    memory region the mode exposes: internal flash at `0x08000000` for ST's
    DFU, the QSPI region at `0x90000000` for the bootloader. Read the strings
    off your own `dfu-util -l`; they are deliberately not quoted here because
    nobody has run this on hardware yet.
- **Sound but no capture**: check SW_1 and whether anything is patched to the
  gate. With nothing patched it free-runs at 120 BPM, which is fine — Capture
  should still work.
- **Knob values jumping**: that's the hysteresis deadband being too small.
  `HYST` in smack_versio.cpp, currently 2% of full scale.

## Going back to stock

Put the Daisy in DFU (hold BOOT, tap RESET) and flash any official Noise
Engineering firmware with their flash tool, or:
```
dfu-util -a 0 -s 0x08000000:leave -D <official>.bin -d ,0483:df11
```
That overwrites internal flash — bootloader gone, module back to stock.
