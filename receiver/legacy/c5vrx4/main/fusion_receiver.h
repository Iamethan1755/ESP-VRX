#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "demod_quality.h"

/*
 * C5VRX IQ Fusion Engine (shadow/supervisory layer).
 * Consumes only already-finished control snapshots; never paces live IQ.
 */
typedef enum {
    FUSION_CONTEXT_NO_CARRIER = 0,
    FUSION_CONTEXT_WEAK,
    FUSION_CONTEXT_CLEAN,
    FUSION_CONTEXT_BLOCKER,
    FUSION_CONTEXT_OVERLOAD,
    FUSION_CONTEXT_COUNT,
} fusion_context_t;

typedef struct {
    uint8_t phase[5];
    int power[5];
    unsigned history;
    uint32_t transitions, low_confidence, phase_jitter_sum;
    int previous_delta;
    bool have_previous_delta;
    uint32_t lag2_total, lag2_disagree;
    uint32_t lag4_total, lag4_disagree;
    uint32_t slope_total, slope_residual_sum;
    uint32_t consensus_total, consensus_outliers;
    int pll_predictor_delta;
    bool have_pll_predictor;
    uint32_t pll_lite_total, pll_lite_slips, pll_lite_holds;
} fusion_shadow_t;

typedef struct {
    int low_confidence_permille;
    int phase_jitter_x100;
    int lag2_disagreement_permille;
    int lag4_disagreement_permille;
    int slope_residual_x100;
    int consensus_outlier_permille;
    int pll_lite_slip_permille;
    int pll_lite_hold_permille;
    int trajectory_uncertainty_permille;
} fusion_shadow_metrics_t;

typedef struct {
    int p_median, q_phase, clip_permille, origin_permille;
    int winding_permille, strong_winding_permille;
    int iq_skew_permille, iq_cross_permille, sync_quality;
    fusion_shadow_metrics_t shadow;
    fusion_context_t context;
    int confidence;
    int quality;
    int catastrophic_risk; /* 0..1000: phase-slip / clip / near-origin danger */
} fusion_observation_t;

static inline int fusion_abs(int v) { return v < 0 ? -v : v; }
static inline int fusion_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int fusion_median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

static inline int fusion_robust_delta3(int d0, int d1, int d2)
{
    return fusion_median3(d0, d1, d2);
}

static inline void fusion_shadow_reset(fusion_shadow_t *s)
{
    *s = (fusion_shadow_t){0};
}

