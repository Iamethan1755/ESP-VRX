#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fusion_receiver.h"
#include "fusion_temporal.h"

#define FUSION_OPT_STATE_COUNT 13u
#define FUSION_OPT_SETTLE_TICKS 10u
#define FUSION_OPT_EVAL_TICKS 8u
#define FUSION_OPT_DECISION_TICKS 40u
#define FUSION_OPT_FAST_FADE_TICKS 3u
#define FUSION_OPT_COOLDOWN_TICKS 80u
#define FUSION_OPT_DECAY_TICKS 256u

typedef struct {
    uint16_t visits;
    int mean_quality;
    int mean_risk;
} fusion_bandit_cell_t;

/* One cell describes the local transition between state i (higher gain) and
 * state i+1 (lower gain). mean_lower_minus_higher > 0 means lower gain has
 * historically been better in this context. */
typedef struct {
    uint16_t visits;
    int mean_lower_minus_higher;
} fusion_edge_cell_t;

typedef struct {
    fusion_bandit_cell_t cell[FUSION_CONTEXT_COUNT][FUSION_OPT_STATE_COUNT];
    fusion_edge_cell_t edge[FUSION_CONTEXT_COUNT][FUSION_OPT_STATE_COUNT - 1u];
    uint8_t state, previous_state, trial_state, max_state;
    fusion_context_t context, trial_context;
    unsigned settle, cooldown, decision_age, trial_samples;
    int trial_sum, trial_risk_sum, baseline_quality, baseline_risk;
    uint32_t updates;
    bool trial_active;
} fusion_optimizer_t;

static const uint8_t s_fusion_gain_states[FUSION_OPT_STATE_COUNT] = {
    62u, 58u, 54u, 50u, 46u, 42u, 38u, 34u, 28u, 22u, 16u, 8u, 2u
};

static inline uint8_t fusion_optimizer_state_for_gain(uint8_t gain)
{
    unsigned best = 0;
    int best_error = 999;
    for (unsigned i = 0; i < FUSION_OPT_STATE_COUNT; ++i) {
        int e = (int)s_fusion_gain_states[i] - (int)gain;
        if (e < 0) e = -e;
        if (e < best_error) { best_error = e; best = i; }
    }
    return (uint8_t)best;
}

static inline uint8_t fusion_optimizer_gain(const fusion_optimizer_t *o)
{
    return s_fusion_gain_states[o->state];
}

static inline void fusion_optimizer_reset(fusion_optimizer_t *o, uint8_t gain)
{
    *o = (fusion_optimizer_t){0};
    o->state = fusion_optimizer_state_for_gain(gain);
    o->previous_state = o->state;
    o->trial_state = o->state;
    o->max_state = FUSION_OPT_STATE_COUNT - 1u;
    o->settle = FUSION_OPT_SETTLE_TICKS;
    o->context = FUSION_CONTEXT_NO_CARRIER;
    o->trial_context = FUSION_CONTEXT_NO_CARRIER;
}

static inline void fusion_optimizer_set_gain_floor(fusion_optimizer_t *o,
                                                    uint8_t minimum_gain)
{
    /* max_state is the lowest physical gain this profile may enter. The
     * ordered table remains shared so RANGE V2 can recover from near-VTX
     * overload without changing the conservative FUSION baseline. */
    o->max_state = fusion_optimizer_state_for_gain(minimum_gain);
    if (o->state > o->max_state) o->state = o->max_state;
    if (o->previous_state > o->max_state) o->previous_state = o->max_state;
    if (o->trial_state > o->max_state) o->trial_state = o->max_state;
}

static inline void fusion_bandit_update(fusion_bandit_cell_t *c,
                                        int score, int risk)
{
    score = fusion_clamp(score, 0, 1000);
    risk = fusion_clamp(risk, 0, 1000);
    c->mean_quality = c->visits ? (c->mean_quality * 7 + score) / 8 : score;
    c->mean_risk = c->visits ? (c->mean_risk * 7 + risk) / 8 : risk;
    if (c->visits < UINT16_MAX) ++c->visits;
}

static inline void fusion_edge_update(fusion_edge_cell_t *e, int delta)
{
    delta = fusion_clamp(delta, -1000, 1000);
    e->mean_lower_minus_higher =
        e->visits ? (e->mean_lower_minus_higher * 7 + delta) / 8 : delta;
    if (e->visits < UINT16_MAX) ++e->visits;
}

static inline void fusion_optimizer_decay(fusion_optimizer_t *o)
{
    /* The RF environment is non-stationary. Old certainty must decay or a
     * flight after a channel/antenna/environment change remains overconfident
     * in stale gain states forever. Keep means, decay effective sample count. */
    for (unsigned c = 0; c < FUSION_CONTEXT_COUNT; ++c) {
        for (unsigned s = 0; s < FUSION_OPT_STATE_COUNT; ++s)
            o->cell[c][s].visits /= 2u;
        for (unsigned e = 0; e + 1u < FUSION_OPT_STATE_COUNT; ++e)
            o->edge[c][e].visits /= 2u;
    }
}

