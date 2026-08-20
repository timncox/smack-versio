/*
 * Guards the parameter formatting that killed LIVE mode.
 *
 *   make -f firmware/Makefile.test
 *
 * The engine formats every readable parameter with integer conversions only.
 * That is not a style preference. On the Versio the firmware links newlib-nano
 * without -u _printf_float, where _printf_float is a WEAK reference that
 * nothing defines -- and when it is null, nano's vfprintf emits NOTHING for a
 * "%f" conversion. No error, no truncation, an empty string, which atoi() and
 * atof() both turn into 0.
 *
 * play_frame was "%.0f". It therefore read 0 forever on hardware, LIVE mode's
 * wrap test compared 0 against 0, and the feature never fired on any build --
 * while every native test passed, because glibc formats floats perfectly well.
 *
 * Which is exactly why these tests cannot catch the real failure directly:
 * they run against glibc, where "%f" works. What they CAN do is pin the
 * output format, so that if someone reintroduces a float conversion the
 * rounding or the decimal place changes and something here goes red. The
 * stronger guard is the comment at the flag in firmware/Makefile.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vendor/smack_core.h"

#define BLK 128

static float fake_bpm(void) { return 120.0f; }

static smack_t *make(void)
{
    static host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version      = 1;
    host.sample_rate      = SMACK_SR;
    host.frames_per_block = BLK;
    host.get_bpm          = fake_bpm;
    smack_t *s = smack_create(&host);
    assert(s);
    return s;
}

static void get(smack_t *s, const char *k, char *buf, int n)
{
    int r = smack_get_param(s, k, buf, n);
    assert(r >= 0);
    /* An empty string is the exact signature of the nano-printf failure. */
    assert(buf[0] != '\0');
}

/* The three densities are percentages and must come back as bare integers --
 * "42", never "42.0" and never "". */
static void test_densities_are_integers(void)
{
    smack_t *s = make();
    char b[32];

    smack_set_param(s, "fx_density", "42");
    get(s, "fx_density", b, sizeof(b));
    assert(!strcmp(b, "42"));

    smack_set_param(s, "order_density", "7");
    get(s, "order_density", b, sizeof(b));
    assert(!strcmp(b, "7"));

    smack_set_param(s, "wet", "100");
    get(s, "wet", b, sizeof(b));
    assert(!strcmp(b, "100"));

    smack_set_param(s, "wet", "0");
    get(s, "wet", b, sizeof(b));
    assert(!strcmp(b, "0"));

    printf("ok: densities format as bare integers\n");
    smack_destroy(s);
}

/* bpm_override round-trips through the one-decimal formatter, which is hand
 * rolled from integer division precisely so it needs no float conversion. */
static void test_one_decimal_round_trip(void)
{
    smack_t *s = make();
    char b[32];

    const char *cases[] = { "128.5", "60.0", "199.9", "90.1" };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        smack_set_param(s, "bpm_override", cases[i]);
        get(s, "bpm_override", b, sizeof(b));
        assert(!strcmp(b, cases[i]));
    }

    /* Out of range is rejected to 0, which must still print as "0.0" and not
     * as an empty string. */
    smack_set_param(s, "bpm_override", "5");
    get(s, "bpm_override", b, sizeof(b));
    assert(!strcmp(b, "0.0"));

    printf("ok: one-decimal params round-trip without a float conversion\n");
    smack_destroy(s);
}

/* play_frame is the one that actually broke. It is an integer count of frames
 * and must never regain a decimal point. */
static void test_play_frame_is_an_integer(void)
{
    smack_t *s = make();
    char b[32];

    get(s, "play_frame", b, sizeof(b));
    assert(strchr(b, '.') == NULL);
    assert(strspn(b, "-0123456789") == strlen(b));

    printf("ok: play_frame is a bare integer (the LIVE-mode bug)\n");
    smack_destroy(s);
}

/* Every readable parameter the Versio firmware touches, checked for the empty
 * string that a float conversion would produce on hardware. */
static void test_firmware_reads_are_never_empty(void)
{
    smack_t *s = make();
    char b[64];
    const char *keys[] = { "run_state", "loop_frames", "play_frame",
                           "detected_bpm", "bpm_override", "n_slices",
                           "fx_density", "order_density", "wet" };

    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        get(s, keys[i], b, sizeof(b));

    printf("ok: no readable param comes back empty\n");
    smack_destroy(s);
}

int main(void)
{
    test_densities_are_integers();
    test_one_decimal_round_trip();
    test_play_frame_is_an_integer();
    test_firmware_reads_are_never_empty();
    printf("param_format: all assertions passed\n");
    return 0;
}
