/*
 * Smack Versio — live loop capture + seeded per-slice glitch, as Noise
 * Engineering Versio firmware.
 *
 * The DSP is smack_core, vendored unchanged from timncox/schwung-smack
 * @169905d (see vendor/smack_core.h). This file is only the host shim: it
 * wires the Versio's 7 knobs, 2 switches, button, gate and 4 LEDs to the
 * engine's string parameter API, and turns the gate jack into the 24 ppqn
 * MIDI clock the engine already speaks (clock_adapter.c).
 *
 * See DESIGN.md for the reasoning. The short version:
 *   - 48 kHz because libDaisy offers no 44.1 (sai.h)
 *   - 128-frame blocks because the engine's clock regression was built
 *     against 128 and misbehaves at Daisy's default 48 (verified natively)
 *   - the ring lives in SDRAM via a bump allocator, so the engine needs
 *     no edits at all
 */
#include "daisy_versio.h"
#include "util/PersistentStorage.h"
#include "clock_adapter.h"
#include "versio_alloc.h"
#include "settings.h"

extern "C" {
#include "vendor/smack_core.h"
}

#include <stdio.h>
#include <string.h>

using namespace daisy;

/* ---- hardware ---------------------------------------------------------- */

static DaisyVersio hw;
static CpuLoadMeter cpu;

/* Ring is SMACK_RING_FRAMES * 2ch * 2 bytes = 13.4 MB at 48 k; the lanes add
 * ~160 KB; smack_t itself is small. 16 MB of the Versio's 64 MB is ample. */
#define POOL_BYTES (16u * 1024u * 1024u)
static uint8_t DSY_SDRAM_BSS g_pool[POOL_BYTES];

static smack_t         *S;
static clock_adapter_t  CLK;
static host_api_v1_t    HOST;

/* Persistent settings live in the last QSPI sector (see settings.h). The
 * storage object only holds a reference to the peripheral, so constructing it
 * at static-init time is safe -- nothing touches the chip until Init(). */
static PersistentStorage<VersioSettings> STORE(hw.seed.qspi);
static VersioSettings                   *CFG = NULL;

/* How often the main loop offers the settings for saving. The offer is cheap;
 * PersistentStorage only erases when VersioSettings::operator!= says the change
 * was worth it, so this is a ceiling on write frequency, not a write rate. */
#define SAVE_INTERVAL_MS 10000u

#define BLOCK_SIZE 128

/* ---- host shim (the entire host surface smack_core needs) --------------- */

static float host_get_bpm(void)
{
    float b = clk_bpm(&CLK);
    return (b > 20.0f && b < 300.0f) ? b : 120.0f;
}

/* ---- parameter dispatch ------------------------------------------------- */

/*
 * Knobs are read every block, but smack_set_param takes strings. Formatting
 * seven of them per block would be pointless churn in the audio thread, so we
 * only send a parameter when its *quantized* value actually changes.
 *
 * The quantized knobs (length, resolution, seed) also need hysteresis: the
 * pot sums with its CV jack in analog hardware, so a knob parked on a step
 * boundary will otherwise chatter and re-slice the loop continuously. This is
 * the single most likely "why does it sound broken" bug on this platform.
 */
#define HYST 0.02f /* ~2% of full scale */

struct Param {
    const char *key;
    int         lo, hi;     /* inclusive integer range sent to the engine */
    int         last;       /* last value dispatched; -32768 = never */
    float       last_norm;  /* knob position at that dispatch */
};

/* Knob order matches the panel left-to-right, top-to-bottom.
 * loop_len indices 3..6 = 8/16/32/64 steps (loop_len_hs_table in the engine
 * is in half-steps: {2,4,8,16,32,64,128,256,512}), which covers the 16-64
 * step range this module is for. */
enum { P_FXD = 0, P_ORD, P_LEN, P_RES, P_WET, P_SEED, P_PITCH, P_COUNT };

static Param P[P_COUNT] = {
    { "fx_density",    0, 100, -32768, 0.0f },
    { "order_density", 0, 100, -32768, 0.0f },
    { "loop_len",      3,   6, -32768, 0.0f },
    { "slice_res",     0,   3, -32768, 0.0f },
    { "wet",           0, 100, -32768, 0.0f },
    { "seed",          0, 127, -32768, 0.0f },
    { "pitch_range",   1,  24, -32768, 0.0f },
};

