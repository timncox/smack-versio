# Smack Versio

**Live loop capture, sliced and re-ordered, with seeded per-slice glitch.**
Alternative firmware for the Noise Engineering Versio platform.

> There is a web version of this manual at
> **<https://timncox.github.io/smack-versio/>** with an interactive panel
> diagram drawn from the real hole coordinates. This file is the text of
> record; `docs/index.html` is generated alongside it by hand, so if the two
> disagree, trust this one.

> **Version 0.3.0 — installs from Noise Engineering's own firmware page.**
> Put the module in DFU, hand it `smack_versio.bin`, done — no terminal, no
> bootloader, no extra tools. Confirmed on a module 2026-08-20.
>
> Everything else is confirmed too: audio, capture, the knob layout, the
> effects and LIVE mode, with a boot CPU report of **50–75% at the worst
> case**. See [Is it working?](#is-it-working).
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
| **PITCH** | How far pitch-shifting effects are allowed to move — or the **DJ filter**, if you set it that way |
| **CLK** switch | Clock ratio: right `/2`, centre `=1`, left `×2` |
| **GATE** switch | Left **PUNCH**, centre normal, right **DUAL** — the switch decides what the button does |
| **CAPTURE** | Tap: re-roll. Hold >0.6 s: grab the last LENGTH steps. Hold >2 s: drop the loop. Double-tap: LIVE. Triple-tap: the config layer |
| **CLK** jack | Clock and/or capture trigger |

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

## PUNCH

**Put the GATE switch left and the button becomes a momentary effect punch.**
Hold it and every slice in the loop is forced through one chosen effect;
release and the pattern comes straight back.

Nothing else is on the button in this position. No re-roll, no capture, no
clear, no LIVE, no config layer — all of it stands down. That is the point: a
punch you have to think about is not a punch, and a gesture that might re-roll
your pattern instead cannot be played hard. **Capture your loop in the centre
position, then flip left to perform.**

Which effect it punches is set in the [config layer](#the-config-layer), and
the choice is remembered across power cycles. **Punch CLEAN** is one of the
options and is worth trying first: instead of adding an effect it momentarily
*drops* the glitch pattern, so the loop snaps back to unmangled for as long as
you hold. On a busy pattern that reads as the biggest gesture on the module.

LED 0 goes white while a punch is held.

Flipping the switch away while you are still holding the button releases the
punch for you. It has to — the switch has just taken away the button that
would otherwise have done it.

---

## The config layer

**Triple-tap CAPTURE.** LED 0 turns magenta. Any single tap leaves again.

Three knobs stop driving their printed functions and address a setting
instead. There is nothing to scroll and no cursor to move, because with seven
absolute controls on the front there is nothing to navigate — you turn the
knob for the thing you want, and all three settings are visible at once.

| Knob | Setting | Left | Right |
|---|---|---|---|
| **PITCH** (lower right) | What the PITCH knob does normally | PITCH range | **DJ filter** |
| **FX** (top left) | Which effect PUNCH uses | punch clean, then the effect list | |
| **LENGTH** (centre) | How the CLK jack is read | AUTO — work it out | Always treat it as a clock |

| LED | Shows |
|---|---|
| **0** | Magenta — you are in the config layer |
| **1** | Green PITCH range · blue DJ filter |
| **2** | White = punch clean · a red→green ramp across the effect list |
| **3** | White AUTO · blue always-a-clock |

**Opening the layer to look at your settings cannot change them.** A knob is
only adopted once you actually move it, so the knobs' resting positions — which
are wherever their normal jobs left them — are ignored until you turn one.

**Leaving hands the knobs back without jumping anything.** A knob you moved in
here is no longer where its printed function left it, so the module keeps that
function exactly where it was and picks the knob up on your next touch. Turn
LENGTH to choose a clock mode and your loop does not re-length itself when you
leave.

All three settings are saved to flash.

### The DJ filter

Set PITCH's role to **DJ filter** and the knob becomes one sweep across
everything the module puts out: **left sweeps a lowpass down to 40 Hz, right
sweeps a highpass up to 6 kHz, and the centre is a real bypass** — a detent-free
notch you can find by feel.

It runs last, after BLEND, on the module's whole output. So it works on your dry
input before you have captured anything at all, which nothing else on this panel
does.

**It takes over the moment the knob next reaches the centre**, not the moment you
select it. You choose the filter by turning PITCH to the *right*, which is also a
filter position — without the wait, closing the layer would drop a highpass onto
whatever is playing. Bring the knob back to the notch and it picks up from there,
which is where a filter sits between gestures anyway.

The trade is that PITCH range loses its knob and is pinned at **one octave**
while the filter has it. That is a deliberate swap and not a compromise: pitch
range is a set-and-forget parameter and a filter sweep is a performance one.

---

## The clock

The Versio has exactly one gate jack, and by default the module works out for
itself what is arriving: **a steady train reads as a clock, sporadic hits read
as triggers** and the tempo is inferred from the intervals between them. There
is nothing to set, and in almost every patch there is nothing to think about.

The one case that needs telling is a deliberately uneven clock — swung, or
gated, or otherwise not steady — that you still want read as a clock. Set
**always-a-clock** in the [config layer](#the-config-layer) and it will be.

> **The GATE switch no longer selects the clock source.** It used to, in its
> left position. Almost nothing is lost by the move: AUTO already picks
> correctly, so what the position was really offering was a manual override for
> a case that rarely comes up — a set-once decision, and set-once decisions are
> what a config layer is for. A performance switch position is worth more than
> that, and it is now PUNCH.
>
> **Both switches read the same way round.** Left is left on both, which is
> worth stating because this manual once said the opposite. That claim came
> from reading a report about what was *heard* as a report about where the
> *labels sit* — the switches were fine, the reasoning was not.

### DUAL

**Left and right stop being a stereo pair.** Each becomes its own lane, rolling
its own pattern from the same SEED, and hard-panned to its own side. Same
knobs, same seed, two different manglings — one in each ear.

It is the widest thing this module does, and it is genuinely hard to get any
other way from a 10 HP effect: not a stereo effect on one signal, but two
signals being cut up differently.

**It costs most of the module's remaining CPU, and that is measured.** The
effected path renders twice instead of once. Driven hard:

| Run | Boot report | Peak |
|---|---|---|
| Normal, driven hard | STATE + PLAY | 50–75% |
| DUAL, driven hard | STATE + PLAY + **BLEND amber** | **75–90%** |
| DUAL + BPM detect | STATE + PLAY | 50–75% |

Note the last two: DUAL is not a fixed surcharge. What it costs depends on what
else is running — how many slices carry an effect, how finely the loop is cut —
so it reached amber in one hard run and stayed green in another. **75–90% is
the worst seen**, and it never reached red, which is 90%.

Treat DUAL as a deliberate setting rather than somewhere to leave the switch.
If you want it cheaper, **BLEND is the lever**: only the wet path renders
twice, so pulling BLEND back reduces the cost proportionally.

**All three switch positions still clock**, and all three still capture from
the jack. The switch has never selected whether the module listens to the gate,
only what the button does.

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

**STATE goes white while a PUNCH is held**, and magenta while the config layer
is open — where all four LEDs change meaning, as tabulated
[above](#the-config-layer).

**All four solid red** means the engine could not allocate its buffer. That
should be impossible — it needs 27.63 MB of a 32 MB pool, measured — but it
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

Noise Engineering's own firmware page does accept a file you choose yourself,
and it is how you put the module back to stock — but it writes internal flash,
and this build lives in QSPI behind the Daisy bootloader, so it cannot install
this one. See [FLASHING.md](firmware/FLASHING.md) for why, and what would have
to change.

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
| Allocator | **Verified natively.** Engine runs on the SDRAM pool, 27.63 MB of 32 MB |
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
