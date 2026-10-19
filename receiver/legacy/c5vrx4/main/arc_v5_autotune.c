#include "arc_v5_autotune.h"

#include <limits.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#define ARC_V5_FAST_CONFIRM             2u
#define ARC_V5_CONTROL_PERIOD_MS       50u
#define ARC_V5_VERIFY_MIN_TICKS        10u /* proven 500 ms gain-settle hold */
#define ARC_V5_VERIFY_MAX_TICKS        20u /* bound persisted timing influence */
#define ARC_V5_NO_CARRIER_RETURN_TICKS 20u
#define ARC_V5_SAVE_MIN_DIRTY           4u
#define ARC_V5_SAVE_INTERVAL_MS     60000u

static int iabs_i(int v) { return v < 0 ? -v : v; }

static uint8_t clamp_gain(const arc_v5_autotune_t *a, int gain)
{
    if (gain < 2) gain = 2;
    if (gain > a->table.max_index) gain = a->table.max_index;
    return (uint8_t)gain;
}

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t table_fingerprint(const arc_gain_table_t *table)
{
    uint32_t h = fnv1a(table->spans, sizeof(table->spans));
    h ^= table->max_index;
    h *= 16777619u;
    return h;
}

static void factory_model(arc_v5_autotune_t *a)
{
    memset(a->model, 0, sizeof(a->model));

    /* Conservative priors only. samples=0 means these numbers never authorize
     * a large predictive jump by themselves. Real hardware transitions must
     * earn confidence first. */
    for (unsigned g = 0; g < ARC_V5_GAIN_STATES; ++g) {
        a->model[g].dp_q8 = (int16_t)(2 * 256);
        a->model[g].dq_q8 = (int16_t)(3 * 256);
        a->model[g].dorigin_q8 = (int16_t)(-24 * 256);
        a->model[g].dclip_q8 = 0;
        a->model[g].settle_ms = 500u;
    }
}

static void runtime_reset(arc_v5_autotune_t *a, uint8_t gain, uint8_t survival)
{
    a->gain = clamp_gain(a, gain);
    a->survival_gain = clamp_gain(a, survival);
    a->state = ARC_V5_HOLD;
    a->weak_votes = 0u;
    a->strong_votes = 0u;
    a->no_carrier_ticks = 0u;
    a->verify_ticks = 0u;
    a->pending_from = a->pending_to = a->gain;
    memset(&a->pending_before, 0, sizeof(a->pending_before));
    arc_v3_controller_reset(&a->v3, &a->table, a->gain);
}

void arc_v5_autotune_reset(arc_v5_autotune_t *a,
                           const arc_gain_table_t *table,
                           uint8_t gain,
                           uint8_t survival_gain)
{
    memset(a, 0, sizeof(*a));
    a->table = *table;
    a->table_fingerprint = table_fingerprint(table);
    factory_model(a);
    runtime_reset(a, gain, survival_gain);
}

void arc_v5_autotune_rearm(arc_v5_autotune_t *a,
                           const arc_gain_table_t *table,
                           uint8_t gain,
                           uint8_t survival_gain)
{
    uint32_t fp = table_fingerprint(table);
    if (a->table_fingerprint != fp) {
        a->table = *table;
        a->table_fingerprint = fp;
        factory_model(a);
        a->loaded_from_nvs = 0u;
        a->model_generation = 0u;
        a->dirty_updates = 0u;
    } else {
        a->table = *table;
    }
    runtime_reset(a, gain, survival_gain);
}

unsigned arc_v5_model_confidence(const arc_v5_autotune_t *a, uint8_t gain)
{
    if (gain >= ARC_V5_GAIN_STATES) return 0u;
    unsigned sum = 0u;
    unsigned n = 0u;
    int lo = gain > 1u ? (int)gain - 1 : (int)gain;
    int hi = gain + 1u < ARC_V5_GAIN_STATES ? (int)gain + 1 : (int)gain;
    for (int g = lo; g <= hi; ++g) {
        sum += a->model[g].samples;
        ++n;
    }
    return n ? sum / n : 0u;
}

static int ceil_div_pos(int num, int den)
{
    if (num <= 0 || den <= 0) return 0;
    return (num + den - 1) / den;
}

static unsigned exact_up_confidence(const arc_v5_autotune_t *a)
{
    return a->gain < ARC_V5_GAIN_STATES ? a->model[a->gain].samples : 0u;
}

static unsigned exact_down_confidence(const arc_v5_autotune_t *a)
{
    return a->gain > 0u ? a->model[a->gain - 1u].samples : 0u;
}

