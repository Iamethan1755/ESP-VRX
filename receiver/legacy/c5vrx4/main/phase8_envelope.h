#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Issue #119: Phase8 Q4 envelope / origin-collapse oracle.
 *
 * Read-only statistics over completed MODEM_DIAG Q4/I4 bytes. Nothing here
 * paces the 40 MS/s path, writes PHY state or makes a gain decision.
 *
 * Byte layout matches direct_gain_v3.c and tools/gen_phase8_gain_lut.py:
 * signed I in bits 7..4, signed Q in bits 3..0, and cell n represents the
 * half-integer position n+0.5. The four cells touching the origin
 * (0x00, 0x0F, 0xF0, 0xFF) are the "central" cells: a tiny vector movement
 * among them becomes a ~90 or ~180 degree Phase8 step.
 *
 * Power units are identical to DG3_OBS so P50/P90/P95 are comparable:
 * power = (ci^2 + cq^2 + 2) / 4 with ci = 2i+1, cq = 2q+1.
 */

#define P8ENV_RADIUS_BINS   11u  /* floor(radius in cells): 0..10 */
#define P8ENV_DELTA_BINS     9u  /* |delta| / 16 Phase8 codes: 0..8 */
#define P8ENV_POWER_BINS   114u  /* power 0..113 */
#define P8ENV_HARD_DELTA    60   /* >= ~84 deg: every central-cell move (63..65 codes) */
#define P8ENV_COHERENT_DELTA 32  /* <= 45 degrees, same rule as DG3 */
#define P8ENV_COHERENT_POWER  8u

typedef enum {
    P8ENV_CLASS_EMPTY = 0,
    P8ENV_CLASS_COLLAPSE,  /* origin occupancy too high: Phase8 sees noise */
    P8ENV_CLASS_LOW,       /* below the annulus, not yet collapsed */
    P8ENV_CLASS_ANNULUS,   /* candidate sweet annulus */
    P8ENV_CLASS_HOT,       /* above the annulus, not yet clipping */
    P8ENV_CLASS_CLIP,      /* rail occupancy / P95 overload */
} p8env_class_t;

typedef struct {
    uint32_t samples;
    uint32_t pairs;
    uint32_t central;          /* samples in the four origin-touching cells */
    uint32_t origin;           /* power <= 4 (same definition as DG3) */
    uint32_t clip;             /* any rail cell (-8 or +7) */
    uint32_t coherent;         /* DG3 coherence numerator */
    uint32_t hard;             /* |delta| >= P8ENV_HARD_DELTA (~84 degrees) */
    uint32_t central_pairs;    /* pairs with at least one central endpoint */
    uint32_t hard_central;     /* hard pairs with a central endpoint */
    uint32_t hard_outer;       /* hard pairs with both endpoints power >= 8 */
    uint32_t outer_pairs;      /* pairs with both endpoints power >= 8 */
    uint32_t power_hist[P8ENV_POWER_BINS];
    uint32_t radius_hist[P8ENV_RADIUS_BINS];
    uint32_t delta_hist[P8ENV_DELTA_BINS];
    bool has_prior;
    uint8_t prior_raw;
} p8env_accum_t;

typedef struct {
    uint8_t p50, p90, p95;
    uint16_t central_pm, origin_pm, clip_pm;
    uint16_t coherence_pm;
    uint16_t hard_pm;           /* hard pairs / all pairs */
    uint16_t hard_given_central_pm;
    uint16_t hard_given_outer_pm;
    uint16_t central_hard_share_pm; /* share of the hard tail with a central endpoint */
    uint16_t radius_pm[P8ENV_RADIUS_BINS];
    uint16_t delta_pm[P8ENV_DELTA_BINS];
    p8env_class_t cls;
} p8env_summary_t;

/* Provisional annulus thresholds, deliberately the DG3 "healthy" window.
 * Phase 4 of issue #119 must replace them with hardware-characterized values;
 * tools/analyze_p8env.py reports the empirical window from real sweeps. */
typedef struct {
    uint8_t p50_min, p50_max, p95_max;
    uint16_t origin_collapse_pm, clip_pm;
} p8env_thresholds_t;

static const p8env_thresholds_t P8ENV_PROVISIONAL = {
    .p50_min = 13u, .p50_max = 32u, .p95_max = 65u,
    .origin_collapse_pm = 250u, .clip_pm = 20u,
};

static inline int p8env_i(uint8_t raw) { return (int)(int8_t)(raw & 0xF0u) >> 4; }
static inline int p8env_q(uint8_t raw) { return (int)(int8_t)((raw & 0x0Fu) << 4) >> 4; }

static inline unsigned p8env_r2(uint8_t raw)
{
    int ci = 2 * p8env_i(raw) + 1, cq = 2 * p8env_q(raw) + 1;
    return (unsigned)(ci * ci + cq * cq);
}

static inline unsigned p8env_power(uint8_t raw) { return (p8env_r2(raw) + 2u) / 4u; }

static inline bool p8env_is_central(uint8_t raw)
{
    return p8env_r2(raw) == 2u;
}

static inline bool p8env_is_clip(uint8_t raw)
{
    int i = p8env_i(raw), q = p8env_q(raw);
    return i == -8 || i == 7 || q == -8 || q == 7;
}

/* floor(radius) in cells = floor(sqrt(r2 / 4)). */
static inline unsigned p8env_radius_bin(uint8_t raw)
{
    unsigned x = p8env_r2(raw) / 4u, r = 0;
    while ((r + 1u) * (r + 1u) <= x) ++r;
    return r < P8ENV_RADIUS_BINS ? r : P8ENV_RADIUS_BINS - 1u;
}

static inline int p8env_signed_delta(uint8_t previous_phase, uint8_t phase)
{
    return (((int)phase - (int)previous_phase + 128) & 255) - 128;
}

