/* C5VRX by Twotoz and contributors. No RF gain or sample-paced CPU work. */
#include "cvbs_level.h"
#include "cvbs_tables.h"
#include <string.h>
static int magnitude(int v) { return v < 0 ? -v : v; }
static int middle(int a, int b, int c)
{ return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b); }
static int delta(unsigned i)
{
    int d = (int)(i & 63) * 4 + 2 - 128;
    if ((i >> 6) == 1 && d < 0) d += 256;
    if ((i >> 6) == 2 && d >= 0) d -= 256;
    return d;
}
static uint8_t nearest(int64_t uv)
{
    unsigned best = 0;
    int64_t distance = INT64_MAX;
    for (unsigned c = 0; c < 64; ++c) {
        int64_t e = (int64_t)c5v4_dac_uv[c] - uv;
        if (e < 0) e = -e;
        if (e < distance) { distance = e; best = c; }
    }
    return (uint8_t)best;
}
void c5v4_level_init(c5v4_level_t *s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->codes, c5v4_dac_codes, sizeof(s->codes));
}
void c5v4_level_seed(c5v4_level_t *s, const uint8_t codes[256])
{
    for (unsigned i = 0; i < 256; ++i) s->codes[i] = codes[i] & 63u;
}
uint16_t c5v4_level_word(uint16_t original, unsigned code)
{
    return (uint16_t)((original & ~63u) | (code & 63u));
}
unsigned c5v4_level_period(c5v4_level_t *s, uint32_t context, uint64_t now)
{
    if (!s->context_valid || s->context != context) {
        s->context = context; s->context_valid = true; s->good = 0;
        s->recovery_until_us = now + C5V4_LEVEL_RECOVERY_US;
        s->settled = false;
    }
    return now < s->recovery_until_us ? C5V4_LEVEL_FAST_US : C5V4_LEVEL_PERIOD_US;
}
uint8_t c5v4_level_slew(uint8_t current, uint8_t target, const uint32_t volts[64])
{
    return c5v4_level_slew_band(current, target, volts, C5V4_LEVEL_DEADBAND_UV);
}
uint8_t c5v4_level_slew_band(uint8_t current, uint8_t target, const uint32_t volts[64],
                             uint32_t deadband_uv)
{
    uint8_t best = current;
    int64_t goal = volts[target], from = volts[current];
    int64_t distance = goal - from;
    if (distance < 0) distance = -distance;
    if (distance <= (int64_t)deadband_uv) return current;
    for (unsigned c = 0; c < 64; ++c) {
        int64_t step = (int64_t)volts[c] - from;
        if ((goal > from && step < 0) || (goal < from && step > 0)) continue;
        if (step < 0) step = -step;
        if (step > C5V4_LEVEL_STEP_UV) continue;
        int64_t error = (int64_t)volts[c] - goal;
        if (error < 0) error = -error;
        if (error < distance) { distance = error; best = (uint8_t)c; }
    }
    return best;
}
bool c5v4_level_observe(c5v4_level_t *s, const c5v4_cvbs_stats_t *v,
                      bool fresh, uint32_t context, uint64_t now)
{
    unsigned period = c5v4_level_period(s, context, now);
    /* Phase level evidence must agree across three fresh consecutive windows.
     * Reject origin collapse, folding/overload and distorted pulse plateaus.
     * 12 bins is the existing observer floor; never chase arbitrarily small sync.
     * Nominal gain range is about 0.32..3.2x the initial 0.15 V/MHz transfer. */
    bool valid = fresh && v && v->levels_valid && v->repeated && v->pulses >= 2 &&
        v->span_bins >= 12 && v->span_bins <= 120 &&
        v->sync_mad_bins <= 4 && v->blank_mad_bins <= 4 &&
        v->sync_mad_bins * 8 <= v->span_bins && v->blank_mad_bins * 8 <= v->span_bins &&
        v->origin_pm <= 350 && v->ambiguous_pm <= 100 && v->clip_pm <= 200 &&
        v->period_raw >= 2529 && v->period_raw <= 2574;
    if (!valid) { s->good = 0; ++s->refusals; return false; }
    /* Even invalidation/context changes must not make a replay fresh. */
    if (s->evidence_valid && (now <= s->evidence_us ||
        now - s->evidence_us < period / 2)) return false;
    /* Distinct, regularly spaced evidence only: a repeated capture cannot
     * qualify the loop, and a long outage must reacquire before writing. */
    if (s->good && now - s->evidence_us > 100000) s->good = 0;
    unsigned depth = v->period_raw <= 2550 ? 286000u : 300000u;
    if (s->good && s->evidence_depth != depth) s->good = 0;
    s->evidence_depth = depth;
    if (s->good && (magnitude(v->blank_bins - s->blank) > 4 ||
                   magnitude(v->span_bins - s->span) > 4)) {
        s->good = 0;
    }
    if (!s->good) {
        s->recovery_until_us = now + C5V4_LEVEL_RECOVERY_US;
        period = C5V4_LEVEL_FAST_US;
        s->settled = false;
    }
    s->blank = v->blank_bins; s->span = v->span_bins;
    if (!s->good) s->evidence_slot = 0;
    unsigned slot = s->evidence_slot++ % 3;
    s->evidence_blank[slot] = s->blank; s->evidence_span[slot] = s->span;
    s->evidence_us = now; s->evidence_valid = true;
    if (s->good < 3) ++s->good;
    if (s->good < 3 || (s->updates && now - s->last_us < period)) return false;
    if (s->settled && s->updates && now - s->last_us < C5V4_LEVEL_SETTLED_US) return false;
    uint32_t deadband = s->settled ? C5V4_LEVEL_SETTLED_DEADBAND_UV : C5V4_LEVEL_DEADBAND_UV;
    int blank = middle(s->evidence_blank[0], s->evidence_blank[1], s->evidence_blank[2]);
    int span = middle(s->evidence_span[0], s->evidence_span[1], s->evidence_span[2]);
    s->target_depth_mv = depth / 1000u;
    bool changed = false;
    for (unsigned i = 0; i < 256; ++i) {
        /* Fixed black and sync separation, BEFORE clipping to the loaded DAC.
         * Ambiguous trajectories become the black reference, never a rail. */
        int64_t uv = (i >> 6) == 3 ? 10000 + depth :
            10000 + depth + (int64_t)(delta(i) - blank) * depth / span;
        uint8_t target = nearest(uv);
        /* Numeric code distance is not a voltage bound at resistor carries or
         * with measured calibration. Always move electrically toward target. */
        uint8_t current = c5v4_level_slew_band(s->codes[i], target, c5v4_dac_uv, deadband);
        if (s->codes[i] != current) { s->codes[i] = current; changed = true; }
    }
    if (changed) { ++s->updates; s->last_us = now; s->settled = false; }
    else s->settled = true;
    return changed;
}