static int predicted_steps_up(const arc_v5_autotune_t *a,
                              const arc_v5_observation_t *o)
{
    const arc_v5_gain_model_t *m = &a->model[a->gain];
    unsigned confidence = exact_up_confidence(a);
    if (confidence < 8u) return 0;

    int steps = 1;
    if (m->dp_q8 > 64)
        steps = ceil_div_pos((18 - o->p_median) * 256, m->dp_q8) > steps ?
                ceil_div_pos((18 - o->p_median) * 256, m->dp_q8) : steps;
    if (m->dq_q8 > 64)
        steps = ceil_div_pos((85 - o->q_phase) * 256, m->dq_q8) > steps ?
                ceil_div_pos((85 - o->q_phase) * 256, m->dq_q8) : steps;
    if (m->dorigin_q8 < -64)
        steps = ceil_div_pos((o->origin_permille - 160) * 256, -m->dorigin_q8) > steps ?
                ceil_div_pos((o->origin_permille - 160) * 256, -m->dorigin_q8) : steps;

    int limit = confidence >= 64u ? 12 : confidence >= 24u ? 8 : 4;
    if (steps < 2) steps = 2;
    if (steps > limit) steps = limit;
    return steps;
}

static int predicted_steps_down(const arc_v5_autotune_t *a,
                                const arc_v5_observation_t *o)
{
    const arc_v5_gain_model_t *m = &a->model[a->gain > 0 ? a->gain - 1u : 0u];
    unsigned confidence = exact_down_confidence(a);
    if (confidence < 8u) return 0;

    int steps = 1;
    if (m->dp_q8 > 64)
        steps = ceil_div_pos((o->p_median - 26) * 256, m->dp_q8) > steps ?
                ceil_div_pos((o->p_median - 26) * 256, m->dp_q8) : steps;
    if (m->dclip_q8 > 32)
        steps = ceil_div_pos((o->clip_permille - 8) * 256, m->dclip_q8) > steps ?
                ceil_div_pos((o->clip_permille - 8) * 256, m->dclip_q8) : steps;

    int limit = confidence >= 64u ? 10 : confidence >= 24u ? 6 : 3;
    if (steps < 1) steps = 1;
    if (steps > limit) steps = limit;
    return steps;
}

static uint8_t next_anchor_above(const arc_v5_autotune_t *a)
{
    static const uint8_t anchors[] = {16u, 40u, 54u, 70u, 78u, 81u};
    for (unsigned i = 0; i < sizeof(anchors); ++i) {
        uint8_t g = clamp_gain(a, anchors[i]);
        if (g > a->gain) return g;
    }
    return a->gain;
}

static uint8_t factory_weak_target(const arc_v5_autotune_t *a,
                                   const arc_v5_observation_t *o)
{
    if (o->q_phase < 28 || o->origin_permille > 600)
        return next_anchor_above(a);
    if (o->q_phase < 48 || o->origin_permille > 450)
        return clamp_gain(a, (int)a->gain + 6);
    return clamp_gain(a, (int)a->gain + 3);
}

static uint8_t prediction_target(const arc_v5_autotune_t *a,
                                 const arc_v5_observation_t *o,
                                 int direction)
{
    if (direction > 0) {
        int steps = predicted_steps_up(a, o);
        if (steps == 0) return factory_weak_target(a, o);
        return clamp_gain(a, (int)a->gain + steps);
    }

    int steps = predicted_steps_down(a, o);
    if (steps == 0) {
        if (o->clip_permille >= 100 || o->p_median >= 60)
            steps = 6;
        else
            steps = 2;
    }
    return clamp_gain(a, (int)a->gain - steps);
}

static void model_ema(int16_t *dst, int sample_q8, uint16_t samples)
{
    int weight = samples < 8u ? 2 : samples < 32u ? 4 : 8;
    int current = *dst;
    current += (sample_q8 - current) / weight;
    if (current > INT16_MAX) current = INT16_MAX;
    if (current < INT16_MIN) current = INT16_MIN;
    *dst = (int16_t)current;
}