static inline void fusion_shadow_push(fusion_shadow_t *s, uint8_t phase, int power)
{
    int current_delta = 0;
    bool have_current_delta = false;
    bool current_low_confidence = false;

    if (s->history) {
        current_delta = demod_phase5_signed_delta(s->phase[s->history - 1u], phase);
        have_current_delta = true;
        current_low_confidence = power < 8 || s->power[s->history - 1u] < 8;
        ++s->transitions;
        if (current_low_confidence) ++s->low_confidence;
        if (s->have_previous_delta)
            s->phase_jitter_sum += (uint32_t)fusion_abs(current_delta - s->previous_delta);
        s->previous_delta = current_delta;
        s->have_previous_delta = true;
    }

    if (s->history < 5u) {
        s->phase[s->history] = phase;
        s->power[s->history] = power;
        ++s->history;
    } else {
        for (unsigned i = 0; i < 4u; ++i) {
            s->phase[i] = s->phase[i + 1u];
            s->power[i] = s->power[i + 1u];
        }
        s->phase[4] = phase;
        s->power[4] = power;
    }

    if (s->history >= 3u) {
        unsigned n = s->history;
        int d0 = demod_phase5_signed_delta(s->phase[n - 3u], s->phase[n - 2u]);
        int d1 = demod_phase5_signed_delta(s->phase[n - 2u], s->phase[n - 1u]);
        int endpoint = demod_phase5_signed_delta(s->phase[n - 3u], s->phase[n - 1u]);
        int pair_sum = d0 + d1;
        ++s->lag2_total;
        if (pair_sum != endpoint) ++s->lag2_disagree;

        if (s->have_pll_predictor) {
            ++s->pll_lite_total;
            bool low_pair = s->power[n - 3u] < 8 ||
                            s->power[n - 2u] < 8 ||
                            s->power[n - 1u] < 8;
            int expected_pair = s->pll_predictor_delta * 2;
            int innovation = fusion_abs(pair_sum - expected_pair);
            /* PLL-lite is observation-only: it marks a likely click/slip when
             * endpoint winding disagreement coincides with low envelope and a
             * large innovation versus the last clean local slope. */
            if (low_pair && pair_sum != endpoint && innovation >= 8)
                ++s->pll_lite_slips;
            if (low_pair && fusion_abs(d1 - s->pll_predictor_delta) >= 8)
                ++s->pll_lite_holds;
        }
    }

    if (s->history >= 4u) {
        unsigned n = s->history;
        int d0 = demod_phase5_signed_delta(s->phase[n - 4u], s->phase[n - 3u]);
        int d1 = demod_phase5_signed_delta(s->phase[n - 3u], s->phase[n - 2u]);
        int d2 = demod_phase5_signed_delta(s->phase[n - 2u], s->phase[n - 1u]);
        int robust = fusion_robust_delta3(d0, d1, d2);
        s->slope_residual_sum += (uint32_t)(fusion_abs(d0 - robust) + fusion_abs(d1 - robust) + fusion_abs(d2 - robust));
        ++s->slope_total;
        ++s->consensus_total;
        if (fusion_abs(d1 - robust) >= 8 && s->power[n - 3u] >= 8 && s->power[n - 2u] >= 8)
            ++s->consensus_outliers;
    }

    if (s->history >= 5u) {
        int adjacent = 0;
        for (unsigned i = 1u; i < 5u; ++i)
            adjacent += demod_phase5_signed_delta(s->phase[i - 1u], s->phase[i]);
        ++s->lag4_total;
        if (adjacent != demod_phase5_signed_delta(s->phase[0], s->phase[4]))
            ++s->lag4_disagree;
    }

    /* Update the tiny frequency predictor only from trustworthy adjacent
     * samples. During a low-envelope event it coasts instead of learning the
     * click -- the fixed-point analogue of a very small PLL holdover. */
    if (have_current_delta && !current_low_confidence) {
        if (!s->have_pll_predictor) {
            s->pll_predictor_delta = current_delta;
            s->have_pll_predictor = true;
        } else {
            s->pll_predictor_delta = (s->pll_predictor_delta * 3 + current_delta) / 4;
        }
    }
}

static inline fusion_shadow_metrics_t fusion_shadow_finish(const fusion_shadow_t *s)
{
    fusion_shadow_metrics_t m = {0};
    if (s->transitions) {
        m.low_confidence_permille = (int)((s->low_confidence * 1000u) / s->transitions);
        if (s->transitions > 1u)
            m.phase_jitter_x100 = (int)((s->phase_jitter_sum * 100u) / (s->transitions - 1u));
    }
    if (s->lag2_total) m.lag2_disagreement_permille = (int)((s->lag2_disagree * 1000u) / s->lag2_total);
    if (s->lag4_total) m.lag4_disagreement_permille = (int)((s->lag4_disagree * 1000u) / s->lag4_total);
    if (s->slope_total) m.slope_residual_x100 = (int)((s->slope_residual_sum * 100u) / s->slope_total);
    if (s->consensus_total) m.consensus_outlier_permille = (int)((s->consensus_outliers * 1000u) / s->consensus_total);
    if (s->pll_lite_total) {
        m.pll_lite_slip_permille = (int)((s->pll_lite_slips * 1000u) / s->pll_lite_total);
        m.pll_lite_hold_permille = (int)((s->pll_lite_holds * 1000u) / s->pll_lite_total);
    }
    return m;
}

static inline const char *fusion_context_name(fusion_context_t c)
{
    switch (c) {
    case FUSION_CONTEXT_NO_CARRIER: return "NO_CARRIER";
    case FUSION_CONTEXT_WEAK: return "WEAK";
    case FUSION_CONTEXT_CLEAN: return "CLEAN";
    case FUSION_CONTEXT_BLOCKER: return "BLOCKER";
    case FUSION_CONTEXT_OVERLOAD: return "OVERLOAD";
    default: return "UNKNOWN";
    }
}

