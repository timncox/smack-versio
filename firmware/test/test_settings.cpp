/*
 * Native tests for the persistent-settings layer (M5).
 *
 * The thing under test is not "does it save" — it is "does it *refuse* to
 * save". libDaisy's PersistentStorage calls VersioSettings::operator!= to
 * decide whether to erase and rewrite a 4 KB flash sector, so that operator is
 * the only thing standing between a 100k-cycle flash and a main loop that
 * rewrites it hundreds of times a minute. These tests pin that down, plus the
 * magic/version guard that stops a reflashed build from trusting stale bytes.
 *
 * No hardware, no libDaisy — settings.h is deliberately dependency-free.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../settings.h"

static int checks = 0;
#define CHECK(c)                                                       \
    do                                                                 \
    {                                                                  \
        if(!(c))                                                       \
        {                                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);        \
            return 1;                                                  \
        }                                                              \
        checks++;                                                      \
    } while(0)

/* ---- the guard --------------------------------------------------------- */

static int test_defaults_are_valid(void)
{
    VersioSettings d = settings_defaults();
    CHECK(settings_valid(d));
    CHECK(d.magic == SETTINGS_MAGIC);
    CHECK(d.version == SETTINGS_VERSION);
    CHECK(d.cpu_peak == 0.0f); /* 0 means "no data", not "0% load" */
    printf("  ok  defaults are valid and self-identifying\n");
    return 0;
}

static int test_magic_and_version_always_force_a_rewrite(void)
{
    VersioSettings a = settings_defaults();
    VersioSettings b = a;

    /* Identical -> no write. */
    CHECK(a == b);

    /* A struct from a different firmware must never be silently adopted, even
     * when every other field happens to match. This is the case that bites
     * after a reflash: DFU writes the app image, not this sector. */
    b.magic = 0xDEADBEEFu;
    CHECK(a != b);
    CHECK(!settings_valid(b));

    b         = a;
    b.version = SETTINGS_VERSION + 1;
    CHECK(a != b);
    CHECK(!settings_valid(b));

    printf("  ok  magic/version mismatch forces a rewrite and fails validation\n");
    return 0;
}

static int test_validation_rejects_garbage(void)
{
    VersioSettings s = settings_defaults();

    s               = settings_defaults();
    s.free_run_bpm  = 0.0f; /* erased flash reads as 0xFF / nonsense */
    CHECK(!settings_valid(s));

    s              = settings_defaults();
    s.free_run_bpm = 1e9f;
    CHECK(!settings_valid(s));

    s          = settings_defaults();
    s.cpu_peak = 42.0f; /* load is a 0..1 fraction */
    CHECK(!settings_valid(s));

    s          = settings_defaults();
    s.cpu_peak = NAN;
    CHECK(!settings_valid(s)); /* every NaN comparison is false, so this
                                * relies on the >= / <= pair, not on != */
    printf("  ok  validation rejects out-of-range and NaN\n");
    return 0;
}

/* ---- the wear limiter -------------------------------------------------- */

static int test_small_changes_do_not_trigger_a_write(void)
{
    VersioSettings a = settings_defaults();
    VersioSettings b = a;

    b.free_run_bpm = a.free_run_bpm + (SETTINGS_BPM_EPS * 0.5f);
    CHECK(a == b); /* below threshold: no erase */

    b              = a;
    b.free_run_bpm = a.free_run_bpm + (SETTINGS_BPM_EPS * 2.0f);
    CHECK(a != b); /* above threshold: worth an erase */

    b          = a;
    b.cpu_peak = a.cpu_peak + (SETTINGS_CPU_EPS * 0.5f);
    CHECK(a == b);

    b          = a;
    b.cpu_peak = a.cpu_peak + (SETTINGS_CPU_EPS * 2.0f);
    CHECK(a != b);

    printf("  ok  sub-threshold drift does not cost a flash erase\n");
    return 0;
}

/*
 * The scenario that actually matters: a session where CPU load climbs from
 * idle to pegged in small steps, with the main loop offering the settings for
 * saving on every pass. Without the epsilon this is one erase per step.
 */
static int test_worst_case_session_write_count(void)
{
    VersioSettings stored = settings_defaults();
    VersioSettings live   = stored;
    int            writes = 0;
    int            i;

    for(i = 0; i <= 1000; i++)
    {
        float load = (float)i / 1000.0f; /* 0 -> 100% in 0.1% steps */
        if(load > live.cpu_peak)
            live.cpu_peak = load;

        if(live != stored) /* exactly what PersistentStorage::Save() asks */
        {
            stored = live;
            writes++;
        }
    }

    /* A full 0->100% sweep can cost at most 1/EPS erases. */
    printf("  ok  full 0-100%% CPU sweep costs %d flash writes (bound %d)\n",
           writes,
           (int)(1.0f / SETTINGS_CPU_EPS) + 1);
    CHECK(writes <= (int)(1.0f / SETTINGS_CPU_EPS) + 1);
    CHECK(writes >= 1); /* it must still record *something* */

    /* And the peak it converged on is the real one, within one epsilon. */
    CHECK(fabsf(stored.cpu_peak - 1.0f) <= SETTINGS_CPU_EPS);
    return 0;
}

/* ---- the address ------------------------------------------------------- */

/*
 * If this is wrong the module bricks, so it is worth an assertion rather than
 * a comment. The app image lives at QSPI offset 0x40000 and grows upward; the
 * settings sector must be above it and inside an 8 MB chip.
 */
static int test_settings_sector_cannot_collide_with_the_app(void)
{
    const uint32_t QSPI_SIZE   = 8u * 1024u * 1024u;
    const uint32_t APP_OFFSET  = 0x40000u; /* STM32H750IB_sram.lds ORIGIN */
    const uint32_t SECTOR_SIZE = 0x1000u;  /* QSPIHandle::Erase granularity */

    CHECK(SETTINGS_QSPI_OFFSET > APP_OFFSET);
    CHECK(SETTINGS_QSPI_OFFSET + SECTOR_SIZE <= QSPI_SIZE);
    CHECK((SETTINGS_QSPI_OFFSET % SECTOR_SIZE) == 0); /* whole sector */
    CHECK(sizeof(VersioSettings) + sizeof(uint32_t) < SECTOR_SIZE);

    /* It is the last sector, so no future app growth can reach it. */
    CHECK(SETTINGS_QSPI_OFFSET == QSPI_SIZE - SECTOR_SIZE);

    printf("  ok  settings sector 0x%X is the last one, %u KB clear of the app\n",
           SETTINGS_QSPI_OFFSET,
           (unsigned)((SETTINGS_QSPI_OFFSET - APP_OFFSET) / 1024));
    return 0;
}

int main(void)
{
    printf("settings (M5):\n");
    if(test_defaults_are_valid())
        return 1;
    if(test_magic_and_version_always_force_a_rewrite())
        return 1;
    if(test_validation_rejects_garbage())
        return 1;
    if(test_small_changes_do_not_trigger_a_write())
        return 1;
    if(test_worst_case_session_write_count())
        return 1;
    if(test_settings_sector_cannot_collide_with_the_app())
        return 1;
    printf("settings: all passed (%d checks)\n", checks);
    return 0;
}
