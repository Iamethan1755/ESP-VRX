#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "afc_v2.h"

/*
 * Issue #115 AFC V2 acquisition decision on top of afc2_measure().
 *
 * Only used in AUTO AFC (default OFF) and only outside TRACK. It never uses
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
#define AFC2_CTRL_MAX_MAD_KHZ      80
#define AFC2_CTRL_DEADBAND_KHZ     50
#define AFC2_CTRL_MAX_STEP_KHZ     250
#define AFC2_CTRL_MAX_CORRECTIONS  4u
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
    afc2_ref_t ref;
} afc2_ctrl_t;

static inline void afc2_ctrl_reset(afc2_ctrl_t *c, uint32_t context, bool new_acquisition)
{
    c->context = context;
    c->n = 0;
    c->polarity = 0;
    c->standard = 0;
    if (new_acquisition) c->corrections = 0;
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
}

static inline void afc2_ctrl_observe(afc2_ctrl_t *c, const afc2_result_t *r)
{
    if (!r || !r->lines || !r->standard || !r->polarity ||
        r->burst_x10 < AFC2_BURST_MIN_X10 || !r->sync_pairs || !r->porch_pairs) {
        afc2_ctrl_invalidate(c);
        return;
    }
    if (c->n && (r->polarity != c->polarity || r->standard != c->standard)) c->n = 0;
    c->polarity = r->polarity;
    c->standard = r->standard;
    int32_t v = c->ref == AFC2_REF_SYNC_MID ? (r->sync_khz + r->porch_khz) / 2 : r->porch_khz;
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
    if (!eligible || c->corrections >= AFC2_CTRL_MAX_CORRECTIONS ||
        !afc2_ctrl_stable(c, &med)) return false;
    if (med <= AFC2_CTRL_DEADBAND_KHZ && med >= -AFC2_CTRL_DEADBAND_KHZ) return false;
    int32_t step = med > AFC2_CTRL_MAX_STEP_KHZ ? AFC2_CTRL_MAX_STEP_KHZ :
                   med < -AFC2_CTRL_MAX_STEP_KHZ ? -AFC2_CTRL_MAX_STEP_KHZ : med;
    *step_khz = step;
    ++c->corrections;
    c->n = 0;
    return true;
}