static bool learnable_transition(const arc_v5_autotune_t *a,
                                 const arc_v5_observation_t *after)
{
    const arc_v5_observation_t *before = &a->pending_before;
    if (before->context == ARC_V5_CONTEXT_NO_CARRIER ||
        after->context == ARC_V5_CONTEXT_NO_CARRIER ||
        before->context == ARC_V5_CONTEXT_BLOCKER ||
        after->context == ARC_V5_CONTEXT_BLOCKER)
        return false;
    if (before->clip_permille >= 120 || after->clip_permille >= 120)
        return false;

    int dg = (int)a->pending_to - (int)a->pending_from;
    if (dg == 0) return false;

    /* Reject transitions dominated by an RF fade rather than the actuator.
     * This gate is intentionally broad; repeated coherent samples are needed
     * before confidence can authorize large jumps. */
    if (dg > 0) {
        if (after->p_median < before->p_median - 5 &&
            after->q_phase < before->q_phase - 12)
            return false;
        if (after->origin_permille > before->origin_permille + 180)
            return false;
    } else {
        if (after->p_median > before->p_median + 7 &&
            after->clip_permille > before->clip_permille + 50)
            return false;
    }
    return true;
}

static void learn_transition(arc_v5_autotune_t *a,
                             const arc_v5_observation_t *after)
{
    if (!learnable_transition(a, after)) return;

    int dg = (int)a->pending_to - (int)a->pending_from;
    int abs_dg = iabs_i(dg);
    int sign = dg > 0 ? 1 : -1;

    /* The vendor table is not linear in dB. A broad G54->G70 validation is
     * useful safety evidence, but it must not pretend every unseen edge had
     * the same response. Only local transitions are allowed to shape the
     * persistent per-edge calibration curve. */
    if (abs_dg > 4) return;

    int dp = (after->p_median - a->pending_before.p_median) * sign;
    int dq = (after->q_phase - a->pending_before.q_phase) * sign;
    int dorigin = (after->origin_permille - a->pending_before.origin_permille) * sign;
    int dclip = (after->clip_permille - a->pending_before.clip_permille) * sign;

    int dp_q8 = dp * 256 / abs_dg;
    int dq_q8 = dq * 256 / abs_dg;
    int dorigin_q8 = dorigin * 256 / abs_dg;
    int dclip_q8 = dclip * 256 / abs_dg;

    int lo = a->pending_from < a->pending_to ? a->pending_from : a->pending_to;
    int hi = a->pending_from < a->pending_to ? a->pending_to : a->pending_from;
    if (hi >= (int)ARC_V5_GAIN_STATES) hi = (int)ARC_V5_GAIN_STATES - 1;

    for (int g = lo; g < hi; ++g) {
        arc_v5_gain_model_t *m = &a->model[g];
        model_ema(&m->dp_q8, dp_q8, m->samples);
        model_ema(&m->dq_q8, dq_q8, m->samples);
        model_ema(&m->dorigin_q8, dorigin_q8, m->samples);
        model_ema(&m->dclip_q8, dclip_q8, m->samples);
        if (m->samples < UINT16_MAX) ++m->samples;
        unsigned measured_ms = a->verify_ticks * ARC_V5_CONTROL_PERIOD_MS;
        if (m->settle_ms == 0u) m->settle_ms = (uint16_t)measured_ms;
        else m->settle_ms = (uint16_t)((m->settle_ms * 7u + measured_ms) / 8u);
    }

    ++a->learned_updates;
    ++a->dirty_updates;
    ++a->model_generation;
}

static unsigned verify_ticks_required(const arc_v5_autotune_t *a)
{
    unsigned ticks = ARC_V5_VERIFY_MIN_TICKS;
    int lo = a->pending_from < a->pending_to ? a->pending_from : a->pending_to;
    int hi = a->pending_from < a->pending_to ? a->pending_to : a->pending_from;

    if (hi - lo <= 4) {
        for (int g = lo; g < hi && g < (int)ARC_V5_GAIN_STATES; ++g) {
            unsigned edge_ticks =
                (a->model[g].settle_ms + ARC_V5_CONTROL_PERIOD_MS - 1u) /
                ARC_V5_CONTROL_PERIOD_MS;
            if (edge_ticks > ticks) ticks = edge_ticks;
        }
    }

    if (ticks > ARC_V5_VERIFY_MAX_TICKS) ticks = ARC_V5_VERIFY_MAX_TICKS;
    return ticks;
}

static void clean_zero_write_hold(arc_v5_autotune_t *a)
{
    a->state = ARC_V5_LOCK;
    a->weak_votes = a->strong_votes = 0u;

    /* Keep the V3 safety state synchronized without letting its narrower
     * classifier create PHY writes while Fusion says the signal is healthy. */
    a->v3.gain = a->gain;
    a->v3.state = ARC_V3_LOCK;
    a->v3.settle = 0u;
    a->v3.same_class_ticks = 0u;
    a->v3.bad_lock_ticks = 0u;
    a->v3.rf_limit_ticks = 0u;
    a->v3.severe_ticks = 0u;
    a->v3.history_count = 0u;
    a->v3.history_pos = 0u;
    a->v3.filtered_valid = 0u;
}