static inline fusion_context_t fusion_classify(const fusion_observation_t *o)
{
    if (o->clip_permille >= 20 || o->p_median >= 44) return FUSION_CONTEXT_OVERLOAD;
    if (o->q_phase < 18 && o->origin_permille >= 650) return FUSION_CONTEXT_NO_CARRIER;
    if (o->p_median >= 28 && o->q_phase < 38 &&
        (o->winding_permille >= 180 || o->shadow.lag4_disagreement_permille >= 300))
        return FUSION_CONTEXT_BLOCKER;
    if (o->q_phase < 48 || o->p_median < 14 || o->origin_permille >= 300)
        return FUSION_CONTEXT_WEAK;
    return FUSION_CONTEXT_CLEAN;
}

static inline int fusion_catastrophic_risk_score(const fusion_observation_t *o)
{
    /* Keep catastrophic failure separate from average "quality". A large
     * endpoint/trajectory disagreement or near-origin cluster must not be
     * cancelled by a pretty power metric. */
    int risk = 0;
    risk += o->origin_permille / 2;
    risk += o->winding_permille;
    risk += o->strong_winding_permille * 2;
    risk += o->shadow.lag4_disagreement_permille / 2;
    risk += o->shadow.consensus_outlier_permille / 2;
    risk += o->shadow.low_confidence_permille / 2;
    risk += o->shadow.pll_lite_slip_permille;
    risk += o->shadow.pll_lite_hold_permille / 3;
    risk += o->shadow.trajectory_uncertainty_permille / 2;
    risk += o->clip_permille * 4;
    return fusion_clamp(risk, 0, 1000);
}

static inline int fusion_quality_score(const fusion_observation_t *o)
{
    int score = o->q_phase * 9;
    score += (1000 - fusion_clamp(o->shadow.low_confidence_permille, 0, 1000)) / 20;
    score += fusion_clamp(o->sync_quality, 0, 100) / 2;
    score -= o->origin_permille / 3;
    score -= o->clip_permille * 2;
    score -= o->winding_permille / 2;
    score -= o->shadow.lag4_disagreement_permille / 4;
    score -= o->shadow.consensus_outlier_permille / 3;
    score -= o->iq_skew_permille / 8;
    score -= o->iq_cross_permille / 8;
    score -= o->shadow.phase_jitter_x100 / 80;
    score -= o->shadow.slope_residual_x100 / 100;
    score -= o->shadow.pll_lite_slip_permille / 3;
    score -= o->shadow.trajectory_uncertainty_permille / 5;
    score -= fusion_catastrophic_risk_score(o) / 4;
    return fusion_clamp(score, 0, 1000);
}

static inline int fusion_confidence_score(const fusion_observation_t *o)
{
    int confidence = 1000;
    confidence -= o->shadow.low_confidence_permille / 2;
    confidence -= o->winding_permille / 3;
    confidence -= o->shadow.consensus_outlier_permille / 2;
    confidence -= o->shadow.pll_lite_slip_permille / 2;
    confidence -= o->shadow.trajectory_uncertainty_permille / 4;
    confidence -= o->clip_permille * 2;
    return fusion_clamp(confidence, 0, 1000);
}

static inline fusion_observation_t fusion_make_observation(
    int p_median, int q_phase, int clip_permille, int origin_permille,
    int winding_permille, int strong_winding_permille,
    int iq_skew_permille, int iq_cross_permille, int sync_quality,
    fusion_shadow_metrics_t shadow)
{
    fusion_observation_t o = {
        .p_median = p_median, .q_phase = q_phase,
        .clip_permille = clip_permille, .origin_permille = origin_permille,
        .winding_permille = winding_permille,
        .strong_winding_permille = strong_winding_permille,
        .iq_skew_permille = iq_skew_permille,
        .iq_cross_permille = iq_cross_permille,
        .sync_quality = sync_quality, .shadow = shadow,
    };
    o.context = fusion_classify(&o);
    o.catastrophic_risk = fusion_catastrophic_risk_score(&o);
    o.quality = fusion_quality_score(&o);
    o.confidence = fusion_confidence_score(&o);
    return o;
}