/*
 * How far the knob must move, in normalised units, before a new value is
 * dispatched. Zero means "no deadband".
 *
 * "Few steps" is the wrong test for which params need one. SEED spans 0-127,
 * so by span it looks continuous -- but a single step of it re-rolls the whole
 * pattern, which is the loudest response any parameter here has to one LSB of
 * ADC noise. A knob parked on a boundary would re-roll continuously.
 *
 * It gets a band of exactly one step rather than HYST, and the difference
 * matters: HYST is 2% of travel, which is ~2.5 seeds wide, so using it would
 * make turning the knob skip most of the seed space. One step is enough to
 * stop chatter and narrow enough to still reach every seed.
 */
static float deadband_for(const Param &p, int idx)
{
    int span = p.hi - p.lo;
    if (span <= 0)     return 0.0f;
    if (idx == P_SEED) return 1.0f / (float)span; /* exactly one seed */
    if (span <= 24)    return HYST;
    return 0.0f; /* genuinely continuous: one unit of change is inaudible */
}

static int quantize(const Param &p, float norm)
{
    int span = p.hi - p.lo;
    int v    = p.lo + (int)(norm * (float)span + 0.5f);
    if (v < p.lo) v = p.lo;
    if (v > p.hi) v = p.hi;
    return v;
}

static void dispatch_knobs(void)
{
    char buf[16];
    for (int i = 0; i < P_COUNT; i++) {
        float norm = hw.GetKnobValue(i);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int v = quantize(P[i], norm);
        if (v == P[i].last) continue;

        /* Deadband, so summed CV noise can't oscillate a knob across a step
         * boundary. See deadband_for() for why SEED needs one despite its
         * 128-value span. */
        float dead = deadband_for(P[i], i);
        if (dead > 0.0f && P[i].last != -32768) {
            float moved = norm - P[i].last_norm;
            if (moved < 0.0f) moved = -moved;
            if (moved < dead) continue;
        }

        P[i].last      = v;
        P[i].last_norm = norm;
        snprintf(buf, sizeof(buf), "%d", v);
        smack_set_param(S, P[i].key, buf);
    }
}

/* ---- switches ----------------------------------------------------------- */

/* SW_0 = clock ratio, SW_1 = gate role. Switch3::Read() returns
 * POS_CENTER 0 / POS_UP 1 / POS_DOWN 2. */
static void dispatch_switches(void)
{
    static int last0 = -1, last1 = -1;

    int s0 = hw.sw[DaisyVersio::SW_0].Read();
    if (s0 != last0) {
        last0 = s0;
        clk_set_ratio(&CLK, s0 == Switch3::POS_UP     ? CLK_TICKS_DIV2
                          : s0 == Switch3::POS_DOWN   ? CLK_TICKS_2X
                                                      : CLK_TICKS_1X);
    }

    int s1 = hw.sw[DaisyVersio::SW_1].Read();
    if (s1 != last1) {
        last1 = s1;
        clk_set_mode(&CLK, s1 == Switch3::POS_UP   ? CLK_EXTERNAL
                         : s1 == Switch3::POS_DOWN ? CLK_INFER
                                                   : CLK_AUTO);
    }
}

/* ---- button: short = capture, long = re-roll ---------------------------- */

#define LONG_PRESS_MS 600.0f

static bool  btn_down      = false;
static bool  btn_long_done = false;

static void handle_button(void)
{
    hw.tap.Debounce(); /* ProcessAllControls() only does the ANALOG controls */

    if (hw.tap.RisingEdge()) {
        btn_down      = true;
        btn_long_done = false;
    }

    if (btn_down && !btn_long_done && hw.tap.Pressed()
        && hw.tap.TimeHeldMs() > LONG_PRESS_MS) {
        smack_set_param(S, "reroll", "1"); /* new pattern, same loop */
        btn_long_done = true;
    }

    if (btn_down && !hw.tap.Pressed()) {
        btn_down = false;
        if (!btn_long_done)
            smack_set_param(S, "capture", "1"); /* retro-grab the last N steps */
    }
}

/* ---- audio -------------------------------------------------------------- */

static void emit_to_engine(void *ctx, uint8_t byte)
{
    smack_on_midi((smack_t *)ctx, &byte, 1, 3); /* source 3 = host, as on Move */
}

static int16_t bufi[BLOCK_SIZE * 2];

