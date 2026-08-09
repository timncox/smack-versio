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
#include "clock_adapter.h"
#include "versio_alloc.h"

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

        /* Coarse (few-step) params get a deadband so summed CV noise can't
         * make them oscillate across a boundary. Continuous 0-100 params
         * don't need it -- one unit of change is inaudible. */
        if ((P[i].hi - P[i].lo) <= 24 && P[i].last != -32768) {
            if (norm > P[i].last_norm - HYST && norm < P[i].last_norm + HYST)
                continue;
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

/* ---- boot --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    versio_alloc_init(g_pool, POOL_BYTES);

    memset(&HOST, 0, sizeof(HOST));
    HOST.api_version      = 1;
    HOST.sample_rate      = SMACK_SR;
    HOST.frames_per_block = BLOCK_SIZE;
    HOST.get_bpm          = host_get_bpm;

    clk_init(&CLK, SMACK_SR, 120.0f);

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

    for (;;) {
        update_leds();
        hw.DelayMs(8); /* ~120 Hz LED refresh; audio runs in the callback */
    }
}
