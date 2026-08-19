# Diagnosing a dark panel

Symptom, first hardware run 2026-08-19: the boot CPU readout appears, and then
all four panel LEDs go dark and stay dark.

That symptom is ambiguous, which is the whole problem. Every LED written in
`update_leds()` is downstream of a `smack_get_param()` call, so a dark panel is
equally consistent with two very different faults:

1. **The main loop is not running** — it faulted, hung, or never got CPU
   because the audio callback is overrunning its block period.
2. **The main loop is running fine** and the engine reads are failing, or the
   LED values themselves are computing to something invisible.

These need opposite fixes, so guessing between them wastes a bench session.

## The heartbeat build

`DIAG=1` compiles in a blink at the very top of the main loop that writes LED 3
and pushes it to the driver **before touching the engine or anything else**, and
compiles the normal LED 3 write out so nothing overwrites it.

```
make -C firmware clean
make -C firmware DIAG=1
make -C firmware program-dfu
```

`clean` is not optional. The sources are byte-identical between a diagnostic
and a release build, so make will not rebuild on the flag change alone and you
will flash the wrong image without noticing. Confirm by size: a release build
is 142,292 bytes, the diagnostic 142,228.

## Reading the result

| LED 3 after the boot readout | Meaning | Where to look next |
|---|---|---|
| **Blinks white, ~2 Hz** | The main loop is alive and reaching the top of every iteration | The fault is downstream: `smack_get_param()` in `update_leds()`, or the LED 0–2 values. Hypothesis 2. |
| **Dark, or frozen on one colour** | The loop never gets past its first line | Hard fault, hang, or the audio callback is starving the main loop. Hypothesis 1. |

A *frozen* LED 3 and a *dark* LED 3 mean the same thing here and both point at
hypothesis 1 — the LED driver holds its last written register values, so a
CPU that stops writing leaves whatever was there.

## Discriminating within hypothesis 1

If LED 3 does not blink, the next question is whether the CPU is faulted or
merely saturated. **Is audio still passing through?**

- **Audio still passes** — the audio callback is an interrupt and is still
  being serviced, so the CPU has not faulted. The main loop is being starved:
  `smack_process()` is taking longer than one 128-frame block at 48 kHz
  (2.67 ms), so the callback re-enters continuously and the main loop never
  gets scheduled. That is the CPU-budget failure DESIGN.md §8 warns about,
  showing up as a dead panel instead of a red LED.
- **Audio is dead too** — a hard fault. Everything stopped, interrupts
  included.

This is why "does sound come out" is the first question to ask about a dark
panel, not the last.

## Do not ship a DIAG build

It disables the LED 3 status and CPU alarm entirely. Rebuild with
`make -C firmware clean && make -C firmware` before flashing anything you
intend to keep.
