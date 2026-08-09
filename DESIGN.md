# Smack Versio — Design

Porting Smack (Ableton Move / schwung) to Noise Engineering's Versio platform
as custom Daisy Seed firmware.

Written 2026-08-08. Claims are marked **✅ verified** (source given) or
**⚠️ assumed** (not measured — do not trust until checked on hardware).

---

## 1. What this is

A Eurorack module that continuously records stereo audio, lets you grab the
last 16–64 steps as a loop on a trigger, slices that loop to a grid, and plays
the slices back in a seeded random order with seeded random effects per slice.

That is Smack's existing behaviour, unchanged. The work is not DSP — it's
fitting Smack onto 7 knobs, 2 switches, 1 button and 1 gate jack, and giving
it a clock without MIDI.

**Verdict: feasible.** The engine is portable C with a 2-function host
surface, the hardware has 8× the memory needed, and two third-party firmwares
already ship the hard parts (§3).

---

## 2. Hardware

Versio is a Daisy Seed behind a Eurorack panel. All Versio modules (Desmodus,
Ruina, Imitor, Electus, Melotus…) are the same hardware with different
firmware and faceplates — buy whichever is cheapest used.

| | |
|---|---|
| MCU | STM32H750, 480 MHz Cortex-M7, FPU ✅ |
| RAM | **64 MB SDRAM**, 8 MB flash ✅ ([NE blog][ne-fw]) |
| Audio | stereo in / stereo out, 24-bit ✅ |
| Knobs | **7**, each summing with its CV jack ✅ |
| CV in | **7** — one per knob: Blend, Tone, Regen, Dense, Speed, Index, Size ✅ ([DV manual][dv-pdf] panel diagram) |
| Gate in | **1** (FSU), responds above +2 V ✅ |
| Switches | 2 × three-position (`Switch3`) ✅ |
| Button | 1 momentary (`tap`) ✅ |
| LEDs | 4 × RGB ✅ |
| Size | 10 HP, 1.5″ deep ✅ |

> **Correction worth recording:** an earlier read of the DV manual *text*
> concluded there were only 6 CV inputs (no Blend CV). The panel *diagram* on
> p.3 of the official PDF shows the jack field as `Blend / Tone / Regen /
> Dense` over `Speed / Index / Size / FSU` — **7 CV + 1 gate**. The FRGMNTS
> faceplate template agrees. Every Smack parameter can be CV-modulated.

**libDaisy has first-class support** ✅ — `daisy::DaisyVersio` exposes
`knobs[7]` (`AnalogControl`), `sw[2]` (`Switch3`), `tap` (`Switch`), `gate`
(`GateIn`), `leds[4]` (`RgbLed`), plus `SetAudioSampleRate()` /
`SetAudioBlockSize()` ([class ref][libdaisy-versio]).

**Noise Engineering officially sanctions custom firmware** ✅ — they publish a
how-to and point developers at the Daisy forum, while stating support covers
official firmware only ([NE blog][ne-fw]).

### Two hardware constraints that shape everything

1. **Pots sum with CV in analog hardware.** One ADC channel per knob — you
   *cannot* read a CV jack independently of its knob. A CV jack can only
   offset its own parameter, never act as a second gate input without
   sacrificing that knob.
2. **Sample rates are 8/16/32/48/96 kHz** ✅ — there is no 44.1 kHz. Smack is
   compiled at 44100 (`SMACK_SR`), so the port runs at 48 k and every derived
   constant re-derives (§6).

---

## 3. Prior art — what's already been proven on this hardware

### WTF! ([dubrussell][wtf]) — architecturally Smack already

A real-time stereo granular / beat-repeat firmware with a **~90-second
always-on buffer**. Its control model maps almost one-to-one:

| WTF! | Smack |
|---|---|
| ~90 s rolling buffer | ring buffer (70 s) |
| **PROB** — probability the effect triggers | `fx_density` |
| **VAR** — random variation across 6 params | the seeded roll |
| **SIZE** — grain size *derived from detected BPM* | `slice_res` |
| Grain mode switch: Fwd / PingPong / Rev | FX pool members |
| Button = manual trigger (hold to enable) | Capture |

**The move to steal:** WTF! derives BPM **from the intervals between triggers
at the gate jack**, and explicitly tolerates irregular rhythms rather than
demanding a steady clock. Also: 90 s of buffer is independent confirmation of
the memory budget in §8.

**Where Smack differs:** WTF! has no fixed captured loop — it grabs grains
from a rolling window. Smack captures a *loop* and repeats a *seeded pattern*
over it, identically each pass until re-rolled. That's the differentiator and
it survives the port intact.

### FRGMNTS Versio ([Acidclank][frgmnts], local manual) — the clock and the button

