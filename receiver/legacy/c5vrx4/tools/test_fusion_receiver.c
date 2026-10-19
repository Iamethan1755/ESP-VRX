#include "fusion_receiver.h"
#include "fusion_optimizer.h"
#include <assert.h>
#include <stdio.h>

static fusion_observation_t make_obs(fusion_context_t wanted)
{
    fusion_shadow_metrics_t shadow = {0};
    switch (wanted) {
    case FUSION_CONTEXT_NO_CARRIER:
        return fusion_make_observation(4, 8, 0, 850, 300, 0, 40, 30, 0, shadow);
    case FUSION_CONTEXT_WEAK:
        return fusion_make_observation(10, 38, 0, 280, 120, 20, 40, 30, 20, shadow);
    case FUSION_CONTEXT_BLOCKER:
        return fusion_make_observation(34, 28, 0, 80, 260, 80, 40, 30, 20, shadow);
    case FUSION_CONTEXT_OVERLOAD:
        return fusion_make_observation(50, 65, 100, 20, 30, 10, 40, 30, 90, shadow);
    case FUSION_CONTEXT_CLEAN:
    default:
        return fusion_make_observation(24, 78, 0, 20, 50, 10, 20, 20, 95, shadow);
    }
}

int main(void)
{
    assert(fusion_robust_delta3(2, 3, 15) == 3);

    fusion_shadow_t s;
    fusion_shadow_reset(&s);
    fusion_shadow_push(&s, 0, 80);
    fusion_shadow_push(&s, 10, 80);
    fusion_shadow_push(&s, 20, 80);
    fusion_shadow_metrics_t m = fusion_shadow_finish(&s);
    assert(m.lag2_disagreement_permille > 0);
    assert(m.low_confidence_permille == 0);

    fusion_shadow_reset(&s);
    fusion_shadow_push(&s, 0, 2);
    fusion_shadow_push(&s, 1, 2);
    m = fusion_shadow_finish(&s);
    assert(m.low_confidence_permille == 1000);

    /* PLL-lite learns a clean local slope, then coasts through a low-envelope
     * branch event instead of learning the click as the new frequency. */
    fusion_shadow_reset(&s);
    fusion_shadow_push(&s, 0, 80);
    fusion_shadow_push(&s, 2, 80);
    fusion_shadow_push(&s, 4, 80);
    fusion_shadow_push(&s, 14, 2);
    fusion_shadow_push(&s, 24, 2);
    m = fusion_shadow_finish(&s);
    assert(m.pll_lite_slip_permille > 0);
    assert(m.pll_lite_hold_permille > 0);

    fusion_observation_t clean = make_obs(FUSION_CONTEXT_CLEAN);
    fusion_observation_t weak = make_obs(FUSION_CONTEXT_WEAK);
    fusion_observation_t no_carrier = make_obs(FUSION_CONTEXT_NO_CARRIER);
    assert(clean.context == FUSION_CONTEXT_CLEAN);
    assert(weak.context == FUSION_CONTEXT_WEAK);
    assert(no_carrier.context == FUSION_CONTEXT_NO_CARRIER);
    assert(clean.quality > weak.quality);
    assert(clean.catastrophic_risk < weak.catastrophic_risk);

    fusion_shadow_metrics_t uncertain_shadow = {0};
    uncertain_shadow.trajectory_uncertainty_permille = 900;
    fusion_observation_t uncertain = fusion_make_observation(
        24, 78, 0, 20, 50, 10, 20, 20, 95, uncertain_shadow);
    assert(uncertain.catastrophic_risk > clean.catastrophic_risk);
    assert(uncertain.quality < clean.quality);

    fusion_temporal_t temporal;
    fusion_temporal_reset(&temporal);
    fusion_temporal_metrics_t tm = {0};
    for (unsigned i = 0; i < 32; ++i)
        tm = fusion_temporal_update(&temporal, &clean);
    for (unsigned i = 0; i < 8; ++i)
        tm = fusion_temporal_update(&temporal, &weak);
    assert(tm.fade_score > 0);
    assert(tm.quality_delta < 0);

    fusion_optimizer_t opt;
    fusion_optimizer_reset(&opt, 50);
    for (unsigned i = 0; i < 30; ++i) fusion_optimizer_tick(&opt, &no_carrier, &tm);
    assert(fusion_optimizer_gain(&opt) == 62);

    fusion_optimizer_reset(&opt, 54);
    for (unsigned i = 0; i < 200; ++i) assert(fusion_optimizer_tick(&opt, &clean, &tm) == 54);

    fusion_optimizer_reset(&opt, 62);
    fusion_observation_t overload = make_obs(FUSION_CONTEXT_OVERLOAD);
    assert(fusion_optimizer_tick(&opt, &overload, &tm) <= 54);

    /* Conservative FUSION can retain a G34 floor while RANGE V2 uses the
     * same learner with full near-field headroom down to G2. */
    fusion_optimizer_reset(&opt, 62);
    fusion_optimizer_set_gain_floor(&opt, 34);
    for (unsigned i = 0; i < 20; ++i)
        (void)fusion_optimizer_tick(&opt, &overload, &tm);
    assert(fusion_optimizer_gain(&opt) >= 34);

    fusion_optimizer_reset(&opt, 62);
    fusion_optimizer_set_gain_floor(&opt, 2);
    for (unsigned i = 0; i < 20; ++i)
        (void)fusion_optimizer_tick(&opt, &overload, &tm);
    assert(fusion_optimizer_gain(&opt) < 34);

    puts("Fusion receiver: temporal IQ fusion + Trajectory/PLL-lite risk learner passed");
    return 0;
}