static void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                          AudioHandle::InterleavingOutputBuffer out,
                          size_t                                size)
{
    cpu.OnBlockStart();

    hw.ProcessAllControls();
    handle_button();
    dispatch_switches();
    dispatch_knobs();

    /* Gate edge -> clock. One jack does double duty: in INFER/AUTO the
     * intervals between triggers supply the tempo (DESIGN.md §5). */
    if (hw.gate.Trig())
        clk_gate_edge(&CLK);
    clk_advance(&CLK, (int)size, emit_to_engine, S);

    /* float -1..1  ->  interleaved int16, which is what the engine takes.
     * Converting at the boundary keeps the engine bit-identical to the Move
     * build, so any difference in sound is a shim bug, not a rewrite bug. */
    for (size_t i = 0; i < size * 2; i++) {
        float v = in[i];
        if (v > 0.999969f)  v = 0.999969f;
        if (v < -1.0f)      v = -1.0f;
        bufi[i] = (int16_t)(v * 32767.0f);
    }

    smack_process(S, bufi, bufi, (int)size);

    for (size_t i = 0; i < size * 2; i++)
        out[i] = (float)bufi[i] * (1.0f / 32768.0f);

    cpu.OnBlockEnd();
}

/* ---- LEDs --------------------------------------------------------------- */

/*
 * With no display these four LEDs are the entire status surface, so they
 * answer the questions you actually have while patching:
 *   0  what is the engine doing        (idle / armed / recording / looping)
 *   1  where is the playhead           (pulses once per loop pass)
 *   2  blend position                  (clean loop <-> glitch pattern)
 *   3  clock source + CPU alarm        (see below)
 *
 * LED 3 doubles as the CPU meter because M1's whole job is answering "does
 * this fit in the budget?" -- past ~80% it goes red regardless of clock
 * state, so the first flash answers the question without a second one.
 */
static void update_leds(void)
{
    char buf[32];
    int  run = 0;
    if (smack_get_param(S, "run_state", buf, sizeof(buf)) >= 0)
        run = atoi(buf);

    switch (run) {
        case 1:  hw.SetLed(0, 1.0f, 0.5f, 0.0f); break; /* armed     amber */
        case 2:  hw.SetLed(0, 1.0f, 0.0f, 0.0f); break; /* recording red   */
        case 3:  hw.SetLed(0, 0.0f, 1.0f, 0.0f); break; /* looping   green */
        default: hw.SetLed(0, 0.0f, 0.0f, 0.15f);       /* idle      dim   */
    }

    float pos = 0.0f;
    if (smack_get_param(S, "play_frame", buf, sizeof(buf)) >= 0) {
        int pf = atoi(buf), lf = 0;
        char b2[32];
        if (smack_get_param(S, "loop_frames", b2, sizeof(b2)) >= 0) lf = atoi(b2);
        if (lf > 0) pos = 1.0f - ((float)pf / (float)lf); /* ramp per pass */
    }
    hw.SetLed(1, pos * 0.2f, pos * 0.6f, pos);

    float wet = (float)P[P_WET].last / 100.0f;
    if (P[P_WET].last < 0) wet = 0.0f;
    hw.SetLed(2, wet, 0.35f * (1.0f - wet), 1.0f - wet);

    float load = cpu.GetAvgCpuLoad();
    if (load > 0.80f)
        hw.SetLed(3, 1.0f, 0.0f, 0.0f);              /* CPU alarm      red */
    else if (!clk_locked(&CLK))
        hw.SetLed(3, 0.4f, 0.4f, 0.4f);              /* free-run     white */
    else if (CLK.mode == CLK_INFER)
        hw.SetLed(3, 0.5f, 0.0f, 0.8f);             /* inferred    purple */
    else
        hw.SetLed(3, 0.0f, 0.3f, 1.0f);             /* external      blue */

    hw.UpdateLeds();
}

/* ---- boot-time CPU report ----------------------------------------------- */

/*
 * Replays the *previous* session's worst-case block load on the four LEDs.
 *
 * DESIGN.md §8 calls CPU headroom the one open question that can kill this
 * project, and LED 3's live alarm can only be read by someone watching it --
 * which you are not, while playing with both hands. So the module records its
 * own peak while you play and tells you at the next power-up, when you can
 * actually look. Read it as a bar: more LEDs lit means less headroom.
 *
 * Runs after StartAudio() so the module is already passing audio through
 * during the readout -- there is no reason to hold sound hostage for it.
 */
