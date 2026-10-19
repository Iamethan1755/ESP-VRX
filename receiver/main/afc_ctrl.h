/* C5VRX by Twotoz and the C5VRX contributors (legacy/c5vrx4/main/afc_v2_ctrl.h). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "afc.h"

/*
 * Issue #115 AFC V2 acquisition decision on top of afc2_measure().
 *
 * Only used outside TRACK. It never uses
 * the scene-biased WBFM slope. A correction requires:
 *  - AFC2_CTRL_SAMPLES burst-confirmed window estimates since the last reset,
 *    all with the same sync polarity and video standard;
 *  - a stable estimate: median absolute deviation <= AFC2_CTRL_MAX_MAD_KHZ;
 *  - |median| > AFC2_CTRL_DEADBAND_KHZ.
 * The step is clamped to +/-AFC2_CTRL_MAX_STEP_KHZ and at most
 * AFC2_CTRL_MAX_CORRECTIONS are made per acquisition context; every write
 * changes the frequency offset, which resets the caller's context so only
 * fresh measurements count afterwards.
 *
 * Reference model: the burst-free porch (blanking) level is treated as the
 * carrier centre. #115 section 9 requires confirming this for the VTX on
 * hardware; AFC2_REF_SYNC_MID offers the sync/porch midpoint instead.
 * Sign: a positive estimate means the signal sits above the receiver centre,
 * corrected by raising the offset (same convention as the legacy estimator).
 */

#define AFC2_CTRL_SAMPLES          16u
/* A window without evidence (failed copy, non-stationary envelope, no
 * burst) is missing, not contradicting: the estimate window is kept up to
 * this many such windows in a row (1 s). Clearing it on every one needed 16
 * clean windows in a row; with 30-44 % failed copies on the board that is
 * ~1e-4, and the AFC never locked, so the deviation was never measured
 * (board walks 2026-10-13, docs/FINDINGS.md). */
#define AFC2_CTRL_MAX_MISSES       20u
#define AFC2_CTRL_MAX_MAD_KHZ      80
#define AFC2_CTRL_DEADBAND_KHZ     50
#define AFC2_CTRL_MAX_STEP_KHZ     250
#define AFC2_CTRL_MAX_CORRECTIONS  4u
/* A spent budget re-arms after this many windows (3 s at 50 ms) while the
 * AFC is still not locked. Before, the budget only came back after a lost
 * lock: a VTX more than 4 x 250 kHz off frequency never got centred, never
 * locked, and kept a 0.2-0.5 MHz residual for the whole session
 * (test/test_afc.c), which costs the picture at weak signal
 * (research/situations.py: -0.75 / -1.0 MHz residual at 4 dB C/N). */
#define AFC2_CTRL_REARM_WINDOWS    60u
/* Offset sign check. The sign of "raise the offset to lower the estimate"
 * was never confirmed on hardware (C5VRX-4 INTEGRATION.md: "VTX
 * sign/reference calibration pending"); with the opposite sign every step
 * walked the carrier away, up to the offset clamp. After a step of at least
 * this size, the next stable estimate must have moved toward the target by
 * at least half the step; if it moved away by that much the sign flips.
 * Smaller steps are not judged (estimate noise ~20-40 kHz). */
#define AFC2_CTRL_SIGN_CHECK_KHZ   150
#define AFC2_NATIVE_LOST_WINDOWS   4u

/* Acquisition lock must be sticky. A changed CFO while otherwise receiving
 * valid video is not permission to retune mid-flight. Invalid windows, rather
 * than the retired firmware gain controller, re-arm acquisition. */
static inline bool afc2_native_lock(bool locked, bool valid, bool centred,
                                     unsigned fresh, uint8_t *lost)
{
    if (valid) *lost = 0;
    else if (*lost < AFC2_NATIVE_LOST_WINDOWS) ++*lost;
    if (locked) return *lost < AFC2_NATIVE_LOST_WINDOWS;
    return valid && centred && fresh >= AFC2_CTRL_SAMPLES;
}

enum { AFC2_SIGN_UNVERIFIED, AFC2_SIGN_CONFIRMED, AFC2_SIGN_FLIPPED };

typedef enum {
    AFC2_REF_PORCH = 0,
    AFC2_REF_SYNC_MID,
} afc2_ref_t;

typedef struct {
    uint32_t context;
    int32_t est[AFC2_CTRL_SAMPLES];
    uint8_t n;
    int8_t polarity;
    uint8_t standard;
    uint8_t corrections;
    uint16_t since_correction;   /* observed windows since the last correction */
    uint8_t misses;              /* windows without evidence in a row (AFC2_CTRL_MAX_MISSES) */
    int8_t sign;                 /* +1 / -1 offset direction; 0 = +1 (unverified) */
    uint8_t sign_state;          /* AFC2_SIGN_* */
    bool sign_check;             /* a judged step awaits the next stable estimate */
    int32_t check_med, check_step;
    afc2_ref_t ref;
    /* Design porch frequency of the active demodulator (kHz relative to the
     * tuned frequency). Shared-word range trackers (RANGE32/PAIR/EDGE) are
     * designed for blanking at -436 kHz; 0 keeps the original target. */
    int32_t target_khz;
} afc2_ctrl_t;

static inline void afc2_ctrl_reset(afc2_ctrl_t *c, uint32_t context, bool new_acquisition)
{
    c->context = context;
    c->n = 0;
    c->misses = 0;
    c->polarity = 0;
    c->standard = 0;
    /* Our own retune keeps the pending sign check; anything else (channel,
     * PHY restore, lost lock) makes the judged estimate meaningless. */
    if (new_acquisition) { c->corrections = 0; c->sign_check = false; }
}