static inline int fusion_prior(fusion_context_t ctx, unsigned state)
{
    switch (ctx) {
    case FUSION_CONTEXT_NO_CARRIER: return state == 0u ? 700 : 350 - (int)state * 20;
    case FUSION_CONTEXT_WEAK: return 650 - (int)state * 35;
    case FUSION_CONTEXT_BLOCKER: return 430 + (int)state * 20;
    case FUSION_CONTEXT_OVERLOAD: return 300 + (int)state * 35;
    case FUSION_CONTEXT_CLEAN: default: return 600;
    }
}

static inline int fusion_risk_prior(fusion_context_t ctx)
{
    switch (ctx) {
    case FUSION_CONTEXT_NO_CARRIER: return 900;
    case FUSION_CONTEXT_WEAK: return 600;
    case FUSION_CONTEXT_BLOCKER: return 650;
    case FUSION_CONTEXT_OVERLOAD: return 850;
    case FUSION_CONTEXT_CLEAN: default: return 150;
    }
}

static inline bool fusion_state_allowed(const fusion_optimizer_t *o,
                                        fusion_context_t ctx, unsigned state)
{
    if (state > o->max_state) return false;
    switch (ctx) {
    case FUSION_CONTEXT_NO_CARRIER: return state == 0u;
    case FUSION_CONTEXT_WEAK: return state <= 3u;
    case FUSION_CONTEXT_CLEAN: return state <= 5u;
    case FUSION_CONTEXT_BLOCKER: return state >= 2u;
    case FUSION_CONTEXT_OVERLOAD: return state >= 4u;
    default: return false;
    }
}

static inline int fusion_edge_prediction(const fusion_optimizer_t *o,
                                         fusion_context_t ctx,
                                         unsigned candidate)
{
    if (candidate == (unsigned)o->state + 1u) {
        const fusion_edge_cell_t *e = &o->edge[ctx][o->state];
        return e->visits ? e->mean_lower_minus_higher : 0;
    }
    if (candidate + 1u == (unsigned)o->state) {
        const fusion_edge_cell_t *e = &o->edge[ctx][candidate];
        return e->visits ? -e->mean_lower_minus_higher : 0;
    }
    return 0;
}

static inline int fusion_candidate_value(const fusion_optimizer_t *o,
                                         fusion_context_t ctx, unsigned state,
                                         const fusion_temporal_metrics_t *tm)
{
    const fusion_bandit_cell_t *c = &o->cell[ctx][state];
    int expected = c->visits ? c->mean_quality : fusion_prior(ctx, state);
    int risk = c->visits ? c->mean_risk : fusion_risk_prior(ctx);
    int explore = c->visits ? 120 / (int)(c->visits + 1u) : 160;
    int distance = (int)state - (int)o->state;
    if (distance < 0) distance = -distance;

    int value = expected - risk / 3 + explore - (20 + distance * 35);
    value += fusion_edge_prediction(o, ctx, state) / 2;

    if (tm) {
        /* Lower state number means more gain. A real fast fade biases one
         * local step toward sensitivity; recovery/blocker evidence biases
         * toward headroom. This is a bias, never an unbounded jump. */
        if (ctx == FUSION_CONTEXT_WEAK && tm->fade_score >= 250) {
            if (state < o->state) value += tm->fade_score / 3;
            if (state > o->state) value -= tm->fade_score / 3;
        }
        if ((ctx == FUSION_CONTEXT_BLOCKER || ctx == FUSION_CONTEXT_OVERLOAD) &&
            state > o->state)
            value += (1000 - tm->stability) / 5;
    }
    return value;
}

static inline uint8_t fusion_best_candidate(const fusion_optimizer_t *o,
                                            fusion_context_t ctx,
                                            const fusion_temporal_metrics_t *tm)
{
    unsigned best = o->state;
    int best_value = -100000;
    for (unsigned s = 0; s < FUSION_OPT_STATE_COUNT; ++s) {
        if (!fusion_state_allowed(o, ctx, s)) continue;
        int distance = (int)s - (int)o->state;
        if (distance < 0) distance = -distance;
        /* Gain is an ordered physical chain, but not assumed monotonic.
         * Learn it one local edge at a time; only hard safety states jump. */
        if (distance > 1 && ctx != FUSION_CONTEXT_NO_CARRIER &&
            ctx != FUSION_CONTEXT_OVERLOAD)
            continue;
        int value = fusion_candidate_value(o, ctx, s, tm);
        if (value > best_value) { best_value = value; best = s; }
    }
    return (uint8_t)best;
}

