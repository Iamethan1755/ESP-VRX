/* C5VRX-5 by Twotoz and the C5VRX contributors: pure deviation estimator
 * and profile choice of profile.c (host test: test/test_profile.c).
 *
 * The AFC gives, per burst-confirmed window, the sync tip and porch
 * frequency; their distance is deviation x 1.914 MHz (40 of 140 IRE over
 * 6.7 MHz). A decision needs three batches of 32 estimates, each with a
 * spread (MAD) <= 8 % of its median, and all batch medians within 5 % of
 * each other. A batch that fails restarts the count, so a burst of bad
 * windows can delay a decision but never shape it.
 *
 * Profiles: DEFAULT (normal >= 12 MHz class, kp 0.85), KP060 / KP050
 * (narrow >= 10 MHz class, calmer tracker) for low deviations, WIDE (wide
 * >= 16 MHz class, kp 0.85) for deviations >= 1.6. A profile sets the
 * filter and the kp level it starts from; with AUTO the kp/LP ladder then
 * owns kp and U85Q-LP on every profile (the KP060/KP050 LUTs are ladder
 * levels of the same program). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DEV_SYNC_KHZ     1914
#define DEV_BATCH        32u
#define DEV_BATCHES      3u
#define DEV_MAD_PCT      8
#define DEV_AGREE_PCT    5
/* Profile boundaries (deviation x100) with margin below the simulated
 * break-even points 0.85 / 1.15 (research). */
#define DEV_KP050_MAX    80
#define DEV_KP060_MAX    105
/* Wide VTXs: the 12 MHz default clips their FM spectrum. Range limit,
 * bit-exact U85Q (research/data/wide.txt): deviation 1.7 3.64 dB at
 * 12 MHz vs 3.26 at 16; deviation 2.0 8.03 vs 5.27; deviation 1.4 still
 * best at 12 (2.08 vs 2.54). Break-even ~1.55; WIDE from 1.60. A wrong WIDE
 * on a narrow VTX costs ~0.5-0.8 dB, far less than a wrong narrow profile
 * on a wide one, so the highest-batch rule (errs wide) is kept. */
#define DEV_WIDE_MIN     160

/* Values are stable (NVS-independent, but logged and compared). */
enum { PROFILE_DEFAULT, PROFILE_KP060, PROFILE_KP050, PROFILE_WIDE, PROFILES };

typedef struct {
    int32_t span[DEV_BATCH], medians[DEV_BATCHES];
    unsigned n, batches;
    int result_x100;       /* -1 until decided */
    unsigned rejected;     /* batches dropped for spread or disagreement */
} dev_estimator_t;

static inline void dev_reset(dev_estimator_t *e)
{
    memset(e, 0, sizeof(*e));
    e->result_x100 = -1;
}