Four clock-synced effects in series (Stutter → Tape → Filter → Wash).
Contributes three transferable decisions:

1. **A dedicated CLK jack with a `×2 / =1 / /2` divider switch** — a real
   clock input plus a musical ratio control, using the gate jack.
2. **The button lesson.** v1.2 changelog: *"TAP button repurposed as BYPASS
   switch (Tap Tempo removed)."* They shipped tap tempo, then spent the button
   on something better once a real clock input existed. One button is scarce;
   don't burn it on tempo.
3. **Idioms:** a single bipolar knob for LP/HP with centre = off (same as DV's
   Tone); a 3-way switch to pick effect flavour instead of a knob; a printable
   faceplate template shipped with the `.bin`; a "third-party firmware, don't
   contact Noise Engineering about it" disclaimer.

### Others

**CRCLTR** ✅ — stereo looper, 16 s + 32 s loops, record-while-held, no clock
sync ([repo][crcltr]). **Praetereo** — evolving-loop firmware with two big
asynchronous loops. Both confirm loopers ship here; neither does step-synced
slicing.

**Nothing found does what Smack does** — a step-quantized captured loop with a
seeded per-slice effect pattern. NE's own Melotus Versio is granular texture,
not grid slicing. ⚠️ Absence of prior art is from a few searches, not an
exhaustive survey.

### What the binaries reveal

Nothing. WTF! is 91 KB, FRGMNTS 85 KB, both raw stripped `.bin` — no symbols,
no toolchain markers. The only signal is size, which confirms 8 MB of flash is
a non-constraint.

---

## 4. Control surface

Seven knobs is exactly Smack's parameter count. All seven are CV-modulatable,
which the Move version never had — **FX Density and Order Density under CV is
the most exciting thing this port gains.**

| Control | Parameter | Notes |
|---|---|---|
| Knob 1 | **FX Density** | `fx_density` — CV target #1 |
| Knob 2 | **Order Density** | `order_density` — CV target #2 |
| Knob 3 | **Loop Length** | quantized 16 / 24 / 32 / 48 / 64 steps |
| Knob 4 | **Slice Res** | quantized; re-slices live |
| Knob 5 | **Blend** | A/B — clean loop ↔ pattern |
| Knob 6 | **Seed** | quantized; the "I don't like this pattern" knob |
| Knob 7 | **Depth** | global fxp bias / pitch range |
| SW_0 | **Clock ratio** | `÷2 / =1 / ×2` — ticks per input pulse (§5) |
| SW_1 | **Gate role** | `CLK / TRIG / AUTO` (§5) |
| Button | **Capture** (short) / **Re-Roll** (long) | FRGMNTS's lesson |
| Gate | clock and/or capture trigger | §5 |

**Quantized knobs need hysteresis.** Loop Length, Slice Res and Seed are
discrete, and the knob reading includes summed CV noise. Without a deadband
(⚠️ start at ±3 % of a step, tune by ear) a knob parked on a boundary will
chatter and re-slice continuously. This is the most likely "why does it sound
broken" bug in the whole port.

### LEDs (4 × RGB)

| LED | Shows |
|---|---|
| 0 | State — amber pulse armed, red recording, green pulse on downbeat |
| 1 | Playhead — brightness ramp per slice, hue = current slice's FX family |
| 2 | Blend / A-B position |
| 3 | Clock source — blue external, purple inferred, white free-run; blinks on beat |

LED 3 doubles as the debugging surface. With no display, "is it locked to my
clock?" needs an answer you can see across the room.

---

## 5. Clock architecture — the core design

Versio has no MIDI and one gate jack. Smack's engine already speaks **24 ppqn
MIDI clock** via `smack_on_midi()`, with `0xFA` resetting bar phase.

**So don't touch the timing model. Write a clock adapter that synthesizes
`0xF8` ticks.** This is the single most important decision in the port: the
entire capture-alignment, slice-grid and drift-compensation logic — the part
that took the longest to get right on the Move — carries over untouched.

### Three tiers, degrading gracefully

**Tier 1 — external clock (SW_1 = CLK).** FRGMNTS's approach. Detect rising
edges on `gate` in the audio callback (~1 ms resolution at a 48-frame block —
⚠️ adequate for a 16th-note grid, unmeasured). Measure the interval `I` in
frames between pulses; SW_0 sets ticks-per-pulse `T` (`÷2`=48, `=1`=24,
`×2`=12, i.e. `=1` means one pulse per quarter note). Emit a synthetic `0xF8`
every `I/T` frames using a fractional accumulator, re-locking phase on each
pulse so error can't accumulate. After >2 s of silence, the next pulse also
emits `0xFA` to reset bar phase.

