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
#include "dj_filter.h"
#include "versio_alloc.h"
#include "settings.h"

extern "C" {
#include "vendor/smack_core.h"
}

#include <stdio.h>
#include <stdlib.h>   /* atof, for the detected-BPM readback */
#include <math.h>     /* powf, for the DJ filter's cutoff curve */

/* settings.h is deliberately engine-free -- that is what lets its comparison
 * logic be tested on the laptop -- so it carries the punch range as a literal.
 * This is the thing that stops the two quietly drifting apart. */
static_assert(SETTINGS_PUNCH_FX_MAX == SMACK_FX_COUNT - 1,
              "settings.h punch range is out of step with smack_fx_t");
#include <string.h>

using namespace daisy;

/* ---- hardware ---------------------------------------------------------- */

static DaisyVersio hw;
static CpuLoadMeter cpu;

/* Sized in versio_alloc.h so this and test_versio_alloc.c cannot drift apart.
 *
 * The ring grew from 70 s to 150 s to hold two maximum loops rather than one
 * -- see the comment on SMACK_MAX_SECONDS. Cost is boot time: versio_calloc
 * zeroes the block, and smack_create() runs before the CPU readout, so ~29 MB
 * of SDRAM memset is added to boot latency rather than hidden by it. It lands
 * once, before audio starts. */
#define POOL_BYTES VERSIO_POOL_BYTES
static uint8_t DSY_SDRAM_BSS g_pool[POOL_BYTES];

static smack_t         *S;

/* Engine state, published by update_leds() at ~125 Hz and read by the audio
 * callback. Plain int: a torn read is impossible on a 32-bit aligned word,
 * and a one-block-stale value is inaudible. This exists so the callback can
 * know whether a loop is playing WITHOUT calling smack_get_param(), which is
 * an snprintf and has no business in an interrupt. 3 == SMACK_LOOPING. */
static volatile int     G_RUN_STATE = 0;
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

/*
 * libDaisy's software PWM expects Update() at its Init() samplerate, which
 * RgbLed leaves at the 1000.0f default. Everything that holds an LED state
 * has to pump it at this rate or the carrier drops below flicker fusion.
 */
#define LED_REFRESH_HZ   1000u
#define LED_RECALC_MS    8u     /* colours only need ~125 Hz; PWM needs 1 kHz */

/*
 * How often knob changes are pushed to the engine.
 *
 * Not 1 kHz, which is what the main loop runs at. smack_set_param("seed")
 * calls roll_pattern(), which rewrites the pattern while the audio interrupt
 * is rendering from it -- a real race, audible as digital artifacts. Sweeping
 * SEED across its 128 values at loop rate would fire up to 128 rebuilds, each
 * one colliding with the renderer.
 *
 * 20 ms caps that at 50 rebuilds a second, which is still far faster than a
 * hand can turn a knob and is imperceptible as latency. This narrows the race
 * rather than closing it; closing it properly means double-buffering the
 * pattern so the renderer never reads a half-written one.
 */
#define KNOB_DISPATCH_MS 20u
#define READOUT_HOLD_MS  2500u

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
 *
 * loop_len indices 3..8 = 8/16/32/64/128/256 steps. The engine's
 * loop_len_hs_table is in half-steps -- {2,4,8,16,32,64,128,256,512} -- and
 * loop_clock_ticks is hs*3 against a 24 ppqn clock, so index 8 is 1536 ticks
 * = 64 quarter notes = 16 bars.
 *
 * That is exactly what the ring was dimensioned for: SMACK_MAX_SECONDS is 70,
 * commented "16 bars at 55 BPM". Slower than that and capture_retro truncates
 * to whatever was actually recorded, which is already its behaviour when you
 * capture before the buffer has filled.
 *
 * Originally capped at index 6 on the assumption this module was for short
 * glitch loops. It is not -- opened up after playing it. */
/* Names for the table above. These are ADC channel numbers. */
enum { P_FXD = 0, P_RES, P_LEN, P_SEED, P_ORD, P_WET, P_PITCH, P_COUNT };