static inline int32_t dev_median(int32_t *v, unsigned n)
{
    for (unsigned a = 1; a < n; ++a)
        for (unsigned b = a; b > 0 && v[b - 1] > v[b]; --b) { int32_t t = v[b]; v[b] = v[b - 1]; v[b - 1] = t; }
    return n & 1u ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* One sync-to-porch span (kHz, > 0). Returns the deviation x100 once
 * decided, else -1. */
static inline int dev_add(dev_estimator_t *e, int32_t span_khz)
{
    if (e->result_x100 >= 0) return e->result_x100;
    if (span_khz <= 0) return -1;
    e->span[e->n++] = span_khz;
    if (e->n < DEV_BATCH) return -1;
    e->n = 0;
    int32_t v[DEV_BATCH], d[DEV_BATCH];
    memcpy(v, e->span, sizeof(v));
    int32_t med = dev_median(v, DEV_BATCH);
    for (unsigned k = 0; k < DEV_BATCH; ++k) d[k] = abs(e->span[k] - med);
    int32_t mad = dev_median(d, DEV_BATCH);
    if (mad * 100 > med * DEV_MAD_PCT) { e->batches = 0; ++e->rejected; return -1; }
    e->medians[e->batches++] = med;
    if (e->batches < DEV_BATCHES) return -1;
    int32_t lo = e->medians[0], hi = e->medians[0];
    for (unsigned k = 1; k < DEV_BATCHES; ++k) {
        if (e->medians[k] < lo) lo = e->medians[k];
        if (e->medians[k] > hi) hi = e->medians[k];
    }
    if ((hi - lo) * 100 > lo * DEV_AGREE_PCT) { e->batches = 0; ++e->rejected; return -1; }
    /* The highest batch: errs toward the wide (safe) profile. */
    e->result_x100 = (int)(hi * 100 / DEV_SYNC_KHZ);
    return e->result_x100;
}

/* Profile for a measured deviation; anything unknown, or a profile whose
 * filter class was not calibrated, keeps DEFAULT. */
static inline int profile_class(int dev_x100, bool narrow_filter_known, bool wide_filter_known)
{
    if (dev_x100 <= 0) return PROFILE_DEFAULT;
    if (dev_x100 >= DEV_WIDE_MIN) return wide_filter_known ? PROFILE_WIDE : PROFILE_DEFAULT;
    if (!narrow_filter_known) return PROFILE_DEFAULT;
    return dev_x100 <= DEV_KP050_MAX ? PROFILE_KP050 : dev_x100 <= DEV_KP060_MAX ? PROFILE_KP060 : PROFILE_DEFAULT;
}

/* kp (x100) a profile starts from, and the fixed kp with the ladder OFF. */
static inline int profile_base_kp_x100(int p)
{
    return p == PROFILE_KP050 ? 50 : p == PROFILE_KP060 ? 60 : 85;
}

/* Its kp_tables.h level (0 = kp 0.85, 0.05 per level). */
static inline int profile_base_level_for(int p) { return (85 - profile_base_kp_x100(p)) / 5; }

/* ---- Continuous VTX matching: measure and report only -------------------
 * After startup, a passive measurement cycle starts every 30 s on live video
 * (no retune, filter or tracker change). A cycle uses the estimator above
 * and only windows that pass the reliability gates (burst-confirmed sync,
 * AFC lock, C/N >= DEV_MONITOR_MIN_CN_X10, no clipping, held gain): at a low
 * C/N the porch estimate is pulled toward the receiver centre
 * (research/afc_bias.py), which would read the deviation too narrow and
 * point at a narrow profile that hurts. A cycle that cannot decide within
 * DEV_MONITOR_TIMEOUT_US starts over, so batches never mix situations far
 * apart in time. The result updates the session deviation (kp ladder class)
 * and is kept as PENDING for the next boot or CALIBRATE; the active profile
 * never changes in flight (operator decision 2026-10-12). */
#define DEV_MONITOR_PERIOD_US    30000000LL
#define DEV_MONITOR_TIMEOUT_US   120000000LL
#define DEV_MONITOR_MIN_CN_X10   80          /* 8 dB: 2 dB above the AFC retune gate */
#define DEV_STORE_DELTA_X100     3           /* NVS: ignore smaller changes ... */
#define DEV_STORE_GAP_US         300000000LL /* ... and rewrite at most every 5 min */

typedef struct {
    bool collecting;          /* a cycle is collecting windows */
    int64_t cycle_start_us, next_start_us, last_us, stored_us;
    unsigned decisions, timeouts, agree_streak, mismatch_streak;
    int last_x100, want;      /* last decided deviation and its profile, -1 none */
} dev_monitor_t;

static inline void dev_monitor_reset(dev_monitor_t *m, int64_t now_us)
{
    memset(m, 0, sizeof(*m));
    m->collecting = true;
    m->cycle_start_us = now_us;
    m->next_start_us = now_us;
    m->stored_us = now_us - DEV_STORE_GAP_US;
    m->last_x100 = -1;
    m->want = -1;
}

/* True when the caller must reset its estimator: a new cycle is due, or the
 * running one timed out. */
static inline bool dev_monitor_tick(dev_monitor_t *m, int64_t now_us)
{
    if (!m->collecting) {
        if (now_us < m->next_start_us) return false;
        m->collecting = true;
        m->cycle_start_us = now_us;
        return true;
    }
    if (now_us - m->cycle_start_us < DEV_MONITOR_TIMEOUT_US) return false;
    ++m->timeouts;
    m->cycle_start_us = now_us;
    return true;
}

/* A cycle decided `dev_x100`, which asks for profile `want` while `active`
 * runs. The next cycle starts 30 s after this one started. */
static inline void dev_monitor_decided(dev_monitor_t *m, int dev_x100, int want, int active, int64_t now_us)
{
    ++m->decisions;
    m->agree_streak = want == m->want ? m->agree_streak + 1u : 1u;
    m->mismatch_streak = want != active ? m->mismatch_streak + 1u : 0u;
    m->want = want;
    m->last_x100 = dev_x100;
    m->last_us = now_us;
    m->collecting = false;
    m->next_start_us = m->cycle_start_us + DEV_MONITOR_PERIOD_US;
}

/* NVS wear: store when the profile class changes, otherwise only a change of
 * at least DEV_STORE_DELTA_X100 and not more often than DEV_STORE_GAP_US. */
static inline bool dev_monitor_should_store(int stored_x100, int dev_x100, bool class_changed, int64_t stored_us,
                                            int64_t now_us)
{
    if (dev_x100 <= 0 || dev_x100 == stored_x100) return false;
    if (class_changed) return true;
    return abs(dev_x100 - stored_x100) >= DEV_STORE_DELTA_X100 && now_us - stored_us >= DEV_STORE_GAP_US;
}

/* ---- Live profile switch (asymmetric) -----------------------------------
 * Optional, NVS "prof_live" (default on, console A): a reliable continuous
 * result may change the active profile in flight through the verified
 * pipeline restart plus a filter relock (a few ms, like an AFC step).
 * A narrow filter on a wide VTX clips its FM spectrum and costs the most,
 * so widening follows one reliable decision; narrowing (a gain of ~1.7-3.2
 * dB range limit, simulation) needs DEV_NARROW_AGREE consecutive agreeing
 * decisions, i.e. at least one 30 s cycle more. Report-only mode keeps the
 * old behaviour (PENDING for the next boot). */
#define DEV_NARROW_AGREE 2u

/* Filter width order of the profiles: KP050 < KP060 < DEFAULT < WIDE. */
static inline int profile_width_rank(int p)
{
    return p == PROFILE_KP050 ? 0 : p == PROFILE_KP060 ? 1 : p == PROFILE_WIDE ? 3 : 2;
}

static inline bool profile_live_switch_wanted(bool enabled, int want, int active, unsigned agree_streak)
{
    if (!enabled || want < 0 || want >= PROFILES || want == active) return false;
    if (profile_width_rank(want) > profile_width_rank(active)) return true;   /* widen at once */
    return agree_streak >= DEV_NARROW_AGREE;
}