**Tier 2 — inferred tempo (SW_1 = TRIG).** WTF!'s approach. The gate is a
*capture trigger*, and the intervals between triggers give the tempo: median
of the last N intervals, octave-folded into 60–180 BPM. Smack already has
octave-biasing logic in its v0.6.0 `detect_bpm` — reuse that helper rather
than writing a second one. Then free-run 24 ppqn at that tempo, re-syncing
phase on each trigger.

**Tier 3 — audio detection / free-run.** Nothing patched: Smack's existing
onset-strength autocorrelation BPM detection runs on the input audio, with a
knob-set tempo as the floor.

### Why this resolves the one-jack problem

Earlier framing said you must choose: clock input *or* CV-triggerable capture.
Tier 2 dissolves it — **if the gate is your capture trigger, its own intervals
supply the tempo, so one jack does both.** `AUTO` on SW_1 should be the
default: take an external clock if pulses arrive faster than the loop, else
treat them as capture triggers and infer.

---

## 6. Engine port plan

`smack_core.c` needs a Versio host, not surgery.

### The host shim is two functions

Grepped 2026-08-08 ✅ — smack_core touches exactly `host->get_bpm()` (4 call
sites) and `host->frames_per_block` (2). Everything else is self-contained.
`get_bpm()` returns the tier-2/3 inferred tempo; `frames_per_block` is the
Daisy block size.

### Constants (`smack_core.h:19-23`)

| Constant | Now | At 48 k |
|---|---|---|
| `SMACK_SR` | 44100 | 48000 |
| `SMACK_RING_FRAMES` | `SR × 70` | 3,360,000 frames |
| `SMACK_EDGE_FADE` | 96 (≈2.18 ms) | 105, to keep the same *duration* |
| `SMACK_DLY_LEN` | 8192 | re-check the delay-variant table — the comment notes a quarter-note echo already doesn't fit at 120 BPM, and 48 k makes it 8.8 % worse |
| BPM-detect hop | 512 | re-derive the 8 s window and 60–180 BPM search bounds |

### Allocation → static SDRAM

Four `calloc` sites, all in `smack_create()` (`smack_core.c:681-690`). Daisy
has no meaningful heap; these become statically-placed arrays:

| Buffer | Size @48 k | Where |
|---|---|---|
| `ring` (int16 ×2ch) | **13.4 MB** | SDRAM (`DSY_SDRAM_BSS`) |
| `lane[k].dly` (float ×2ch ×8192) | 64 KB × 2 lanes | internal SRAM |
| `lane[k].verb` (float × `VERB_TOTAL`) | small | internal SRAM |
| `smack_t` struct | small | internal SRAM |

Keep the delay/verb lines in internal SRAM — they're read every sample and
SDRAM is the slow path. ⚠️ If effects sound wrong or CPU spikes, buffer
placement is the first thing to check.

### Sample format

Daisy's callback is float `-1..1`; smack_process takes interleaved int16.
**Use a conversion shim at the block boundary** rather than converting the
engine — it keeps the port bit-identical to the Move build, so a difference in
sound is a port bug, not a rewrite bug. Keep the ring int16: it halves both
memory and SDRAM bandwidth.

### Parameter plumbing

Knob → `smack_set_param("fx_density", "42")`. The param API is strings, so
**cache the last dispatched value and only format on change** — `snprintf` for
7 knobs every block is pointless churn in the audio thread. Dispatch at the
top of the callback, before render.

### Persistence

The Daisy's QSPI flash via `PersistentStorage` can hold seed, palette, switch
prefs and the last-used lengths across power cycles. ⚠️ Not required for v1.

### Illustrative skeleton

Names below are from memory of libDaisy's API — **verify against
`daisy_versio.h` before writing real code.**

```cpp
DaisyVersio hw;
static int16_t DSY_SDRAM_BSS ring[SMACK_RING_FRAMES * 2];

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out, size_t size) {
    hw.ProcessAllControls();
    clock_adapter_tick(size);        // may call smack_on_midi(0xF8 / 0xFA)
    dispatch_changed_knobs();        // only on change, with hysteresis
    if (hw.tap.RisingEdge())  capture_pressed();
    if (hw.tap.TimeHeldMs() > 600) reroll();
    float_to_i16(in, buf, size);
    smack_process(s, buf, buf, size);
    i16_to_float(buf, out, size);
}
```

---

## 7. Scope

**v1 ships:** always-on capture, trigger-to-loop, 16–64 step quantized loops,
grid slicing, seeded reorder, seeded per-slice FX at density, blend, the
three-tier clock, 7 CV inputs, LED feedback.

**Cut — and this is the honest cost:** the 23-pad palette, per-slice pinning
and locks, step-button editing, the web editor, screen-reader support, help
JSON. There is no display and no pad grid. Both reference firmwares are
density/probability instruments with zero per-slice editing, which is good
evidence the knob-shaped subset stands on its own — but Smack loses its
deepest feature and you should decide that's acceptable before starting.