static Param P[P_COUNT] = {
    /*
     * The array index IS the ADC channel. The order below is therefore the
     * panel-position -> ADC-channel mapping, not the reading order of the
     * legend.
     *
     * Derived, not assumed. Noise Engineering's Desmodus Versio manual shows
     * the panel as:
     *
     *      Blend  ......  Regen        (top)
     *             Tone
     *      Speed  ......  Size
     *             Index
     *                     Dense
     *
     * and the ADC channels run down the columns -- left top-to-bottom, then
     * centre, then right:
     *
     *      ADC 0 Blend(top-left)   ADC 1 Speed(mid-left)
     *      ADC 2 Tone(centre)      ADC 3 Index(centre-lower)
     *      ADC 4 Regen(top-right)  ADC 5 Size(mid-right)
     *      ADC 6 Dense(lower-right)
     *
     * The anchor is ADC 4 = top-right, observed on hardware: with the old
     * table BLEND sat at index 4 and turned up at the top-right knob.
     *
     * Our functions keep their positions on the printed diagram, so the
     * entries below are ordered by which ADC drives which position.
     */
    { "fx_density",    0, 100, -32768, 0.0f },   /* ADC 0 - top-left    */
    { "slice_res",     0,   3, -32768, 0.0f },   /* ADC 1 - mid-left    */
    { "loop_len",      3,   8, -32768, 0.0f },   /* ADC 2 - centre      */
    { "seed",          0, 127, -32768, 0.0f },   /* ADC 3 - centre low  */
    { "order_density", 0, 100, -32768, 0.0f },   /* ADC 4 - top-right   */
    /* BLEND is handled in the audio callback, not sent to the engine -- see
     * the crossfade in AudioCallback. The entry stays so the knob is read and
     * still drives LED 2. */
    { NULL,            0, 100, -32768, 0.0f },   /* ADC 5 - mid-right   */
    { "pitch_range",   1,  24, -32768, 0.0f },   /* ADC 6 - lower-right */
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

        /* The PITCH knob has a second job it can be given (see CONFIG
         * LAYER). While it has it, pitch_range is pinned and this knob's
         * position means the filter, so dispatching it here would sweep a
         * parameter the panel is no longer steering. */
        if (i == P_PITCH && CFG->pitch_role) continue;

        int v = quantize(P[i], norm);
        if (v == P[i].last) continue;
        if (!P[i].key) { P[i].last = v; P[i].last_norm = norm; continue; }

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

/* ---- CONFIG LAYER ------------------------------------------------------- */

/*
 * A third gesture tier, because the panel ran out of room before the module
 * ran out of things worth setting.
 *
 * Triple-tap toggles it. While it is on, three of the knobs stop driving their
 * printed function and address a setting instead. There is no list to scroll
 * and nothing to page through, and that is not a simplification -- with seven
 * absolute controls on the front there is nothing to navigate. You turn the
 * knob for the thing you want, and all three settings are visible at once on
 * the LEDs rather than one at a time behind a cursor.
 *
 *   PITCH   (lower right)  what the PITCH knob does with the layer closed
 *   FX DENS (top left)     which effect the button punches in
 *   LENGTH  (centre)       clock: work the gate out, or always trust it
 *
 * A knob is only adopted once it MOVES (see CFG_PICKUP). Otherwise opening the
 * layer would overwrite all three settings with wherever the knobs happen to
 * be sitting for their printed jobs, and merely looking at your settings would
 * destroy them. Open it, read the LEDs, close it: nothing changes.
 */
static bool  G_CONFIG = false;
static float cfg_entry[P_COUNT];
static bool  cfg_moved[P_COUNT];

/* How far a knob must move before the layer believes you meant it. Larger than
 * HYST because this is a deliberate gesture, not a tracking deadband, and the
 * cost of a false positive here is a setting you did not ask for. */
#define CFG_PICKUP 0.06f

/*
 * SW_1, by what the button does in each position.
 *
 * The centre is the module as it has always been. The two ends each take the
 * button away and give it to something else -- which is the whole reason this
 * switch can carry three roles on a panel with only one button.
 */
enum gate_role_t { GATE_PUNCH = 0, GATE_NORMAL, GATE_DUAL };
static gate_role_t G_GATE     = GATE_NORMAL;
static bool        G_PUNCHING = false;

/*
 * The DJ filter's control, as ONE signed word.
 *
 * -1..0 sweeps a lowpass down, 0..+1 sweeps a highpass up, 0 is open. Written
 * by the main loop, read by the audio callback.
 *
 * One word deliberately. Cutoff, mode and an on/off flag as three variables
 * would tear against each other across the interrupt, and a torn LP/HP flip is
 * not a stale block -- it is a click. Packing all three into the sign and
 * magnitude of a single float makes a torn read impossible on an aligned word.
 *
 * The two curves meet at zero: a lowpass at 18 kHz and a highpass at 15 Hz are
 * both "open", so sweeping through the centre is continuous and the mode flip
 * lands where neither filter is doing anything. That is what lets the notch be
 * a real bypass without a crossfade to hide the transition.
 */
static volatile float G_DJ_CTL = 0.0f;

/*
 * Has the PITCH knob passed through the notch since it was given the filter?
 *
 * You SELECT the DJ filter by turning PITCH to the right, which is also a
 * filter position -- so without this, closing the config layer would drop a
 * highpass at roughly 1 kHz straight onto whatever is playing. That breaks the
 * layer's one promise, which is that leaving it never jumps anything.
 *
 * So the filter stays bypassed until the knob next reaches the centre notch,
 * and picks up from there. The notch is where a DJ filter lives between
 * gestures anyway, so the move that arms it is the move you were going to make.
 */
static bool G_DJ_ARMED = false;

/* Button state. Declared here rather than beside handle_button() because
 * moving the gate switch has to reset it -- a press that began under one role
 * must not be completed under another. */
static bool     btn_down     = false;
static bool     btn_cleared  = false;
static uint32_t btn_t0       = 0;
static uint32_t btn_last_tap = 0;
static int      btn_tap_n    = 0;

static void config_enter(void)
{
    for (int i = 0; i < P_COUNT; i++) {
        cfg_entry[i] = hw.GetKnobValue(i);
        cfg_moved[i] = false;
    }
    G_CONFIG = true;
}

static void config_exit(void)
{
    G_CONFIG = false;

    /*
     * Hand the knobs back WITHOUT dispatching them.
     *
     * A knob you turned in the config layer is no longer where its printed
     * function left it. Dispatching on the way out would jump that function to
     * a position you chose for something else entirely -- turn LENGTH to pick
     * the clock mode and the loop would re-length itself the moment you closed
     * the layer. Recording the new position as already-dispatched leaves the
     * engine exactly where it was and gives the knob back on your next touch,
     * which is what every soft-takeover control does.
     *
     * Note what this is NOT: last = -32768. That is the boot path and it means
     * "never dispatched", which dispatches on the very next pass -- precisely
     * the jump this exists to avoid.
     */
    for (int i = 0; i < P_COUNT; i++) {
        float n = hw.GetKnobValue(i);
        if (n < 0.0f) n = 0.0f;
        if (n > 1.0f) n = 1.0f;
        P[i].last      = quantize(P[i], n);
        P[i].last_norm = n;
    }
}

/* Has this knob moved far enough since the layer opened to be taken seriously? */
static bool cfg_touched(int idx)
{
    if (cfg_moved[idx]) return true;
    float d = hw.GetKnobValue(idx) - cfg_entry[idx];
    if (d < 0.0f) d = -d;
    if (d < CFG_PICKUP) return false;
    cfg_moved[idx] = true;
    return true;
}

static void dispatch_config(void)
{
    /* No hysteresis and no change detection: these write struct fields, not
     * engine parameters, so a jittering ADC costs nothing but a redundant
     * store. The one that reaches the engine (punch_fx) is only sent when the
     * button is actually pressed. */
    if (cfg_touched(P_PITCH))
        CFG->pitch_role = hw.GetKnobValue(P_PITCH) < 0.5f ? 0 : 1;

    if (cfg_touched(P_LEN))
        CFG->clock_ext = hw.GetKnobValue(P_LEN) < 0.5f ? 0 : 1;

    if (cfg_touched(P_FXD)) {
        int n = (int)(hw.GetKnobValue(P_FXD) * (float)(SETTINGS_PUNCH_FX_MAX + 1));
        if (n < 0)                          n = 0;
        if (n > (int)SETTINGS_PUNCH_FX_MAX) n = (int)SETTINGS_PUNCH_FX_MAX;
        CFG->punch_fx = (uint8_t)n;
    }
}

/*
 * Push config choices at whatever they actually control, on change only.
 *
 * Separate from dispatch_config() because the settings outlive the layer: they
 * come back from flash at boot, when no knob has been touched at all, and
 * something still has to act on them. This runs either way.
 */
static void apply_config(void)
{
    static int applied_role  = -1;
    static int applied_clock = -1;

    if (applied_role != (int)CFG->pitch_role) {
        applied_role = (int)CFG->pitch_role;
        if (CFG->pitch_role) {
            /* The knob has been taken for the filter, so PITCH RANGE loses its
             * control -- pin it at one octave, which is what it is worth when
             * nothing can steer it. */
            smack_set_param(S, "pitch_range", "12");
            G_DJ_CTL   = 0.0f;
            G_DJ_ARMED = false; /* wait for the notch -- see G_DJ_ARMED */
        } else {
            /* Handing the knob back: the engine must take its real position,
             * not the one the filter left behind. This IS the boot path, and
             * here it is the right one -- an immediate dispatch is exactly what
             * "the knob means this again" should do. */
            P[P_PITCH].last = -32768;
            G_DJ_CTL        = 0.0f;
        }
    }

    if (applied_clock != (int)CFG->clock_ext) {
        applied_clock = (int)CFG->clock_ext;
        clk_set_mode(&CLK, CFG->clock_ext ? CLK_EXTERNAL : CLK_AUTO);
    }
}

/*
 * PITCH knob -> filter control, with a real notch at the centre.
 *
 * The notch is not cosmetic. A DJ filter is a control you park in the middle
 * and reach for, so the middle has to be genuinely out of circuit rather than
 * merely nearly-open -- otherwise the module is quietly filtered all the time
 * and nobody can tell why the top end has gone. DEAD is wide enough to find by
 * feel on a knob with no detent.
 */
static void update_dj_ctl(void)
{
    const float DEAD = 0.06f;
    const float SPAN = 0.5f - DEAD;

    float n = hw.GetKnobValue(P_PITCH);
    float c = 0.0f;
    if (n < 0.5f - DEAD)      c = (n - (0.5f - DEAD)) / SPAN; /* -1 .. 0  LP */
    else if (n > 0.5f + DEAD) c = (n - (0.5f + DEAD)) / SPAN; /*  0 .. +1 HP */

    /* Freshly handed the knob: stay out of circuit until it reaches the notch,
     * so that selecting the filter cannot itself apply one. */
    if (!G_DJ_ARMED) {
        if (c != 0.0f) { G_DJ_CTL = 0.0f; return; }
        G_DJ_ARMED = true;
    }

    if (c < -1.0f) c = -1.0f;
    if (c >  1.0f) c =  1.0f;
    G_DJ_CTL = c;
}

/* ---- switches ----------------------------------------------------------- */

static void set_gate_role(gate_role_t role)
{
    if (role == G_GATE) return;

    /*
     * Release a punch that is still held.
     *
     * Leaving PUNCH mid-press would latch punch_fx with nothing left to
     * release it -- the switch has just taken away the button that would have.
     * Every slice would stay welded to one effect until the next power cycle,
     * and the control that looks responsible (the button) would do nothing.
     */
    if (G_PUNCHING) {
        smack_set_param(S, "punch_fx", "-1");
        G_PUNCHING = false;
    }

    /* A press begun under one role must not be completed under another. */
    btn_down    = false;
    btn_cleared = false;
    btn_tap_n   = 0;

    /*
     * Only PUNCH closes the config layer, because only PUNCH takes the button
     * away -- there would be no way left to leave. DUAL keeps every button
     * gesture, so the layer works there and there is no reason to evict it.
     *
     * This has to agree with what handle_button() will open, or the switch
     * would kick you out of somewhere you can walk straight back into.
     */
    if (G_CONFIG && role == GATE_PUNCH) config_exit();

    G_GATE = role;
    smack_set_param(S, "channel_mode", role == GATE_DUAL ? "1" : "0");
}

/* SW_0 = clock ratio, SW_1 = gate role. Switch3::Read() returns
 * POS_CENTER 0 / POS_UP 1 / POS_DOWN 2.
 *
 * POS_UP and POS_DOWN are libDaisy's names, not the panel's. These switches
 * are mounted horizontally on the Versio, so one position is left and one is
 * right -- and here is the trap:
 *
 *     POS_UP = LEFT,  POS_DOWN = RIGHT   (both switches, same way round)
 *
 * Anchored on the one unambiguous observation: DUAL is dispatched on
 * SW_1's POS_DOWN and appears in the RIGHT-hand position (2026-08-20). It is
 * a mode rather than a ratio, so there is nothing to misread about it.
 *
 * Everything else follows and agrees. SW_0 sends POS_UP to CLK_TICKS_DIV2,
 * which is 48 ticks per pulse and therefore the FAST side -- and the fast side
 * is heard on the left, which is what the manual has said all along.
 *
 * So the panel reads:
 *     SW_0  left = x2 (fast),  centre = =1,     right = /2 (slow)
 *     SW_1  left = PUNCH,      centre = NORMAL, right = DUAL
 *
 * This comment briefly claimed the two switches were mounted inverted from
 * each other. They are not. That came from reading "left is 2x, right is /2"
 * as a statement about where the labels sit rather than about what was heard,
 * and then building a whole theory on it. When a report is about behaviour,
 * work out the wiring from the behaviour -- do not translate it into a claim
 * about geometry first and reason from that.
 *
 * The docs speak left/centre/right; only this file speaks libDaisy's
 * vocabulary, and it should not leak back out. */
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

    /*
     * SW_1: left = PUNCH, centre = NORMAL, right = DUAL.
     *
     * This switch decides who owns the button, which is the only way a panel
     * with one button can offer three things that all want it.
     *
     *   NORMAL  the module as it has always been -- tap re-rolls, hold
     *           captures, hold longer clears, double-tap is LIVE, triple-tap
     *           opens the config layer.
     *   PUNCH   the button becomes a momentary effect punch and nothing else.
     *           Everything above is suspended here on purpose: a punch you
     *           have to think about is not a punch, and a gesture that might
     *           re-roll the pattern instead cannot be played hard.
     *   DUAL    the engine's dual-lane mode, which this panel has never
     *           exposed. L and R stop being a stereo pair and become two
     *           independent lanes, each rolling its own pattern from the same
     *           seed and hard-panned to its own side (pan_l = 0, pan_r = 100).
     *           Not free: the wet path renders render_lane() twice instead of
     *           once. Watch the boot CPU bar in this position.
     *
     * The clock source used to live in the left position (CLK vs AUTO). It has
     * moved to the config layer, and almost nothing is lost by the move: AUTO
     * already tells a steady train from sparse triggers and picks the right
     * one, with a test for exactly that in test_clock_adapter.c. Forcing
     * EXTERNAL only matters for a deliberately uneven clock you still want
     * read as a clock, which is a set-once decision -- and set-once decisions
     * are what a config layer is for. A performance switch position is worth
     * more than that.
     */
    int s1 = hw.sw[DaisyVersio::SW_1].Read();
    if (s1 != last1) {
        last1 = s1;
        set_gate_role(s1 == Switch3::POS_UP   ? GATE_PUNCH
                    : s1 == Switch3::POS_DOWN ? GATE_DUAL
                                              : GATE_NORMAL);
    }
}

