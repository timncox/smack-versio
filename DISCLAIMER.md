# Read this before flashing

**Smack Versio is third-party firmware for the Noise Engineering Versio
platform. It is not a Noise Engineering product.**

Noise Engineering did not write it, did not review it, and cannot support it.
From their own guide to building alternative Versio firmware:

> Our support team only has the capacity (and knowledge!) to help you with
> officially released firmware — most of us here at NE don't speak C/C++ and
> won't be able to review your code.

**Do not contact Noise Engineering about this firmware, about a module running
it, or about anything that goes wrong while it is installed.** If something is
broken, it is broken here — open an issue on this project instead.

## What installing it does

It replaces the firmware in your module's flash. It also installs the Daisy
bootloader, because this app is larger than the STM32H750's 128 KB of internal
flash and runs from SRAM instead.

**This is reversible.** Flashing any official Noise Engineering firmware
overwrites internal flash and returns the module to stock. No hardware
modification is involved, nothing is cut or soldered, and no part of the
process is one-way.

## What has and has not been tested

**Version 0.1.0 has never been run on hardware.** It compiles, its memory
budget is measured, and everything testable without a module is tested — the
clock adapter, the settings layer, the allocator, and the DSP engine at 48 kHz
all pass native test suites. But no one has heard it.

If you are flashing this, you are performing its first hardware test. The CPU
load figure it reports on boot is the number the project most needs.

## Warranty

None. This software is provided as-is, without warranty of any kind, express or
implied. You are responsible for what you install on your own hardware.

Versio, Desmodus Versio and Noise Engineering are trademarks of Noise
Engineering. This project is not affiliated with, endorsed by, or connected to
Noise Engineering in any way.