static void show_cpu_peak_readout(float peak)
{
    for(int i = 0; i < 4; i++)
        hw.SetLed(i, 0.0f, 0.0f, 0.0f);

    if(peak <= 0.0f)
    {
        /* No data: first boot after a flash, or the sector was just reset.
         * Distinct from "measured, and low" so the two never get confused. */
        hw.SetLed(0, 0.0f, 0.0f, 0.25f); /* dim blue */
    }
    else
    {
        /* Always light something, so "plenty of headroom" cannot be mistaken
         * for "dead module". */
        hw.SetLed(0, 0.0f, peak >= 0.25f ? 0.6f : 0.15f, 0.0f);
        if(peak >= 0.50f)
            hw.SetLed(1, 0.0f, 0.6f, 0.0f); /* green  - over half   */
        if(peak >= 0.75f)
            hw.SetLed(2, 1.0f, 0.5f, 0.0f); /* amber  - getting tight */
        if(peak >= 0.90f)
            hw.SetLed(3, 1.0f, 0.0f, 0.0f); /* red    - it did not fit */
    }

    hw.UpdateLeds();
    hw.DelayMs(2500);
}

/* ---- boot --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    versio_alloc_init(g_pool, POOL_BYTES);

    /* Settings, before anything that wants a tempo. A struct written by some
     * other firmware -- or by an older layout of this one -- must never be
     * adopted, and DFU reflashing does not clear this sector, so the
     * magic/version guard is the only thing that catches it. */
    VersioSettings defaults = settings_defaults();
    STORE.Init(defaults, SETTINGS_QSPI_OFFSET);
    CFG = &STORE.GetSettings();
    if(!settings_valid(*CFG))
        STORE.RestoreDefaults();

    /* Last session's peak, before this session starts overwriting it. */
    float boot_peak = CFG->cpu_peak;

    memset(&HOST, 0, sizeof(HOST));
    HOST.api_version      = 1;
    HOST.sample_rate      = SMACK_SR;
    HOST.frames_per_block = BLOCK_SIZE;
    HOST.get_bpm          = host_get_bpm;

    /* Free-run at whatever tempo this module last locked to, not at an
     * arbitrary 120 -- with nothing patched to the gate, that is the only
     * memory the panel cannot express. */
    clk_init(&CLK, SMACK_SR, CFG->free_run_bpm);

    S = smack_create(&HOST);

    /* No USB serial logger: StartLog()/PrintLine() drag in the CDC stack and
     * full printf, and the STM32H750 has only 128 KB of internal flash. The
     * LEDs carry the diagnostics instead (see update_leds). */

    if (!S || versio_alloc_failed()) {
        /* Refuse to run half-initialised: all four LEDs red, no audio. A
         * silent module that looks alive is worse than one that says no. */
        for (;;) {
            for (int i = 0; i < 4; i++) hw.SetLed(i, 1.0f, 0.0f, 0.0f);
            hw.UpdateLeds();
            hw.DelayMs(100);
        }
    }

    /* Pre-capture behaviour: pass audio through. A Eurorack effect that is
     * silent until you press a button reads as broken. */
    smack_set_param(S, "monitor", "1");
    smack_set_param(S, "hw_input", "1");
    smack_set_param(S, "wet", "100");

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    /* Report last session, then start recording this one. */
    show_cpu_peak_readout(boot_peak);
    CFG->cpu_peak = 0.0f;
    cpu.Reset();

    uint32_t last_save = System::GetNow();

    for (;;) {
        update_leds();

        /* Worst block this session. Clamped because an overrunning callback
         * can report over 100%, and a value outside 0..1 would fail
         * settings_valid() on the next boot and throw the reading away. */
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > CFG->cpu_peak) CFG->cpu_peak = mx;

        /* The tempo we actually locked to becomes the next free-run default. */
        if (clk_locked(&CLK)) {
            float b = clk_bpm(&CLK);
            if (b > 20.0f && b < 300.0f) CFG->free_run_bpm = b;
        }

        /*
         * Saving erases a 4 KB QSPI sector, which blocks for tens of ms. That
         * is safe *here and only here*, for two reasons specific to this
         * build: under APP_TYPE = BOOT_SRAM the code runs from SRAM, so an
         * erase never stalls instruction fetch the way it would for an app
         * executing in place from QSPI; and the audio callback is an
         * interrupt, so it keeps rendering straight through. Never call
         * Save() from the callback.
         */
        uint32_t now = System::GetNow();
        if (now - last_save >= SAVE_INTERVAL_MS) {
            last_save = now;
            STORE.Save(); /* no-op unless operator!= says it was worth it */
        }

        hw.DelayMs(8); /* ~120 Hz LED refresh; audio runs in the callback */
    }
}