/* ---- button: short = capture, long = re-roll ---------------------------- */

/*
 * Three tiers, ordered by how often you reach for them.
 *
 *   tap                 re-roll   -- new pattern, same loop
 *   hold  > 600 ms      capture   -- retro-grab the last N steps
 *   hold  > 2000 ms     clear     -- drop the loop, back to passthrough
 *
 * Re-roll is on the tap because it is the gesture you use constantly while
 * playing: you keep pressing until a pattern you like comes up. Capture is
 * deliberate and happens once, and discarding a take should be harder still.
 * This is the reverse of the original mapping, changed after playing it.
 */
#define LONG_PRESS_MS   600u
#define CLEAR_PRESS_MS 2000u

/*
 * Taps closer together than this belong to one gesture: two toggle LIVE, three
 * open the config layer.
 *
 * The first tap still re-rolls immediately. Re-roll is the gesture you use
 * constantly while playing and it must not wait to find out whether a second
 * tap is coming -- so the later taps UNDO rather than defer. That is why the
 * count is tracked as a count and not as a chain of pairs: by the time the
 * third tap lands, what the second one did is known and reversible.
 *
 * The cost is honest: three fast taps re-roll the pattern on the way past. In
 * the config layer that costs nothing, since the layer is where you go to set
 * up rather than to play.
 */
