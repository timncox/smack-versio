/*
 * Persistent settings for Smack Versio (M5).
 *
 * WHAT IS *NOT* HERE, AND WHY
 * ---------------------------
 * DESIGN.md §6 originally proposed persisting "seed, palette, switch prefs and
 * the last-used lengths". That list is wrong for this panel, and the reason is
 * physical: on the Versio every one of those is an *absolute* control — a pot
 * or a 3-position switch — that reports its true position on the first ADC
 * read. dispatch_knobs() would overwrite any restored value before the first
 * block of audio is rendered. Restoring them is a no-op at best.
 *
 * So the only things worth keeping across a power cycle are the values that no
 * control on the panel can express:
 *
 *   free_run_bpm  The tempo to free-run at with nothing patched to the gate.
 *                 The engine needs *some* BPM; 120 is an arbitrary guess. Once
 *                 the module has locked to your rack it can come back at your
 *                 tempo instead.
 *
 *   cpu_peak      The worst-case block load seen during the *previous* session,
 *                 replayed on the LEDs at boot. This exists because DESIGN.md
 *                 §8 lists CPU headroom as the one open question that can kill
 *                 the project, and the live LED-3 alarm can only be read by
 *                 someone who is watching it — which you are not, while playing
 *                 with both hands. This records the peak while you play and
 *                 tells you afterwards.
 *
 * FLASH WEAR
 * ----------
 * PersistentStorage::Save() erases a 4 KB sector and rewrites it whenever the
 * live struct differs from the stored one, and it uses *this* operator!= to
 * decide. So the epsilons below are not cosmetic — they are the wear limiter.
 * Without them, cpu_peak would rewrite the sector on nearly every main-loop
 * pass. Combined with the save cadence in smack_versio.cpp, the worst case is
 * a handful of writes per minute against a ~100k-cycle endurance rating.
 *
 * This header deliberately has no libDaisy dependency so the comparison logic
 * can be tested natively — see test/test_settings.c.
 */
#ifndef SMACK_VERSIO_SETTINGS_H
#define SMACK_VERSIO_SETTINGS_H

#include <stdint.h>
#include <math.h>

/*
 * Where the settings sector lives on the 8 MB QSPI chip.
 *
 * The app image starts at 0x90040000 — offset 0x40000 — because the BOOT_SRAM
 * linker script (STM32H750IB_sram.lds) declares QSPIFLASH ORIGIN = 0x90040000,
 * LENGTH = 7936K, reserving the low 256 KB for the bootloader. libDaisy's
 * PersistentStorage defaults to offset 0, which sits inside that reserved
 * region; the bootloader is a prebuilt blob, so whether it writes there is not
 * something the linker script can tell us.
 *
 * The *last* 4 KB sector is provably clear of both the reserved region and the
 * app image at any app size, so we use it and the question never arises.
 * QSPIHandle::Erase() rounds the start address down to a 4 KB sector boundary,
 * so this erases exactly the top sector and nothing below it.
 */
#define SETTINGS_QSPI_OFFSET 0x7FF000u /* 8 MB - 4 KB */

/*
 * PersistentStorage only validates its own one-word State tag, which survives
 * any reflash — the DFU write range covers the app image, not this sector. So
 * a build with a different struct layout would happily read the old bytes as
 * its own. magic + version are the real guard: on mismatch we restore
 * defaults rather than trusting stale data. Bump SETTINGS_VERSION whenever a
 * field is added, removed, reordered or re-scaled.
 */
#define SETTINGS_MAGIC   0x534D4B56u /* 'SMKV' */
#define SETTINGS_VERSION 1u

/* Significance thresholds — see FLASH WEAR above. */
#define SETTINGS_BPM_EPS 0.5f  /* BPM      */
#define SETTINGS_CPU_EPS 0.05f /* 5 points of load */

struct VersioSettings
{
    uint32_t magic;
    uint32_t version;
    float    free_run_bpm;
    float    cpu_peak; /* 0..1 */

    /* A change only counts as a change if it is big enough to be worth a flash
     * erase cycle. Identity fields are compared exactly — a magic/version
     * mismatch must always force a rewrite. */
    bool operator==(const VersioSettings &o) const
    {
        if(magic != o.magic || version != o.version)
            return false;
        if(fabsf(free_run_bpm - o.free_run_bpm) >= SETTINGS_BPM_EPS)
            return false;
        if(fabsf(cpu_peak - o.cpu_peak) >= SETTINGS_CPU_EPS)
            return false;
        return true;
    }

    bool operator!=(const VersioSettings &o) const { return !(*this == o); }
};

static inline VersioSettings settings_defaults(void)
{
    VersioSettings s;
    s.magic        = SETTINGS_MAGIC;
    s.version      = SETTINGS_VERSION;
    s.free_run_bpm = 120.0f; /* the engine needs a tempo before one is known */
    s.cpu_peak     = 0.0f;   /* 0 == "no data yet", rendered as such at boot */
    return s;
}

/* True if the struct read back from flash is one we are allowed to trust. */
static inline bool settings_valid(const VersioSettings &s)
{
    return s.magic == SETTINGS_MAGIC && s.version == SETTINGS_VERSION
           && s.free_run_bpm > 20.0f && s.free_run_bpm < 300.0f
           && s.cpu_peak >= 0.0f && s.cpu_peak <= 1.0f;
}

#endif /* SMACK_VERSIO_SETTINGS_H */