static uint8_t begin_transition(arc_v5_autotune_t *a,
                                uint8_t target,
                                const arc_v5_observation_t *before)
{
    target = clamp_gain(a, target);
    if (target == a->gain) return a->gain;

    a->pending_from = a->gain;
    a->pending_to = target;
    a->pending_before = *before;
    a->gain = target;
    a->state = ARC_V5_VERIFY;
    a->verify_ticks = 0u;
    a->weak_votes = 0u;
    a->strong_votes = 0u;

    /* V5 owns the short verification interval. Re-seed V3 at the predicted
     * state so its safety fallback never acts on pre-jump history. */
    arc_v3_controller_reset(&a->v3, &a->table, target);
    a->v3.settle = 2u;
    return a->gain;
}

uint8_t arc_v5_autotune_tick(arc_v5_autotune_t *a,
                             const arc_v5_observation_t *o)
{
    if (!a || !o) return a ? a->gain : 0u;

    if (o->context == ARC_V5_CONTEXT_NO_CARRIER) {
        ++a->no_carrier_ticks;
        a->weak_votes = a->strong_votes = 0u;

        /* Never "learn" pure noise. If gain is below the known survival state,
         * recover there quickly. If a real carrier was just lost while already
         * above survival, preserve that extra sensitivity for ~1 s before
         * returning to the stable survival state. */
        if (a->gain < a->survival_gain && a->no_carrier_ticks >= 3u)
            return begin_transition(a, a->survival_gain, o);
        if (a->gain > a->survival_gain &&
            a->no_carrier_ticks >= ARC_V5_NO_CARRIER_RETURN_TICKS)
            return begin_transition(a, a->survival_gain, o);
        return a->gain;
    }
    a->no_carrier_ticks = 0u;

    if (a->state == ARC_V5_VERIFY) {
        ++a->verify_ticks;

        /* Immediate evidence that a predictive jump went the wrong direction
         * is allowed to undo it; this is not written into the model. */
        if (a->pending_to > a->pending_from &&
            o->context == ARC_V5_CONTEXT_OVERLOAD)
            return begin_transition(a, a->pending_from, o);
        if (a->pending_to < a->pending_from &&
            o->context == ARC_V5_CONTEXT_WEAK &&
            o->q_phase < 45)
            return begin_transition(a, a->pending_from, o);

        if (a->verify_ticks < verify_ticks_required(a))
            return a->gain;

        learn_transition(a, o);
        if (o->context == ARC_V5_CONTEXT_CLEAN)
            clean_zero_write_hold(a);
        else {
            a->state = ARC_V5_HOLD;
            a->v3.settle = 0u;
        }
    }

    bool weak = o->context == ARC_V5_CONTEXT_WEAK &&
                (o->q_phase < 72 || o->p_median < 11 ||
                 o->origin_permille > 350);
    bool strong = o->context == ARC_V5_CONTEXT_OVERLOAD ||
                  o->clip_permille >= 30 || o->p_median > 45;

    if (weak) {
        if (a->weak_votes < 255u) ++a->weak_votes;
        a->strong_votes = 0u;
    } else if (strong) {
        if (a->strong_votes < 255u) ++a->strong_votes;
        a->weak_votes = 0u;
    } else {
        a->weak_votes = a->strong_votes = 0u;
    }

    unsigned up_confidence = exact_up_confidence(a);
    unsigned down_confidence = exact_down_confidence(a);
    unsigned weak_confirm = up_confidence >= 24u ? ARC_V5_FAST_CONFIRM : 3u;
    unsigned strong_confirm = down_confidence >= 24u ? ARC_V5_FAST_CONFIRM : 3u;
    bool severe_factory_weak =
        o->q_phase < 35 || o->origin_permille > 550;

    /* Cold/unknown regions stay on V3 unless raw Q4 is already near collapse.
     * Once the exact edge being extrapolated has confidence, the predictive
     * path may act earlier and skip several discovery steps. */
    if (weak && a->weak_votes >= weak_confirm && a->gain < a->table.max_index &&
        (up_confidence >= 8u || severe_factory_weak)) {
        uint8_t target = prediction_target(a, o, +1);
        return begin_transition(a, target, o);
    }

    if (strong && a->strong_votes >= strong_confirm && down_confidence >= 8u) {
        uint8_t target = prediction_target(a, o, -1);
        return begin_transition(a, target, o);
    }

    /* Fusion CLEAN is the authoritative zero-write zone. V3 deliberately has
     * a narrower target envelope, so invoking it here would reintroduce gain
     * hunting during otherwise healthy video. */
    if (o->context == ARC_V5_CONTEXT_CLEAN) {
        clean_zero_write_hold(a);
        return a->gain;
    }

    /* Stable/unknown territory falls back to the proven ARC V3 controller.
     * V5 can still enlarge a V3-requested step when its learned response has
     * enough confidence, but V3 remains the safety floor. */
    arc_v3_observation_t v3o = {
        .p_median = o->p_median,
        .q_phase = o->q_phase,
        .clip_permille = o->clip_permille,
        .origin_permille = o->origin_permille,
        .winding_permille = o->winding_permille,
    };
    uint8_t v3_target = arc_v3_controller_tick(&a->v3, &v3o);
    if (v3_target != a->gain) {
        uint8_t target = v3_target;
        if (v3_target > a->gain && weak && up_confidence >= 24u)
            target = prediction_target(a, o, +1);
        else if (v3_target < a->gain && strong && down_confidence >= 24u)
            target = prediction_target(a, o, -1);
        return begin_transition(a, target, o);
    }

    a->state = a->v3.state == ARC_V3_LOCK &&
               o->context == ARC_V5_CONTEXT_CLEAN ?
               ARC_V5_LOCK : ARC_V5_HOLD;
    return a->gain;
}