#define DOUBLE_TAP_MS   400u

/*
 * LIVE mode: re-capture once per loop pass, so the window keeps sliding onto
 * fresh audio and the slice effects land on what you are playing now rather
 * than on one frozen take.
 *
 * This works because capture is grid-aligned: capture_retro() takes the
 * quantum ending at the last boundary and then chases the current phase, so
 * firing it repeatedly stays in time instead of drifting or restarting.
 * Latency is one loop pass, so short LENGTH settings feel live and long ones
 * feel like a slow refresh.
 *
 * What this cannot be is a true insert. Half the palette -- REVERSE,
 * TAPESTOP, SCRATCH, RETRIG, REVAFTER, FREEZE -- operates on audio that has
 * already happened, so there is no zero-latency version of it to build.
 */
static volatile bool G_LIVE = false;

/* btn_down / btn_cleared / btn_t0 / btn_last_tap / btn_tap_n are declared up
 * in the CONFIG LAYER section, because set_gate_role() has to reset them. */

static void handle_button(void)
{
    hw.tap.Debounce();

    /*
     * Edges are derived from Pressed(), NOT from RisingEdge().
     *
     * Switch::Debounce() clears `updated_` at the top of every call and only
     * sets it again once System::GetNow() has advanced a whole millisecond:
     *
     *     updated_ = false;
     *     if(now - last_update_ >= 1) { updated_ = true; state_ = ...; }
     *
     * and RisingEdge() is `updated_ ? state_ == 0x7f : false`. state_ holds
     * 0x7f for exactly one call, so if that call lands in the same
     * millisecond as the previous one, the edge is gone for good -- the next
     * call sees 0xff.
     *
     * In the audio callback at 375 Hz every call was ~2.7 ms apart, so
     * `updated_` was always true and the edge was never missed. This loop
     * runs at ~1 kHz, exactly where that comparison becomes a coin flip, and
     * the button stopped registering the moment it moved here.
     *
     * Pressed() reads the debounced state directly (state_ == 0xff) and does
     * not consult `updated_`, so tracking the transition ourselves is correct
     * at any call rate -- including one that changes later.
     */
    const bool pressed = hw.tap.Pressed();

    /*
     * In the PUNCH position the button has exactly one job: hold to force every
     * slice through the chosen effect, release to drop back.
     *
     * Edge-triggered rather than level-driven so the engine sees one parameter
     * write per gesture instead of one per millisecond -- smack_set_param is an
     * strcmp chain and this loop runs at 1 kHz.
     */
    if (G_GATE == GATE_PUNCH) {
        if (pressed != G_PUNCHING) {
            G_PUNCHING = pressed;
            char b[8];
            /* punch_fx: "-1" releases, "0" punches CLEAN -- momentarily
             * dropping the glitch pattern, which is as much a punch as adding
             * an effect is -- and 1.. forces that effect on every slice. */
            if (pressed) snprintf(b, sizeof(b), "%d", (int)CFG->punch_fx);
            else         snprintf(b, sizeof(b), "-1");
            smack_set_param(S, "punch_fx", b);
        }
        return;
    }

    if (pressed && !btn_down) {
        btn_down    = true;
        btn_cleared = false;
        btn_t0      = System::GetNow();
    }

    const uint32_t held = System::GetNow() - btn_t0;

    /* Clear fires while held, so passing 2 s cancels the capture that the
     * release would otherwise trigger. Suspended in the config layer: nothing
     * there is worth losing a take over. */
    if (!G_CONFIG && btn_down && !btn_cleared && pressed && held > CLEAR_PRESS_MS) {
        smack_set_param(S, "clear", "1");
        btn_cleared = true;
    }

    if (btn_down && !pressed) {
        btn_down = false;
        if (btn_cleared)
            return;

        /*
         * In the config layer the button has one job: leave.
         *
         * A single tap, not another triple. Symmetry would be tidier but this
         * is the gesture you make when you are unsure where you are, and the
         * safe direction to resolve that is out. It also makes a fourth fast
         * tap self-correcting: three taps open the layer, a fourth closes it
         * again.
         */
        if (G_CONFIG) {
            config_exit();
            btn_tap_n = 0;
            return;
        }

        if (held > LONG_PRESS_MS) {
            smack_set_param(S, "capture", "1");
            /*
             * With nothing patched to the gate we free-run at whatever tempo
             * was last locked, which is a guess that survives across power
             * cycles and is often wrong. The engine can do better: it
             * autocorrelates an onset envelope over the last 8 s of ring
             * audio, which at capture time is exactly the audio you just
             * played. So ask it, and only when there is no real clock to
             * defer to. The scan is incremental on the audio thread and its
             * result is picked up in the main loop; nothing blocks here.
             */
            if (!clk_locked(&CLK)) smack_set_param(S, "detect_bpm", "1");
            return;
        }

        /* Tap tiers. See DOUBLE_TAP_MS for why later taps undo earlier ones
         * rather than the first tap waiting to find out what it is. */
        uint32_t now = System::GetNow();
        btn_tap_n    = (now - btn_last_tap < DOUBLE_TAP_MS) ? btn_tap_n + 1 : 1;
        btn_last_tap = now;

        if (btn_tap_n == 2) {
            G_LIVE = !G_LIVE;
            return;
        }
        if (btn_tap_n == 3) {
            G_LIVE    = !G_LIVE; /* put back what tap two just did */
            btn_tap_n = 0;
            config_enter();
            return;
        }
        smack_set_param(S, "reroll", "1");
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

    /*
     * Controls are NOT read here. They are handled in the main loop.
     *
     * dispatch_knobs() calls smack_set_param(), and the engine does real work
     * inside it -- a seed change runs roll_pattern(), which regenerates the
     * whole pattern synchronously. Doing that inside a 2.67 ms audio budget
     * overruns the block and is audible as glitching whenever the Seed knob
     * moves. Every other knob paid a smaller version of the same tax: one
     * snprintf per change, in an interrupt.
     *
     * The engine never expected this. Its own comments refer to "the shadow
     * UI's knob cache" -- on the Move build set_param comes from the UI
     * thread, and only smack_process() runs in audio. Putting the dispatch in
     * the callback was a porting mistake.
     *
     * What stays here is what is genuinely per-block: the gate edge and the
     * clock advance need frame accuracy, and the conversion and process calls
     * are the audio work itself.
     */

    /* Gate edge -> clock. One jack does double duty: in INFER/AUTO the
     * intervals between triggers supply the tempo (DESIGN.md §5). */
    if (hw.gate.Trig())
        clk_gate_edge(&CLK);
    /* frames, not samples -- passing `size` here ran the clock at double
     * tempo on top of everything else. See the note below. */
    clk_advance(&CLK, (int)(size / 2), emit_to_engine, S);

    /*
     * `size` is SAMPLES, not frames.
     *
     * libDaisy's interleaving callback hands over the whole interleaved
     * buffer and counts every sample in it -- audio.cpp strides `i += 2` and
     * touches both `fin[i]` and `fin[i+1]`. So a 128-frame stereo block
     * arrives as size == 256, and `frames` is size / 2.
     *
     * This was read as frames, which was wrong twice over and cost an entire
     * bench session. The loop ran to size * 2 == 512 and wrote 512 int16 into
     * a 256-entry bufi -- a 512-byte overrun on every callback, starting with
     * the first -- and smack_process() was told the block was 256 frames when
     * it was 128, breaking the block-size contract DESIGN.md calls
     * load-bearing. The module hard-faulted immediately after StartAudio(),
     * leaving the LEDs latched and the SAI DMA recycling a stale buffer,
     * which is the steady buzz.
     */
    const size_t frames = size / 2;

    /* Refuse to run rather than corrupt memory if the block size is ever not
     * what SetAudioBlockSize() asked for. bufi is fixed at BLOCK_SIZE * 2
     * samples; silently overrunning it is what made the original bug present
     * as an unexplained hard fault instead of an obvious wrong number. */
    if (size > (size_t)(BLOCK_SIZE * 2)) {
        for (size_t i = 0; i < size; i++) out[i] = in[i];
        cpu.OnBlockEnd();
        return;
    }

    /* float -1..1  ->  interleaved int16, which is what the engine takes.
     * Converting at the boundary keeps the engine bit-identical to the Move
     * build, so any difference in sound is a shim bug, not a rewrite bug. */
    for (size_t i = 0; i < size; i++) {
        float v = in[i];
        if (v > 0.999969f)  v = 0.999969f;
        if (v < -1.0f)      v = -1.0f;
        bufi[i] = (int16_t)(v * 32767.0f);
    }

    smack_process(S, bufi, bufi, (int)frames);

    /*
     * BLEND: dry input against the effected loop. Done here because this is
     * the only point where both signals still exist separately -- once the
     * engine mixes its own monitor path in, they cannot be pulled apart.
     *
     * P[P_WET].last is written by the main loop and read here. A torn read
     * costs one block at a slightly stale blend, which is inaudible; a lock
     * would not be.
     */
    int   bl  = P[P_WET].last;
    float wet = (bl < 0) ? 1.0f : (float)bl * 0.01f;

    /*
     * With no loop captured the engine returns silence (monitor = 0), so
     * blending toward it would fade the module into nothing. DESIGN.md is
     * explicit that a Eurorack effect which is silent until you press a
     * button reads as broken -- so until there is something to blend WITH,
     * BLEND is bypassed and the input passes through untouched.
     *
     * The effects are retro by nature: slicing, reordering and re-pitching
     * all need a recorded buffer, so there is no live-processed signal to
     * offer here even in principle.
     */
    if (G_RUN_STATE != 3) /* not SMACK_LOOPING */
        wet = 0.0f;

    float dry = 1.0f - wet;

    for (size_t i = 0; i < size; i++)
        out[i] = in[i] * dry + (float)bufi[i] * (1.0f / 32768.0f) * wet;

    /*
     * DJ filter -- last in the chain, and after the blend on purpose.
     *
     * That is what the control is for: one sweep across everything the module
     * is putting out, dry passthrough included. It is the only thing on this
     * panel that does anything before you have captured a loop, which for a
     * module whose every other control needs a recorded buffer is worth having.
     *
     * The filter itself is in dj_filter.h so it can be tested off the module;
     * what belongs here is only the decision to run it. See test_dj_filter.c.
     */
    {
        /* Static storage is zero-initialised, which is exactly what
         * dj_filter_reset() writes -- so the first block is already clean. */
        static dj_filter_t djf;
        static bool        dj_on = false;

        const bool want = CFG->pitch_role != 0;
        if (want != dj_on) {
            /* Start from silence rather than from whatever the integrators
             * held the last time this knob had the job -- otherwise every role
             * change fires the previous one's decay as a click. */
            dj_filter_reset(&djf);
            dj_on = want;
        }
        if (dj_on) dj_filter_block(&djf, out, (int)size, G_DJ_CTL);
    }

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
/*
 * The config layer's entire display: one LED per setting, all three at once.
 *
 * That is the payoff for spending knobs instead of building a menu. There is no
 * cursor, so there is nothing to be "on" and nothing to page past -- what the
 * module is set to is simply visible, and turning a knob moves the LED that
 * belongs to it. Opening the layer to check a setting is a complete gesture.
 *
 * LED 0 is the layer itself, in a magenta nothing else on this panel uses, so a
 * module left in config mode can never be mistaken for one doing something else.
 */
static void config_leds(void)
{
    hw.SetLed(0, 0.8f, 0.0f, 0.8f);                      /* in config  magenta */

    if (CFG->pitch_role) hw.SetLed(1, 0.0f, 0.3f, 1.0f); /* DJ filter     blue */
    else                 hw.SetLed(1, 0.0f, 1.0f, 0.0f); /* PITCH RANGE  green */

    /*
     * Punch effect, shown as a position rather than a name. There are 27 and
     * there are four LEDs, so the honest display is a coarse hue you can find
     * your way back to, with your ears doing the identifying -- the same deal
     * SEED already offers, and it works there.
     *
     * Clean punch is the exception and gets its own colour, because it is the
     * one choice you cannot recognise from the effect you hear.
     */
    if (CFG->punch_fx == 0) {
        hw.SetLed(2, 1.0f, 1.0f, 1.0f);                  /* punch clean  white */
    } else {
        float t = (float)(CFG->punch_fx - 1)
                / (float)(SETTINGS_PUNCH_FX_MAX - 1);
        hw.SetLed(2, 1.0f - t, t, 0.15f);                /* red -> green ramp  */
    }

    if (CFG->clock_ext) hw.SetLed(3, 0.0f, 0.3f, 1.0f);  /* trust the gate blue */
    else                hw.SetLed(3, 0.4f, 0.4f, 0.4f);  /* AUTO detect   white */
}

static void update_leds(void)
{
    char buf[32];

    /* The config layer owns the whole panel while it is open. Returning here
     * rather than letting both writers run is not tidiness: this function is
     * called every 8 ms and would overwrite the display between refreshes. */
    if (G_CONFIG) { config_leds(); return; }
    int  run = 0;
    if (smack_get_param(S, "run_state", buf, sizeof(buf)) >= 0)
        run = atoi(buf);
    G_RUN_STATE = run;

    switch (run) {
        case 1:  hw.SetLed(0, 1.0f, 0.5f, 0.0f); break; /* armed     amber */
        case 2:  hw.SetLed(0, 1.0f, 0.0f, 0.0f); break; /* recording red   */
        case 3:  G_LIVE ? hw.SetLed(0, 0.0f, 0.8f, 0.8f)   /* live     cyan  */
                        : hw.SetLed(0, 0.0f, 1.0f, 0.0f);  /* looping  green */
                 break;
        default: hw.SetLed(0, 0.0f, 0.0f, 0.15f);       /* idle      dim   */
    }

    /* A held punch takes LED 0 outright. It is momentary and it is the loudest
     * thing the module is doing, so it should be the thing the panel says. */
    if (G_PUNCHING) hw.SetLed(0, 1.0f, 1.0f, 1.0f);

    float pos = 0.0f;
    if (smack_get_param(S, "play_frame", buf, sizeof(buf)) >= 0) {
        int pf = atoi(buf), lf = 0;
        char b2[32];
        if (smack_get_param(S, "loop_frames", b2, sizeof(b2)) >= 0) lf = atoi(b2);
        if (lf > 0) pos = 1.0f - ((float)pf / (float)lf); /* ramp per pass */

        /*
         * LIVE: re-capture each time the playhead wraps, so the window slides
         * onto fresh audio every pass instead of repeating one take.
         *
         * Detected as the play frame going backwards, which is the wrap. Done
         * here because this is already where play_frame is read -- doing it
         * anywhere else would mean a second smack_get_param, and that is an
         * snprintf we do not need twice.
         *
         * Safe to fire repeatedly: capture is grid-aligned and chases phase,
         * so successive captures stay in time rather than restarting.
         */
        static int last_pf = 0;
        if (G_LIVE && run == 3 && lf > 0 && pf < last_pf)
            smack_set_param(S, "capture", "1");
        last_pf = pf;
    }
    hw.SetLed(1, pos * 0.2f, pos * 0.6f, pos);

    float wet = (float)P[P_WET].last / 100.0f;
    if (P[P_WET].last < 0) wet = 0.0f;
    hw.SetLed(2, wet, 0.35f * (1.0f - wet), 1.0f - wet);

#ifndef DIAG_HEARTBEAT
    /* LED 3 belongs to the heartbeat in a diagnostic build; writing it here
     * too would overwrite the blink with a steady colour and prove nothing. */
    float load = cpu.GetAvgCpuLoad();
    if (load > 0.80f)
        hw.SetLed(3, 1.0f, 0.0f, 0.0f);              /* CPU alarm      red */
    else if (!clk_locked(&CLK))
        hw.SetLed(3, 0.4f, 0.4f, 0.4f);              /* free-run     white */
    else if (CLK.mode == CLK_INFER)
        hw.SetLed(3, 0.5f, 0.0f, 0.8f);             /* inferred    purple */
    else
        hw.SetLed(3, 0.0f, 0.3f, 1.0f);             /* external      blue */
#endif

    /* Deliberately no UpdateLeds() here -- see refresh_leds(). This function
     * only decides colours; pushing them runs on a much faster clock. */
}

/*
 * Software PWM, and the reason the panel looked dead on the first hardware
 * run.
 *
 * The Versio's LEDs are plain GPIO, not a driver chip, so libDaisy makes
 * brightness by toggling the pin inside Led::Update():
 *
 *     pwm_ += 120.f / samplerate_;
 *     hw_pin_.Write(bright_ > pwm_ ? on_ : off_);
 *
 * RgbLed::Init() never passes a samplerate, so samplerate_ is the 1000.0f
 * default, and led.h says plainly that it "sets the rate at which Update()
 * will be called". Call it slower and the PWM carrier drops with it: the old
 * 125 Hz main loop (DelayMs(8)) produced about 15 Hz, far below flicker
 * fusion.
 *
 * Worse, Led::Set() cubes its argument for gamma. LED 0's idle 0.15 becomes
 * 0.15^3 ~= 0.003 -- a 0.3% duty cycle at 15 Hz, indistinguishable from off.
 * The panel was not dead; it was being strobed too slowly and too faintly to
 * see.
 *
 * The boot readout escaped this by accident: it calls UpdateLeds() exactly
 * once and then blocks in DelayMs(2500), so the pin is written once and held
 * -- 100% duty. That is why the readout was the one thing that worked, and
 * why its working was misleading rather than reassuring.
 */
static void refresh_leds(void)
{
    hw.UpdateLeds();
}

#ifdef DIAG_KNOBMAP
/*
 * Knob identification. DIAGNOSE.md.
 *
 * The panel-position -> ADC-index mapping was assumed to be panel reading
 * order and is wrong: BLEND turned up where the diagram says ORDER. Two
 * reported symptoms are not enough to derive a 7-way permutation, and
 * libDaisy cannot help -- its knobs are named KNOB_0..KNOB_6 against an
 * adc_pin[] table that describes traces, not panel positions.
 *
 * So ask the hardware. Turn one knob; the module reports which ADC index
 * moved, in binary on LEDs 0-2 (LED0 = bit0), with LED 3 white while a knob
 * is actually being turned. Indices 0..6 all fit in three bits.
 *
 * Work along the panel in the order the diagram shows and write down what
 * each position reports. That is the mapping, measured rather than assumed.
 */
static void knobmap_service(void)
{
    /*
     * Shows the RAW ADC value, read straight from the peripheral, bypassing
     * libDaisy's AnalogControl layer entirely.
     *
     * On the bench every channel read frozen through GetKnobValue(), and the
     * value looked pinned at full scale. DaisyVersio initialises these knobs
     * with flip = true, so a raw reading of ZERO becomes 1.0 after flipping --
     * which is precisely what a non-converting ADC would look like through
     * that layer.
     *
     * seed.adc.GetFloat() is the peripheral's own output: no flip, no
     * smoothing, no AnalogControl state. That splits the two possibilities
     * cleanly:
     *
     *   raw moves, GetKnobValue() did not  -> AnalogControl is the problem
     *                                         (slew coefficient, or Process()
     *                                          not being reached)
     *   raw does not move either           -> the ADC is not converting, and
     *                                         no knob mapping was ever going
     *                                         to be readable
     *
     *   Bank A (green): LED0..3 = raw ADC 0..3
     *   Bank B (red):   LED0..2 = raw ADC 4..6, LED3 dark
     *
     * Button switches bank.
     */
    static bool bank_b   = false;
    static bool was_down = false;

    bool down = hw.tap.Pressed();
    if (down && !was_down) bank_b = !bank_b;
    was_down = down;

    for (int i = 0; i < 4; i++) {
        int idx = bank_b ? (4 + i) : i;
        if (idx >= P_COUNT) { hw.SetLed(i, 0.0f, 0.0f, 0.0f); continue; }

        float v = hw.seed.adc.GetFloat((size_t)idx);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        /* Square-rooted so mid positions are clearly visible: Led::Set()
         * cubes for gamma, which buries everything below about 0.7. */
        float b = v * v; /* pre-compensate a little, then let Set() cube it */
        b = v;           /* keep it linear-in-value; brightness is relative */

        if (bank_b) hw.SetLed(i, b, 0.0f, 0.0f);
        else        hw.SetLed(i, 0.0f, b, 0.0f);
    }

    hw.UpdateLeds();
}
#endif

#ifdef DIAG_BOOTSTAGE
/*
 * Boot-stage indicator. DIAGNOSE.md.
 *
 * Two rounds of guessing have now failed to move the symptom, which means the
 * real unknown is not "which line is wrong" but "how far does it even get".
 * This counts the boot out on the panel: n green LEDs, held long enough to
 * read, at each milestone in main(). Whatever number it stops on is the step
 * that killed it.
 *
 * Full brightness on purpose. Led::Set() cubes its argument, so 1.0 stays 1.0
 * and the LED is on for every PWM comparison regardless of how fast Update()
 * is being called. That makes this readable even if the refresh-rate fix is
 * itself wrong -- a diagnostic that depends on the thing being diagnosed is
 * worth nothing.
 */
static void boot_stage(int n)
{
    for(int i = 0; i < 4; i++)
        hw.SetLed(i, 0.0f, 0.0f, 0.0f);
    for(int i = 0; i < n && i < 4; i++)
        hw.SetLed(i, 0.0f, 1.0f, 0.0f);
    for(uint32_t t = 0; t < 700u; t++) {
        hw.UpdateLeds();
        hw.DelayMs(1);
    }
    /* Dark gap so two consecutive stages cannot be read as one. */
    for(int i = 0; i < 4; i++)
        hw.SetLed(i, 0.0f, 0.0f, 0.0f);
    for(uint32_t t = 0; t < 250u; t++) {
        hw.UpdateLeds();
        hw.DelayMs(1);
    }
}
/* Stage 5 in blue rather than a fifth green LED, because there is no fifth
 * LED and "four green again" would be indistinguishable from stage 4. */
static void boot_mark_audio(void)
{
    for(int i = 0; i < 4; i++)
        hw.SetLed(i, 0.0f, 0.0f, 1.0f);
    for(uint32_t t = 0; t < 700u; t++) {
        hw.UpdateLeds();
        hw.DelayMs(1);
    }
    for(int i = 0; i < 4; i++)
        hw.SetLed(i, 0.0f, 0.0f, 0.0f);
    for(uint32_t t = 0; t < 250u; t++) {
        hw.UpdateLeds();
        hw.DelayMs(1);
    }
}
#else
#define boot_stage(n)     ((void)0)
#define boot_mark_audio() ((void)0)
#endif

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

    /* Hold the bar by pumping the software PWM at its rated 1 kHz, not by
     * writing once and sleeping. A single UpdateLeds() followed by
     * DelayMs(2500) latches the pin for the whole hold, i.e. 100% duty on
     * every lit LED -- which would erase the only thing this readout encodes.
     * "Measured and low" is 0.15 green and "measured, over a quarter" is 0.6;
     * at a latched 100% those are the same picture. */
    for (uint32_t t = 0; t < READOUT_HOLD_MS; t++) {
        refresh_leds();
        hw.DelayMs(1);
    }
}

/* ---- boot --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    boot_stage(1); /* hw.Init + cpu.Init survived */
    versio_alloc_init(g_pool, POOL_BYTES);
    boot_stage(2); /* SDRAM pool initialised */

    /* Settings, before anything that wants a tempo. A struct written by some
     * other firmware -- or by an older layout of this one -- must never be
     * adopted, and DFU reflashing does not clear this sector, so the
     * magic/version guard is the only thing that catches it. */
    VersioSettings defaults = settings_defaults();
    STORE.Init(defaults, SETTINGS_QSPI_OFFSET);
    boot_stage(3); /* QSPI settings read back */
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
    boot_stage(4); /* engine allocated and constructed */

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
    /*
     * monitor = 0 so the engine returns the LOOP ONLY. With monitor = 1 and
     * hw_input = 1 the engine adds the live input at full level no matter
     * what `wet` is (see the mixer: `out = ll + inl * dry`, dry == 1), and
     * `wet` only crossfades the loop's clean tap against its glitched render.
     * That is the Move build's behaviour and it is wrong for a Eurorack
     * insert, where BLEND has to mean dry-versus-effected.
     *
     * wet = 100 keeps the loop fully effected; the dry/wet crossfade is done
     * in the audio callback against the actual input, which is the only place
     * the two signals still exist separately.
     */
    smack_set_param(S, "monitor",  "0");
    smack_set_param(S, "hw_input", "1");
    smack_set_param(S, "wet",      "100");

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    boot_mark_audio(); /* stage 5, in blue so it cannot be miscounted as green */

    /* Report last session, then start recording this one. */
    show_cpu_peak_readout(boot_peak);
    CFG->cpu_peak = 0.0f;
    cpu.Reset();

    uint32_t last_save   = System::GetNow();
    uint32_t last_recalc = 0;
    uint32_t last_knobs  = 0;

    for (;;) {
        /*
         * Two different clocks on purpose.
         *
         * refresh_leds() is the software PWM and must run at ~1 kHz or the
         * panel strobes below flicker fusion and reads as dead -- that was
         * the first hardware run's failure.
         *
         * update_leds() only decides colours, and it is the expensive half:
         * three smack_get_param() calls, each an snprintf. Running that at
         * 1 kHz would triple the main loop's cost for no visible benefit,
         * so it stays at ~125 Hz, which is already faster than anyone can
         * see a colour change.
         */
        uint32_t t_led = System::GetNow();
#ifdef DIAG_HEARTBEAT
        /* Diagnostic: prove the loop is alive before anything else can fail.
         * Written first and at full brightness, so it survives both a broken
         * engine read and a wrong PWM rate. */
        {
            float on = ((t_led / 250u) & 1u) ? 1.0f : 0.0f;
            hw.SetLed(3, on, on, on);
        }
#endif
        /* Controls, at ~1 kHz -- faster than the 375 Hz block rate they used
         * to be polled at, and now outside the audio interrupt. */
        hw.ProcessAllControls();

        /* Button at full loop rate: the debounce shift register needs regular
         * calls, and press latency is felt directly. */
        handle_button();

#ifdef DIAG_KNOBMAP
        knobmap_service();
        hw.DelayMs(1);
        continue; /* nothing else runs in knob-identification builds */
#endif
        /* Knobs and switches are rate-limited -- see KNOB_DISPATCH_MS. */
        if (t_led - last_knobs >= KNOB_DISPATCH_MS) {
            last_knobs = t_led;
            dispatch_switches();
            /* apply_config() before either dispatcher, and outside the branch:
             * these settings come back from flash at boot, when no knob has
             * been touched and dispatch_config() has never run. */
            apply_config();
            if (G_CONFIG) {
                dispatch_config();
            } else {
                dispatch_knobs();
                if (CFG->pitch_role) update_dj_ctl();
            }
        }

        if (t_led - last_recalc >= LED_RECALC_MS) {
            last_recalc = t_led;
            update_leds();
        }
        refresh_leds();

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
        } else {
            /*
             * No clock patched: pick up a finished BPM scan, started by the
             * last capture. "-1" means still scanning and "0" means it found
             * nothing usable -- neither is a tempo, and both must be ignored
             * rather than clamped into one.
             *
             * Read here rather than in the audio callback because
             * smack_get_param is an snprintf. Polling it every loop is fine:
             * det_active gates the work, so this is a string compare and a
             * couple of integer divides until a scan actually completes.
             */
            char db[32];
            if (smack_get_param(S, "detected_bpm", db, sizeof(db)) >= 0) {
                float d = (float)atof(db);
                if (d >= 50.0f && d <= 200.0f) {
                    clk_set_free_bpm(&CLK, d);
                    CFG->free_run_bpm = d;
                }
            }
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

        hw.DelayMs(1); /* ~1 kHz, the rate libDaisy's software PWM expects */
    }
}