**v2 candidates:** dual-mono lanes (Smack's existing L/R independent-lane mode
maps beautifully to stereo Eurorack — two independent glitch lanes from one
module); QSPI presets; CV-triggered slice play (needs a jack it doesn't have —
open question §11).

---

## 8. Budgets

**Memory ✅ — a non-issue.** The 70 s ring is 13.4 MB of 64 MB. WTF! ships 90 s
in practice. `SMACK_MAX_SECONDS` could go to several minutes if wanted;
`SMACK_MAX_SLICES` (512) is the real ceiling on loop length × resolution.

**Flash ✅** — reference firmwares are 85–91 KB against 8 MB.

**CPU ⚠️ — unprofiled, the one genuine unknown.** 480 MHz single-core M7
versus the Move's Cortex-A53. The saving grace is that only 1–2 effects render
at a time (one per lane), not 26. Profile Verb, Freeze, PShift and Scatter
first — they're the expensive ones. If it doesn't fit: drop to one lane, or
trim the FX pool.

---

## 9. Build, flash, debug

- **Toolchain:** libDaisy + DaisySP, `arm-none-eabi-gcc`, standard Daisy
  Makefile. ✅ `DaisyVersio` board class is upstream.
- **Flashing:** micro-USB on the Daisy Seed, **on the back of the module** ✅ —
  you pull the module from the case to flash. Expect that in the edit loop.
  DFU via `make program-dfu` or the Electrosmith web programmer.
- **Debugging:** no display. USB serial `Logger` when tethered; LED 3 as the
  always-visible clock-lock indicator otherwise.
- **Keep `make test` working.** Smack's `test/host_sim.c` runs the engine
  natively with a simulated clock and no hardware. Point it at the Versio
  constants and the 48 k re-derivation gets verified on a laptop, before any
  module is bought. **Do this first — it's free.**

---

## 10. Distribution

Both references distribute the same way: a `.bin`, a PDF manual, and a
printable faceplate template. FRGMNTS sells via Gumroad; WTF! is free on a
personal site. Both carry a "third-party firmware, use at your own risk, don't
contact Noise Engineering about it" disclaimer — copy that.

The mismatched-legend problem is solved by convention: you ship a faceplate
template and users print or order one.

Tim's existing Schwung store pattern (thin repo + `release.json` + tagged
release tarballs) maps cleanly onto `.bin` + manual + faceplate.

---

## 11. Open questions

1. **CPU headroom** — the only thing that could kill the project. Measure
   before buying anything beyond one module.
2. **Gate edge resolution at block rate** — is ~1 ms jitter audible on a
   16th grid? ⚠️ Assumed fine.
3. **Hysteresis deadband** for quantized knobs — needs tuning by ear.
4. **Does `AUTO` gate-role detection actually feel right**, or does it need to
   be an explicit switch choice? Real playing will decide.
5. **Slice triggering has no input** — the gate is spoken for. Is per-slice CV
   trigger (Smack's `pad_play`) worth a switch mode that repurposes the gate?
6. **Which Versio to buy** — all identical hardware ✅; pick on used price.
7. ⚠️ **Is the 24 ppqn synthesis stable under a wobbly analog clock?** The
   engine's regression window assumed 128-frame host callback quantization;
   Daisy's default block is 48.

---

## 12. Risks

**1. Forking an unverified engine.** Smack v0.15.x is hardware-test
unconfirmed on the Move. A bug found on Versio won't tell you which side it
came from. *Mitigation: verify Smack on the Move first — it's cheaper than
debugging two unknowns at once.*

**2. The UI collapse may matter more than it looks on paper.** Per-slice
pinning is Smack's deepest feature and it cannot come along. *Mitigation:
decide up front that the density+seed instrument is the product.*

**3. CPU.** See §8. *Mitigation: profile early, one lane if needed.*

**4. Project sprawl.** There are already six active Schwung modules with
pending hardware verification (smack, filltron, mark, belt, mono, work). This
adds a seventh thing in the same domain, on hardware Tim doesn't own yet.
*Mitigation: this doc is deliberately the whole deliverable for now.*

---

[ne-fw]: https://noiseengineering.us/blogs/loquelic-literitas-the-blog/create-your-own-firmware-on-a-versio-module/
[libdaisy-versio]: https://electro-smith.github.io/libDaisy/classdaisy_1_1_daisy_versio.html
[dv-pdf]: https://imagescdn.juno.co.uk/manual/881913-01U.pdf
[wtf]: https://2020.dubrussell.com/resources/wtf/
[frgmnts]: https://acidclank.gumroad.com/l/frgmntsversio
[crcltr]: https://github.com/s3g/crcltr