bool arc_v5_export_model(const arc_v5_autotune_t *a,
                         arc_v5_persisted_model_t *out)
{
    if (!a || !out) return false;
    memset(out, 0, sizeof(*out));
    out->magic = ARC_V5_MODEL_MAGIC;
    out->version = ARC_V5_MODEL_VERSION;
    out->bytes = (uint16_t)sizeof(*out);
    out->table_fingerprint = a->table_fingerprint;
    out->generation = a->model_generation;
    memcpy(out->gain, a->model, sizeof(out->gain));
    out->crc = fnv1a(out, offsetof(arc_v5_persisted_model_t, crc));
    return true;
}

bool arc_v5_import_model(arc_v5_autotune_t *a,
                         const arc_v5_persisted_model_t *in)
{
    if (!a || !in) return false;
    if (in->magic != ARC_V5_MODEL_MAGIC ||
        in->version != ARC_V5_MODEL_VERSION ||
        in->bytes != sizeof(*in) ||
        in->table_fingerprint != a->table_fingerprint)
        return false;
    uint32_t crc = fnv1a(in, offsetof(arc_v5_persisted_model_t, crc));
    if (crc != in->crc) return false;

    memcpy(a->model, in->gain, sizeof(a->model));
    a->model_generation = in->generation;
    a->loaded_from_nvs = 1u;
    a->dirty_updates = 0u;
    return true;
}

bool arc_v5_load_nvs(arc_v5_autotune_t *a)
{
#ifdef ESP_PLATFORM
    arc_v5_persisted_model_t blob;
    size_t len = sizeof(blob);
    nvs_handle_t h;
    if (nvs_open("arc_v5", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(h, "model", &blob, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(blob) && arc_v5_import_model(a, &blob);
#else
    (void)a;
    return false;
#endif
}

bool arc_v5_should_save(const arc_v5_autotune_t *a, uint32_t now_ms)
{
    if (!a || a->dirty_updates < ARC_V5_SAVE_MIN_DIRTY) return false;
    return (uint32_t)(now_ms - a->last_save_ms) >= ARC_V5_SAVE_INTERVAL_MS;
}

bool arc_v5_save_nvs(arc_v5_autotune_t *a, uint32_t now_ms)
{
#ifdef ESP_PLATFORM
    if (!a) return false;
    arc_v5_persisted_model_t blob;
    if (!arc_v5_export_model(a, &blob)) return false;

    nvs_handle_t h;
    if (nvs_open("arc_v5", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "model", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    a->dirty_updates = 0u;
    a->last_save_ms = now_ms;
    return true;
#else
    (void)a;
    (void)now_ms;
    return false;
#endif
}

const char *arc_v5_state_name(arc_v5_state_t state)
{
    switch (state) {
    case ARC_V5_VERIFY: return "VERIFY";
    case ARC_V5_LOCK: return "LOCK";
    default: return "HOLD";
    }
}
