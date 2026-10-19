#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fusion_receiver.h"

/*
 * Multi-timescale shadow observer for Range v2.
 *
 * The fast observer may run much more often than the physical gain actuator.
 * This lets C5VRX tell a persistent weak carrier from a short fade without
 * increasing PHY writes. All math is integer/fixed-cost and observation-only.
 */
typedef struct {
    bool primed;
    uint32_t samples;

    int fast_quality, slow_quality;
    int fast_confidence, slow_confidence;
    int fast_q_phase, slow_q_phase;
    int fast_origin, slow_origin;
    int fast_winding, slow_winding;
    int fast_lag4, slow_lag4;
    int fast_clip, slow_clip;
} fusion_temporal_t;

typedef struct {
    int fade_score;      /* 0..1000: recent deterioration vs longer baseline */
    int recovery_score;  /* 0..1000: recent improvement vs longer baseline */
    int stability;       /* 0..1000: fast and slow state agree */
    int quality_delta;   /* fast - slow */
    int q_phase_delta;   /* fast - slow */
    int origin_delta;    /* fast - slow */
    int winding_delta;   /* fast - slow */
    int lag4_delta;      /* fast - slow */
    uint32_t samples;
} fusion_temporal_metrics_t;

static inline int fusion_temporal_abs(int v) { return v < 0 ? -v : v; }

static inline int fusion_temporal_ema(int current, int sample, unsigned divisor)
{
    return current + (sample - current) / (int)divisor;
}

static inline void fusion_temporal_reset(fusion_temporal_t *t)
{
    *t = (fusion_temporal_t){0};
}

static inline fusion_temporal_metrics_t
fusion_temporal_update(fusion_temporal_t *t, const fusion_observation_t *o)
{
    if (!t->primed) {
        t->primed = true;
        t->fast_quality = t->slow_quality = o->quality;
        t->fast_confidence = t->slow_confidence = o->confidence;
        t->fast_q_phase = t->slow_q_phase = o->q_phase;
        t->fast_origin = t->slow_origin = o->origin_permille;
        t->fast_winding = t->slow_winding = o->winding_permille;
        t->fast_lag4 = t->slow_lag4 = o->shadow.lag4_disagreement_permille;
        t->fast_clip = t->slow_clip = o->clip_permille;
    } else {
        /* At the 6 ms shadow cadence these are roughly a few tens of ms
         * versus a few hundred ms of memory. The 50 ms actuator remains slow. */
        t->fast_quality = fusion_temporal_ema(t->fast_quality, o->quality, 4u);
        t->slow_quality = fusion_temporal_ema(t->slow_quality, o->quality, 32u);
        t->fast_confidence = fusion_temporal_ema(t->fast_confidence, o->confidence, 4u);
        t->slow_confidence = fusion_temporal_ema(t->slow_confidence, o->confidence, 32u);
        t->fast_q_phase = fusion_temporal_ema(t->fast_q_phase, o->q_phase, 4u);
        t->slow_q_phase = fusion_temporal_ema(t->slow_q_phase, o->q_phase, 32u);
        t->fast_origin = fusion_temporal_ema(t->fast_origin, o->origin_permille, 4u);
        t->slow_origin = fusion_temporal_ema(t->slow_origin, o->origin_permille, 32u);
        t->fast_winding = fusion_temporal_ema(t->fast_winding, o->winding_permille, 4u);
        t->slow_winding = fusion_temporal_ema(t->slow_winding, o->winding_permille, 32u);
        t->fast_lag4 = fusion_temporal_ema(t->fast_lag4,
                                           o->shadow.lag4_disagreement_permille, 4u);
        t->slow_lag4 = fusion_temporal_ema(t->slow_lag4,
                                           o->shadow.lag4_disagreement_permille, 32u);
        t->fast_clip = fusion_temporal_ema(t->fast_clip, o->clip_permille, 4u);
        t->slow_clip = fusion_temporal_ema(t->slow_clip, o->clip_permille, 32u);
    }
    if (t->samples != UINT32_MAX) ++t->samples;

    fusion_temporal_metrics_t m = {
        .quality_delta = t->fast_quality - t->slow_quality,
        .q_phase_delta = t->fast_q_phase - t->slow_q_phase,
        .origin_delta = t->fast_origin - t->slow_origin,
        .winding_delta = t->fast_winding - t->slow_winding,
        .lag4_delta = t->fast_lag4 - t->slow_lag4,
        .samples = t->samples,
    };

    int deterioration = 0;
    if (m.quality_delta < 0) deterioration += -m.quality_delta * 2;
    if (m.q_phase_delta < 0) deterioration += -m.q_phase_delta * 8;
    if (m.origin_delta > 0) deterioration += m.origin_delta;
    if (m.winding_delta > 0) deterioration += m.winding_delta;
    if (m.lag4_delta > 0) deterioration += m.lag4_delta / 2;
    if (t->fast_clip > t->slow_clip) deterioration += (t->fast_clip - t->slow_clip) * 2;
    m.fade_score = fusion_clamp(deterioration, 0, 1000);

    int recovery = 0;
    if (m.quality_delta > 0) recovery += m.quality_delta * 2;
    if (m.q_phase_delta > 0) recovery += m.q_phase_delta * 8;
    if (m.origin_delta < 0) recovery += -m.origin_delta;
    if (m.winding_delta < 0) recovery += -m.winding_delta;
    if (m.lag4_delta < 0) recovery += (-m.lag4_delta) / 2;
    m.recovery_score = fusion_clamp(recovery, 0, 1000);

    int disagreement =
        fusion_temporal_abs(m.quality_delta) * 2 +
        fusion_temporal_abs(m.q_phase_delta) * 6 +
        fusion_temporal_abs(m.origin_delta) / 2 +
        fusion_temporal_abs(m.winding_delta) / 2 +
        fusion_temporal_abs(m.lag4_delta) / 3;
    m.stability = 1000 - fusion_clamp(disagreement, 0, 1000);
    return m;
}