static inline void p8env_reset(p8env_accum_t *a)
{
    *a = (p8env_accum_t){0};
}

/* A new capture block breaks sample adjacency; never pair across blocks. */
static inline void p8env_break(p8env_accum_t *a) { a->has_prior = false; }

static inline void p8env_add(p8env_accum_t *a, const uint8_t *samples,
                             size_t bytes, const uint8_t phase8_lut[256])
{
    if (!a || !samples || !phase8_lut) return;
    for (size_t n = 0; n < bytes; ++n) {
        uint8_t raw = samples[n];
        unsigned power = p8env_power(raw);
        bool central = p8env_is_central(raw);
        ++a->samples;
        ++a->power_hist[power < P8ENV_POWER_BINS ? power : P8ENV_POWER_BINS - 1u];
        ++a->radius_hist[p8env_radius_bin(raw)];
        a->central += central;
        a->origin += power <= 4u;
        a->clip += p8env_is_clip(raw);
        if (a->has_prior) {
            uint8_t prior = a->prior_raw;
            int delta = p8env_signed_delta(phase8_lut[prior], phase8_lut[raw]);
            unsigned magnitude = (unsigned)(delta < 0 ? -delta : delta);
            bool hard = magnitude >= (unsigned)P8ENV_HARD_DELTA;
            bool any_central = central || p8env_is_central(prior);
            bool outer = power >= P8ENV_COHERENT_POWER &&
                         p8env_power(prior) >= P8ENV_COHERENT_POWER;
            ++a->pairs;
            ++a->delta_hist[magnitude / 16u < P8ENV_DELTA_BINS ?
                            magnitude / 16u : P8ENV_DELTA_BINS - 1u];
            /* DG3 coherence: current endpoint strong and step <= 45 degrees. */
            if (power >= P8ENV_COHERENT_POWER &&
                magnitude <= (unsigned)P8ENV_COHERENT_DELTA) ++a->coherent;
            a->hard += hard;
            a->central_pairs += any_central;
            a->hard_central += hard && any_central;
            a->outer_pairs += outer;
            a->hard_outer += hard && outer;
        }
        a->prior_raw = raw;
        a->has_prior = true;
    }
}

static inline uint16_t p8env_pm(uint32_t num, uint32_t den)
{
    return den ? (uint16_t)(((uint64_t)num * 1000u + den / 2u) / den) : 0u;
}

static inline p8env_class_t p8env_classify(const p8env_summary_t *s,
                                           uint32_t samples,
                                           const p8env_thresholds_t *t)
{
    if (!samples) return P8ENV_CLASS_EMPTY;
    if (s->clip_pm >= t->clip_pm || s->p95 > t->p95_max) return P8ENV_CLASS_CLIP;
    if (s->origin_pm > t->origin_collapse_pm) return P8ENV_CLASS_COLLAPSE;
    if (s->p50 < t->p50_min) return P8ENV_CLASS_LOW;
    if (s->p50 > t->p50_max) return P8ENV_CLASS_HOT;
    return P8ENV_CLASS_ANNULUS;
}

static inline p8env_summary_t p8env_summarize(const p8env_accum_t *a,
                                              const p8env_thresholds_t *t)
{
    p8env_summary_t s = {0};
    if (!a || !a->samples) return s;
    uint32_t rank50 = (a->samples + 1u) / 2u;
    uint32_t rank90 = (uint32_t)(((uint64_t)a->samples * 90u + 99u) / 100u);
    uint32_t rank95 = (uint32_t)(((uint64_t)a->samples * 95u + 99u) / 100u);
    uint32_t cumulative = 0;
    bool got50 = false, got90 = false;
    for (unsigned p = 0; p < P8ENV_POWER_BINS; ++p) {
        cumulative += a->power_hist[p];
        if (!got50 && cumulative >= rank50) { s.p50 = (uint8_t)p; got50 = true; }
        if (!got90 && cumulative >= rank90) { s.p90 = (uint8_t)p; got90 = true; }
        if (cumulative >= rank95) { s.p95 = (uint8_t)p; break; }
    }
    s.central_pm = p8env_pm(a->central, a->samples);
    s.origin_pm = p8env_pm(a->origin, a->samples);
    s.clip_pm = p8env_pm(a->clip, a->samples);
    s.coherence_pm = p8env_pm(a->coherent, a->pairs);
    s.hard_pm = p8env_pm(a->hard, a->pairs);
    s.hard_given_central_pm = p8env_pm(a->hard_central, a->central_pairs);
    s.hard_given_outer_pm = p8env_pm(a->hard_outer, a->outer_pairs);
    s.central_hard_share_pm = p8env_pm(a->hard_central, a->hard);
    for (unsigned b = 0; b < P8ENV_RADIUS_BINS; ++b)
        s.radius_pm[b] = p8env_pm(a->radius_hist[b], a->samples);
    for (unsigned b = 0; b < P8ENV_DELTA_BINS; ++b)
        s.delta_pm[b] = p8env_pm(a->delta_hist[b], a->pairs);
    s.cls = p8env_classify(&s, a->samples, t ? t : &P8ENV_PROVISIONAL);
    return s;
}

static inline const char *p8env_class_name(p8env_class_t cls)
{
    switch (cls) {
    case P8ENV_CLASS_COLLAPSE: return "COLLAPSE";
    case P8ENV_CLASS_LOW:      return "LOW";
    case P8ENV_CLASS_ANNULUS:  return "ANNULUS";
    case P8ENV_CLASS_HOT:      return "HOT";
    case P8ENV_CLASS_CLIP:     return "CLIP";
    case P8ENV_CLASS_EMPTY:
    default:                   return "EMPTY";
    }
}
