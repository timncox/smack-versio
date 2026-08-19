# Flashing Smack Versio

**Flashes cleanly; not yet heard.** As of 2026-08-19 the procedure below has
been run end to end on a real Versio — bootloader to internal flash, app to
QSPI, both verified by `dfu-util` — so the steps and the addresses are known
good. What is still unconfirmed is everything downstream of that: no one has
reported audio out of it, so the engine, the panel mapping and the CPU budget
remain untested. The first person to listen is still performing the
acceptance test for M1, and its real job is answering one question: **does the
engine fit in the CPU budget?** LED 3 answers that (see below).

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
   **This step ends in an error even when it works.** Expect exactly this:
   ```
   Download done.
   File downloaded successfully
   dfu-util: Error during download get_status
   make: *** [program-boot] Error 74
   ```
   That is benign. `program-boot` passes `:leave`, so the chip exits DFU and
   jumps to the new bootloader before dfu-util can read a final status — the
   write already completed on the line above. Judge it by
   `File downloaded successfully`, not by the exit code. Confirm by running
   `dfu-util -l` again: the name should now be the QSPI map rather than
   `@Internal Flash`.

   Two other harmless warnings on every flash: `Invalid DFU suffix signature`
   (a raw `.bin` carries no DFU trailer) and the note about future dfu-util
   releases requiring one.

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
| Knob 5 | Blend — clean loop ↔ glitch pattern |
| Knob 6 | Seed — the "I don't like this pattern" knob |
| Knob 7 | Pitch Range |
| SW_0 | Clock ratio: up `/2`, centre `=1`, down `x2` |
| SW_1 | Gate role: up CLK, centre AUTO, down TRIG |
| **Button, short** | **Capture** — grab the last N steps |
| **Button, long** (>600 ms) | **Re-roll** — new pattern, same loop |
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
    memory region the mode exposes. Observed on hardware 2026-08-19:

    ST's built-in ROM DFU (BOOT held at reset) — internal flash, and a second
    alt setting for the option bytes:
    ```
    alt=0, name="@Internal Flash   /0x08000000/16*128Kg", serial="200364500000"
    alt=1, name="@Option Bytes   /0x5200201C/01*128 e"
    ```
    The Daisy bootloader — the QSPI map, one alt setting, different serial:
    ```
    alt=0, name="@Flash /0x90000000/64*4Kg/0x90040000/60*64Kg/0x90400000/60*64Kg"
    serial="3986335E3330"
    ```
    Easiest tell of all: `ioreg -p IOUSB -w0 -l | grep '"USB Product Name"'`
    reports **`Daisy Bootloader`** by name once it is installed.

- **The module does not appear on the USB bus while it is running.** Neither
  the stock firmware nor this one initializes the USB device peripheral, so an
  empty `dfu-util -l` with the module plugged in and playing is normal and
  says nothing about your cable. Only the DFU modes above enumerate. Don't
  read a dead bus as a dead cable — check it *after* a BOOT/RESET, which is
  the case that must enumerate.

- **A charge-only USB cable powers the Seed but never enumerates.** VBUS is
  present, the module lights up and runs, and the two data lines go nowhere —
  so the module looks perfectly alive while being invisible to `dfu-util`.
  This cost the first flash about twenty minutes. If a correct BOOT/RESET
  produces nothing, change the cable before you change anything else, and
  prefer a port directly on the machine over a hub.
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