static inline void fusion_begin_trial(fusion_optimizer_t *o,
                                      uint8_t candidate,
                                      const fusion_observation_t *obs)
{
    o->previous_state = o->state;
    o->trial_state = candidate;
    o->state = candidate;
    o->baseline_quality = obs->quality;
    o->baseline_risk = obs->catastrophic_risk;
    o->trial_context = obs->context;
    o->trial_samples = 0;
    o->trial_sum = 0;
    o->trial_risk_sum = 0;
    o->trial_active = true;
    o->settle = FUSION_OPT_SETTLE_TICKS;
    o->decision_age = 0;
}

static inline void fusion_record_trial_edge(fusion_optimizer_t *o,
                                            fusion_context_t ctx,
                                            int trial_quality)
{
    int observed = trial_quality - o->baseline_quality;
    if (o->trial_state == (uint8_t)(o->previous_state + 1u)) {
        fusion_edge_update(&o->edge[ctx][o->previous_state], observed);
    } else if (o->previous_state == (uint8_t)(o->trial_state + 1u)) {
        fusion_edge_update(&o->edge[ctx][o->trial_state], -observed);
    }
}

static inline uint8_t fusion_optimizer_tick(
    fusion_optimizer_t *o,
    const fusion_observation_t *obs,
    const fusion_temporal_metrics_t *tm)
{
    o->context = obs->context;
    if (o->cooldown) --o->cooldown;
    ++o->decision_age;
    if (++o->updates % FUSION_OPT_DECAY_TICKS == 0u)
        fusion_optimizer_decay(o);

    if (obs->clip_permille >= 80) {
        unsigned next = (unsigned)o->state + 2u;
        if (next > o->max_state) next = o->max_state;
        o->trial_active = false;
        o->state = (uint8_t)next;
        o->settle = FUSION_OPT_SETTLE_TICKS;
        o->cooldown = 20u;
        o->decision_age = 0;
        return fusion_optimizer_gain(o);
    }

    if (o->settle) { --o->settle; return fusion_optimizer_gain(o); }

    fusion_bandit_update(&o->cell[obs->context][o->state],
                         obs->quality, obs->catastrophic_risk);

    if (o->trial_active) {
        o->trial_sum += obs->quality;
        o->trial_risk_sum += obs->catastrophic_risk;
        ++o->trial_samples;
        if (o->trial_samples < FUSION_OPT_EVAL_TICKS) return fusion_optimizer_gain(o);

        int trial_quality = o->trial_sum / (int)o->trial_samples;
        int trial_risk = o->trial_risk_sum / (int)o->trial_samples;
        fusion_record_trial_edge(o, o->trial_context, trial_quality);

        o->trial_active = false;
        o->trial_samples = 0;
        o->trial_sum = 0;
        o->trial_risk_sum = 0;
        o->cooldown = FUSION_OPT_COOLDOWN_TICKS;
        o->decision_age = 0;

        /* Hierarchical acceptance: average quality cannot buy a large increase
         * in catastrophic phase risk. A big risk reduction may win with a
         * small quality cost because preventing CVBS/sync bombs matters more. */
        bool quality_win = trial_quality >= o->baseline_quality + 10 &&
                           trial_risk <= o->baseline_risk + 60;
        bool risk_win = trial_risk + 80 < o->baseline_risk &&
                        trial_quality >= o->baseline_quality - 25;
        if (!quality_win && !risk_win) {
            o->state = o->previous_state;
            o->settle = FUSION_OPT_SETTLE_TICKS;
        }
        return fusion_optimizer_gain(o);
    }

    /* Loss at the range edge means maximum known sensitivity. Never walk
     * downward through lower-gain states merely because semantic video died. */
    if (obs->context == FUSION_CONTEXT_NO_CARRIER) {
        if (o->state != 0u) {
            o->previous_state = o->state;
            o->state = 0u;
            o->settle = FUSION_OPT_SETTLE_TICKS;
            o->decision_age = 0;
        }
        return fusion_optimizer_gain(o);
    }

    if (obs->context == FUSION_CONTEXT_CLEAN &&
        obs->quality >= 700 && obs->confidence >= 650 &&
        obs->catastrophic_risk < 250)
        return fusion_optimizer_gain(o);

    bool severe_fade = tm && tm->fade_score >= 600 &&
                       obs->context == FUSION_CONTEXT_WEAK &&
                       obs->catastrophic_risk >= 450;
    if (o->cooldown && !severe_fade) return fusion_optimizer_gain(o);

    unsigned required_age = severe_fade ? FUSION_OPT_FAST_FADE_TICKS
                                        : FUSION_OPT_DECISION_TICKS;
    if (o->decision_age < required_age) return fusion_optimizer_gain(o);

    /* Do not chase one contradictory transient. Severe fade is allowed to
     * override this because waiting for perfect stability can lose the link. */
    if (tm && tm->stability < 250 && !severe_fade) {
        o->decision_age = required_age / 2u;
        return fusion_optimizer_gain(o);
    }

    uint8_t candidate = fusion_best_candidate(o, obs->context, tm);
    if (candidate != o->state) fusion_begin_trial(o, candidate, obs);
    else o->decision_age = 0;
    return fusion_optimizer_gain(o);
}
