# Smack Versio

**Live loop capture, sliced and re-ordered, with seeded per-slice glitch.**
Alternative firmware for the Noise Engineering Versio platform.

> There is a web version of this manual at
> **<https://timncox.github.io/smack-versio/>** with an interactive panel
> diagram drawn from the real hole coordinates. This file is the text of
> record; `docs/index.html` is generated alongside it by hand, so if the two
> disagree, trust this one.

> **Version 0.2.0 — it works.**
> Confirmed on a module: audio, capture, the knob layout, the effects and LIVE
> mode. The boot CPU report reads **50–75% at the worst case**, so the headroom
> question the whole design rested on is answered. See
> [Is it working?](#is-it-working).
>
> **Do not flash v0.1.0.** It hard-faults on boot — the audio callback treated
> libDaisy's `size` as frames when it is samples and overran its buffer every
> block, so the module emitted a steady buzz and nothing else.

---

## Not a Noise Engineering product

This is third-party firmware. Noise Engineering did not write it, did not
review it, and cannot support it. In their own words, from the page where they
invite people to build things like this:

> Our support team only has the capacity (and knowledge!) to help you with
> officially released firmware — most of us here at NE don't speak C/C++ and
> won't be able to review your code.

**Do not contact Noise Engineering about this firmware.** If something is
wrong, it is wrong here.

Installing this replaces the firmware your module shipped with. That is
reversible — flashing any official Noise Engineering firmware restores the
module to stock, and the procedure is in [FLASHING.md](firmware/FLASHING.md).
No hardware modification is involved and nothing is permanent.

Use at your own risk. No warranty, express or implied.

---

## What it does

Audio runs through the module continuously and is always being recorded into a
rolling buffer. When you hold **CAPTURE**, the last **LENGTH** steps of what
you just played become a loop — you do not have to arm anything first or know in advance
that you wanted it.

That loop is then cut into a grid of slices. The slices get re-ordered, and a
proportion of them get an effect applied — which slices, which effects, and in
what order, all follow from one **SEED** value. Turn SEED and you get a
different arrangement of the same audio. **BLEND** crossfades between the clean
loop and the glitched pattern.

Everything is quantised to a clock, so it stays in time with the rest of the
rack.

This is a port of [Smack](https://github.com/timncox/schwung-smack), a module
for the Ableton Move. The DSP is the same engine, unmodified.

### What did not come across

The Move version has a 23-pad grid and a screen; the Versio has seven knobs and
four LEDs. Per-slice pinning and locks, step editing, and the effect palette
editor are all gone — there is nowhere to put them. What is left is a
density-and-seed instrument: you steer the *statistics* of the mangling rather
than editing individual slices.

---

## Quick start

1. Patch audio into **IN L** / **IN R**. It passes through immediately — the
   module is not silent before you do anything.
2. Patch a clock into **CLK** if you have one. If you don't, it free-runs.
3. Play something.
4. Hold **CAPTURE** for half a second. The last few bars become a loop and
   start playing.
5. Turn **FX** and **ORDER** up. Turn **SEED** until you like the pattern.
6. **BLEND** decides how much of the mangling you hear.

Now **tap CAPTURE** to re-roll: same loop, new pattern. Keep tapping until you
get one you like — that is the gesture you will use most, which is why it is
the cheapest one.

To throw the loop away and go back to passing your input straight through,
hold **CAPTURE** for two seconds.

---

## Controls

Every knob has a CV input directly below the panel's knob section, and the CV
sums with the knob in analogue hardware — so a knob at noon with a CV at the
jack behaves like a knob being turned. **FX** and **ORDER** under CV is the
thing this version can do that the Move version cannot.

| Control | What it does |
|---|---|
| **FX** | How many slices get an effect, from none to all |
| **ORDER** | How much the slice order is scrambled |
| **LENGTH** | Loop length: 8 / 16 / 32 / 64 / 128 / 256 steps (256 = 16 bars) |
| **SLICE** | How finely the loop is cut up |
| **BLEND** | Your live input ←→ the effected loop. Fully left is dry thru |
| **SEED** | Which pattern. Same seed, same result, every time |
| **PITCH** | How far pitch-shifting effects are allowed to move |
| **CLK** switch | Clock ratio: right `/2`, centre `=1`, left `×2` |
| **GATE** switch | What the gate jack means — see below |
| **CAPTURE** | Tap: re-roll the pattern. Hold >0.6 s: grab the last LENGTH steps. Hold >2 s: drop the loop |
| **CLK** jack | Clock and/or capture trigger, per the GATE switch |

**Very slow clocks shorten the longest LENGTH.** A loop may take at most half
the module's 150-second buffer, because the recorder has to keep writing a
fresh loop alongside the one that is playing. Below about 51 BPM, 256 steps no
longer fits, so LENGTH drops one notch — to 128 steps, then 64 — rather than
being trimmed to a partial loop, which would leave it off the grid and drifting
against your clock. At any normal tempo you will never see this.

Note that 51 BPM is the tempo the *engine* sees, not the one on your clock
source: with **CLK** set to `×2`, a 100 BPM clock lands there. The drop sticks
until the knob moves — if the tempo comes back up, LENGTH stays at the shorter
setting, because a parked knob sends nothing. Sweep LENGTH off its position and
back to restore it.

**LENGTH, SLICE and SEED are stepped**, and a stepped control parked exactly on
a boundary would otherwise chatter between two values as CV noise nudges it
back and forth — re-slicing the loop, or re-rolling the pattern, continuously.
Each stepped knob has a deadband to stop that: 2% of travel for the coarse ones,
and exactly one seed for SEED, which needs a much narrower band or turning it
would skip most of the 128 seeds. Both live in `deadband_for()` in
`firmware/smack_versio.cpp`.

### LIVE mode

**Double-tap CAPTURE.** LED 0 turns cyan. Double-tap again to leave.

Normally you capture once and that snapshot repeats until you capture again —
turn the knobs and the *pattern* changes, but the audio underneath is the same
few seconds forever. In LIVE, the module re-captures itself once per loop pass,
so the buffer keeps refilling with what you are playing now.

**LENGTH sets how often that happens, and the wait is a full loop pass:**

| LENGTH | At 120 BPM, refreshes every | Feels like |
|---|---|---|
| 8 steps | 1 s | near-live, chattery |
| 32 steps | 4 s | a bar or two behind |
| 256 steps | **32 s** | slow drift, nearly ambient |

So at 256 steps, LIVE looks completely dead for half a minute. **If you want
LIVE to feel live, turn LENGTH down** — that is the control, not a setting
elsewhere. With no clock patched the tempo is the internal free-run one, so the
interval is set by LENGTH alone.

It stays in time because capture is grid-aligned: it takes the quantum ending
at the last boundary and chases the current phase, so re-firing never drifts or
restarts mid-bar.

**LIVE records your dry input, never the effected output.** Each pass is clean
source, re-sliced from scratch, so effects never compound on themselves and the
loop cannot degrade into mush over time. What LIVE is *not* is a true insert:
reverse, tapestop, scratch, retrig and freeze all work on audio that has
already happened, so there is no zero-latency version of them to build. One
loop pass is the floor.

---

## The clock

The Versio has exactly one gate jack, and this module wants two things from it:
a tempo, and a "capture now" trigger. The **GATE** switch decides which.

| Position | The gate jack is |
|---|---|
| **CLK** | A clock. Tempo comes from the pulse intervals |
| **AUTO** | Worked out from what arrives — a steady train reads as a clock, sporadic hits read as triggers |
| **TRIG** | A capture trigger. Tempo is inferred from the gaps between hits |

Internally the module turns gate edges into 24 ppqn MIDI clock, because that is
what the engine already speaks. This is the piece with the most design risk in
the whole port, and it is also the piece that is most thoroughly tested — see
[Status](#status).

**With nothing patched**, it free-runs. The tempo it free-runs at is the last
tempo it successfully locked to, remembered across power cycles — not a fixed
120.

---

## The LEDs

Four RGB LEDs are the entire status display.

| LED | Shows |
|---|---|
| **STATE** | dim blue idle · amber armed · red recording · green looping |
| **PLAY** | Ramps once per pass through the loop. Moving = a loop is playing |
| **BLEND** | The BLEND knob's position |
| **CLOCK** | blue external · purple inferred · white free-running · **red = CPU above 80%** |

**All four solid red** means the engine could not allocate its buffer. That
should be impossible — it needs 12.98 MB of a 16 MB pool, measured — but it
fails loudly rather than running silently broken.

### At power-up

For about two and a half seconds after boot, the LEDs show **the worst CPU load
from your last session**, as a bar:

| Lit | Meaning |
|---|---|
| **STATE** dim blue | No data — first boot after flashing |
| **STATE** dim green | Measured, and comfortably under 25% |
| **STATE** | Peak was over 25% |
| **STATE PLAY** | over 50% |
| **STATE PLAY BLEND** | over 75% |
| **STATE PLAY BLEND CLOCK**, last one red | over 90% — it did not fit |

Audio passes through during the readout. This exists because the live CPU alarm
can only be read by someone looking at it, and you are not looking at it while
playing. Play hard, power down, power up, read the answer.

---

## Installing

Full procedure, including the one-time bootloader step and how to go back to
stock: **[firmware/FLASHING.md](firmware/FLASHING.md)**.

The short version: the micro-USB port is on the back of the Daisy Seed, so the
module comes out of the case. This build needs the Daisy bootloader installed
once, because the app is larger than the STM32H750's 128 KB of internal flash
and runs from SRAM instead.

**You do not need a terminal for either step.** Electro-Smith's [Daisy Web
Programmer](https://flash.daisy.audio/) flashes the bootloader and then the
firmware from Chrome or Edge, and works out the app's `0x90040000` address
itself — you never type one.

Noise Engineering's own uploader at <https://portal.noiseengineering.us/> is a
different tool: it flashes *their* firmwares, which is how you put the module
back to stock. It will not install this one.

---

## The faceplate

The panel legend will not match — your module says whatever it said before.
`faceplate/` generates a printable overlay with the right words on it:

```
python3 faceplate/make_faceplate.py
```

Print `smack-versio-printsheet.svg` **at 100%**, check the 50 mm ruler on it
with a real ruler, and only then cut. Its geometry is derived rather than
measured; [faceplate/MEASURE.md](faceplate/MEASURE.md) explains what is solid,
what isn't, and how to correct it.

---

## Is it working?

Here is what "working" looks like, in order:

1. **Audio passes through on boot.** If not, the flash did not take.
2. **The boot LED readout appears** for ~2.5 s, then normal operation.
3. **Hold CAPTURE for about a second** while audio is playing — STATE goes
   green and PLAY starts ramping. A *tap* re-rolls the pattern instead; it is
   the hold that grabs a loop.
4. **Turn FX and ORDER up.** The loop should audibly come apart.
5. **The CLOCK LED never goes red** under heavy settings — FX and ORDER high,
   SLICE short, LENGTH long.

All five are confirmed on a module. Step 5 was the one that mattered and it
passed. The boot report is per-session — the peak is cleared the moment it is
shown — and two hard runs read **50–75%** and **under 50%**. Neither tripped
the 80% alarm. The engine fits, with the worse of the two still leaving a
quarter of a block spare.

**The PLAY LED is the one to watch if something seems inert.** It ramps once
per loop pass, so it is the fastest check that the playhead is actually moving
— and a playhead position that never advances is exactly what stopped LIVE
mode from working on every build before v0.2.0.

### If something is wrong

| Symptom | Likely cause |
|---|---|
| No sound at all | Flash did not take, or the bootloader is missing |
| Sound, but CAPTURE does nothing | You may be tapping — tap re-rolls, *hold* captures. Otherwise check the GATE switch; with nothing patched it should still work |
| LIVE goes cyan but never refreshes | Firmware older than v0.2.0. `play_frame` came back empty on hardware, so the wrap that triggers LIVE was never detected |
| PLAY LED steady instead of ramping | Same cause as above — the ramp is derived from the playhead position |
| A stepped value flickers at one knob position | Deadband too narrow — widen it in `deadband_for()` (`HYST` for LENGTH/SLICE/PITCH; the one-step band for SEED) |
| The wrong knob does the wrong thing | Knob-to-ADC order — see [faceplate/MEASURE.md](faceplate/MEASURE.md) §2 |
| All four LEDs red | Allocation failed — this would be a real bug, please report it |

---

## Status

| Piece | State |
|---|---|
| Clock adapter | **Verified natively.** 7 tests; captures a bar from synthetic gate pulses to within 0.02% |
| Persistent settings | **Verified natively.** 6 tests covering the flash-wear limiter and the version guard |
| Engine at 48 kHz | **Verified natively.** The upstream suite passes at 48 k unmodified |
| Allocator | **Verified natively.** Engine runs on the SDRAM pool, 12.98 MB of 16 MB |
| Host shim, controls, LEDs | **Built, never run.** Compiles; nobody has heard it |
| CPU headroom | **Unmeasured.** The one thing that needs hardware |

---

## Specifications

| | |
|---|---|
| Platform | Noise Engineering Versio (Daisy Seed, STM32H750, 480 MHz Cortex-M7) |
| Width | 10 HP |
| Sample rate | 48 kHz |
| Block size | 128 frames |
| Max loop | ~70 s buffer, 13.4 MB of the 64 MB SDRAM |
| Effects | 26, one per lane at a time |
| Binary | ~142 KB, runs from SRAM via the Daisy bootloader |

---

## Credits

Engine: [Smack](https://github.com/timncox/schwung-smack) for Ableton Move.

The port owes its shape to two firmwares that proved this hardware can do this
kind of work: **WTF!** by dubrussell and **FRGMNTS Versio** by Acidclank. The
faceplate geometry was derived from measurements of the FRGMNTS 1:1 print
template; no artwork from it is reproduced here.

Versio, Desmodus Versio and Noise Engineering are trademarks of Noise
Engineering. This project is not affiliated with or endorsed by them.