/* Returns true when the context changed (samples discarded). A context change
 * caused by our own correction keeps the correction count. */
static inline bool afc2_ctrl_sync(afc2_ctrl_t *c, uint32_t context, bool own_write)
{
    if (c->context == context) return false;
    afc2_ctrl_reset(c, context, !own_write);
    return true;
}

/* Loss of evidence must discard the whole consecutive estimate window.
 * Keep the per-acquisition correction budget; only explicit acquisition reset
 * replenishes it. No stale decision after settling, bad IQ, or missing burst. */
static inline void afc2_ctrl_invalidate(afc2_ctrl_t *c)
{
    c->n = 0;
    c->polarity = 0;
    c->standard = 0;
    c->misses = 0;
}

/* One window without evidence: keep the estimates unless too many in a row. */
static inline void afc2_ctrl_miss(afc2_ctrl_t *c)
{
    if (++c->misses > AFC2_CTRL_MAX_MISSES) afc2_ctrl_invalidate(c);
}

static inline void afc2_ctrl_observe(afc2_ctrl_t *c, const afc2_result_t *r)
{
    if (c->since_correction < 0xffffu) ++c->since_correction;
    if (!r || !r->lines || !r->standard || !r->polarity || !r->sync_pairs || !r->porch_pairs) {
        afc2_ctrl_miss(c);
        return;
    }
    c->misses = 0;
    if (c->n && (r->polarity != c->polarity || r->standard != c->standard)) c->n = 0;
    c->polarity = r->polarity;
    c->standard = r->standard;
    int32_t v = (c->ref == AFC2_REF_SYNC_MID ? (r->sync_khz + r->porch_khz) / 2 : r->porch_khz) - c->target_khz;
    if (c->n < AFC2_CTRL_SAMPLES) {
        c->est[c->n++] = v;
    } else {
        for (unsigned k = 1; k < AFC2_CTRL_SAMPLES; ++k) c->est[k - 1] = c->est[k];
        c->est[AFC2_CTRL_SAMPLES - 1] = v;
    }
}

static inline int32_t afc2_ctrl_median(const int32_t *v, unsigned n)
{
    int32_t s[AFC2_CTRL_SAMPLES];
    for (unsigned k = 0; k < n; ++k) s[k] = v[k];
    for (unsigned a = 1; a < n; ++a)
        for (unsigned b = a; b > 0 && s[b - 1] > s[b]; --b) {
            int32_t t = s[b]; s[b] = s[b - 1]; s[b - 1] = t;
        }
    return n ? (n & 1u ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : 0;
}

/* The same full-window stability test gates acquisition lock and writes.
 * A single centred latest estimate cannot lock an unstable history. */
static inline bool afc2_ctrl_stable(const afc2_ctrl_t *c, int32_t *median)
{
    if (c->n < AFC2_CTRL_SAMPLES) return false;
    int32_t med = afc2_ctrl_median(c->est, c->n);
    int32_t dev[AFC2_CTRL_SAMPLES];
    for (unsigned k = 0; k < c->n; ++k)
        dev[k] = c->est[k] > med ? c->est[k] - med : med - c->est[k];
    if (afc2_ctrl_median(dev, c->n) > AFC2_CTRL_MAX_MAD_KHZ) return false;
    *median = med;
    return true;
}

static inline bool afc2_ctrl_can_lock(const afc2_ctrl_t *c, bool auto_afc)
{
    int32_t med;
    return afc2_ctrl_stable(c, &med) && (!auto_afc ||
        (med >= -AFC2_CTRL_DEADBAND_KHZ && med <= AFC2_CTRL_DEADBAND_KHZ));
}

/* Decide one acquisition step. `eligible` carries AUTO/not TRACK/settle. */
static inline bool afc2_ctrl_decide(afc2_ctrl_t *c, bool eligible, int32_t *step_khz)
{
    int32_t med;
    if (c->corrections >= AFC2_CTRL_MAX_CORRECTIONS && c->since_correction >= AFC2_CTRL_REARM_WINDOWS)
        c->corrections = 0;     /* still not centred: another bounded burst */
    if (!afc2_ctrl_stable(c, &med)) return false;
    if (c->sign_check) {
        /* Did the last step move the estimate toward the target? */
        int32_t delta = med - c->check_med, half = c->check_step / 2;
        if ((c->check_step > 0 && delta <= -half) || (c->check_step < 0 && delta >= -half))
            c->sign_state = AFC2_SIGN_CONFIRMED;
        else if ((c->check_step > 0 && delta >= half) || (c->check_step < 0 && delta <= half)) {
            c->sign = c->sign < 0 ? 1 : -1;
            c->sign_state = AFC2_SIGN_FLIPPED;
            c->corrections = 0;    /* the wrong-way step is undone from a fresh budget */
        }
        c->sign_check = false;
    }
    if (!eligible || c->corrections >= AFC2_CTRL_MAX_CORRECTIONS) return false;
    if (med <= AFC2_CTRL_DEADBAND_KHZ && med >= -AFC2_CTRL_DEADBAND_KHZ) return false;
    int32_t step = med > AFC2_CTRL_MAX_STEP_KHZ ? AFC2_CTRL_MAX_STEP_KHZ :
                   med < -AFC2_CTRL_MAX_STEP_KHZ ? -AFC2_CTRL_MAX_STEP_KHZ : med;
    c->sign_check = step >= AFC2_CTRL_SIGN_CHECK_KHZ || step <= -AFC2_CTRL_SIGN_CHECK_KHZ;
    c->check_med = med;
    c->check_step = step;
    *step_khz = c->sign < 0 ? -step : step;
    ++c->corrections;
    c->since_correction = 0;
    c->n = 0;
    return true;
}
