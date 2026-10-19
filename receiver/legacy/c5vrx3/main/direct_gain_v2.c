#include "direct_gain_v2.h"
#include <string.h>

/* The vendor index is only a fallback power prior. Every candidate is decoded
 * into its physical tuple and an exact measured edge supersedes that prior. */
#define V2_TARGET_P 22
#define V2_FRESH_US 12000u
#define V2_LOCK_MIN_P 12
#define V2_LOCK_MAX_P 32

static int abs_i(int x) { return x < 0 ? -x : x; }
static int clamp_i(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static uint8_t clamp_gain(const direct_gain_v2_t *v2, int gain)
{
    int hi = v2->table.max_index ? v2->table.max_index : ARC_VENDOR_GAIN_MAX;
    int lo = hi < (int)DIRECT_GAIN_V2_FLOOR ? hi : (int)DIRECT_GAIN_V2_FLOOR;
    return (uint8_t)clamp_i(gain, lo, hi);
}

static bool useful(const direct_gain_v2_observation_t *o)
{
    return o->p >= V2_LOCK_MIN_P && o->p <= V2_LOCK_MAX_P &&
           o->q >= 55 && o->origin_pm <= 350 && o->clip_pm < 80;
}

static direct_gain_v2_edge_t *edge_find(direct_gain_v2_t *v2,
                                        uint8_t from, uint8_t to)
{
    for (unsigned i = 0; i < DIRECT_GAIN_V2_EDGE_SLOTS; ++i)
        if (v2->edge[i].count && v2->edge[i].from == from &&
            v2->edge[i].to == to) return &v2->edge[i];
    return NULL;
}

static int predicted_p(const direct_gain_v2_t *v2, int p, uint8_t from,
                       uint8_t to, bool *learned)
{
    for (unsigned i = 0; i < DIRECT_GAIN_V2_EDGE_SLOTS; ++i) {
        const direct_gain_v2_edge_t *e = &v2->edge[i];
        if (e->count >= 3 && e->from == from && e->to == to) {
            *learned = true;
            return clamp_i((p * e->ratio_q10 + 512) / 1024, 0, 255);
        }
    }
    *learned = false;
    /* ~0.82 dB per index is a conservative, uncalibrated prior. Physical
     * boundary costs below keep it from treating all numerical hops alike. */
    int ratio = 1024;
    int delta = (int)to - (int)from;
    delta = clamp_i(delta, -24, 24);
    while (delta > 0) { ratio = clamp_i((ratio * 121 + 50) / 100, 1, 32768); --delta; }
    while (delta < 0) { ratio = clamp_i((ratio * 100 + 60) / 121, 1, 32768); ++delta; }
    return clamp_i((p * ratio + 512) / 1024, 0, 255);
}

static unsigned boundary_cost(const direct_gain_v2_t *v2,
                              uint8_t from, uint8_t to)
{
    const arc_gain_tuple_t *a = &v2->tuple[from], *b = &v2->tuple[to];
    if (a->rf_stage != b->rf_stage) return 50u;
    if (a->bb_code != b->bb_code) return 12u;
    if (a->fine_code != b->fine_code) return 1u;
    return 0u;
}

static uint8_t choose_target(direct_gain_v2_t *v2,
                             const direct_gain_v2_observation_t *o,
                             bool hard)
{
    uint8_t best = v2->current_gain;
    int best_score = 0x7fffffff;
    uint8_t lo = clamp_gain(v2, 0);
    uint8_t hi = v2->table.max_index;
    for (unsigned g = lo; g <= hi; ++g) {
        bool learned;
        int p = predicted_p(v2, o->p, v2->current_gain, (uint8_t)g, &learned);
        /* Normal corrections accept a broad useful amplitude. This lets an
         * individual Fine adjustment win over a disruptive RF boundary. */
        int amplitude_error = hard ? abs_i(p - V2_TARGET_P) :
            (p < 17 ? 17 - p : p > 27 ? p - 27 : 0);
        int score = amplitude_error * 12;
        if (p < V2_LOCK_MIN_P) score += (V2_LOCK_MIN_P - p) * 16;
        if (p > V2_LOCK_MAX_P) score += (p - V2_LOCK_MAX_P) * 20;
        unsigned boundary = boundary_cost(v2, v2->current_gain, (uint8_t)g);
        score += hard ? (int)(boundary / 4u) : (int)boundary;
        if (g != v2->current_gain) score += hard ? 1 : 10;
        if (!learned && g != v2->current_gain) score += hard ? 1 : 4;
        direct_gain_v2_edge_t *edge = edge_find(v2, v2->current_gain, (uint8_t)g);
        if (edge && edge->count >= 3) score += edge->artifact_score / 10;
        if (score < best_score || (score == best_score &&
            abs_i((int)g - v2->current_gain) < abs_i((int)best - v2->current_gain))) {
            best_score = score;
            best = (uint8_t)g;
        }
    }
    return best;
}

static void learn_edge(direct_gain_v2_t *v2,
                       const direct_gain_v2_observation_t *o)
{
    /* Clipped or carrierless windows do not identify an edge response. A
     * measurement applies only to this exact from->to path. */
    if (v2->prior_p < 5 || v2->prior_p > 40 || o->p < 5 || o->p > 60 ||
        v2->prior_q < 50 || o->q < 50 || v2->prior_clip >= 80 ||
        o->clip_pm >= 250) return;
    direct_gain_v2_edge_t *e = edge_find(v2, v2->prior_gain, v2->current_gain);
    if (!e) {
        e = &v2->edge[v2->edge_next++ % DIRECT_GAIN_V2_EDGE_SLOTS];
        *e = (direct_gain_v2_edge_t){.from = v2->prior_gain,
                                     .to = v2->current_gain, .settle_ms = 12u};
    }
    int ratio = clamp_i((o->p * 1024) / v2->prior_p, 1, 32767);
    if (e->count == 0) e->ratio_q10 = (int16_t)ratio;
    else e->ratio_q10 = (int16_t)((3 * e->ratio_q10 + ratio) / 4);
    e->d_q = (int16_t)(o->q - v2->prior_q);
    e->d_origin = (int16_t)(o->origin_pm - v2->prior_origin);
    e->d_clip = (int16_t)(o->clip_pm - v2->prior_clip);
    e->artifact_score = (uint16_t)clamp_i(abs_i(e->d_q) * 4 +
        abs_i(e->d_origin) / 4 + abs_i(e->d_clip) / 4, 0, 1000);
    if (e->count < 15) ++e->count;
}

void direct_gain_v2_reset(direct_gain_v2_t *v2, const arc_gain_table_t *table,
                          uint8_t current_gain, uint8_t survival_gain)
{
    if (!v2) return;
    memset(v2, 0, sizeof(*v2));
    if (table && table->max_index >= 2u &&
        table->max_index <= ARC_VENDOR_GAIN_MAX) v2->table = *table;
    else arc_gain_table_from_bytes(&v2->table, NULL, ARC_VENDOR_GAIN_MAX);
    for (unsigned g = 0; g <= v2->table.max_index; ++g)
        (void)arc_gain_tuple_decode(&v2->table, (uint8_t)g, &v2->tuple[g]);
    v2->current_gain = clamp_gain(v2, current_gain);
    v2->target_gain = v2->current_gain;
    v2->survival_gain = clamp_gain(v2, survival_gain);
    v2->state = DIRECT_GAIN_V2_SEEK;
}

uint8_t direct_gain_v2_tick(direct_gain_v2_t *v2,
                            const direct_gain_v2_observation_t *o)
{
    if (!v2 || !o) return 52u;
    if (v2->state == DIRECT_GAIN_V2_VERIFY) {
        if (o->observed_us <= v2->write_us ||
            o->observed_us - v2->write_us < V2_FRESH_US) {
            ++v2->stale_rejects;
            return v2->current_gain;
        }
        learn_edge(v2, o);
        if (useful(o)) {
            v2->state = DIRECT_GAIN_V2_LOCK;
            ++v2->locks;
            return v2->current_gain;
        }
        v2->state = DIRECT_GAIN_V2_SEEK;
        if (v2->verify_corrections >= 1u && o->p > 4 && o->p < 60)
            return v2->current_gain;
    }

    if (useful(o)) {
        v2->state = DIRECT_GAIN_V2_LOCK;
        ++v2->locks;
        return v2->current_gain;
    }
    v2->state = DIRECT_GAIN_V2_SEEK;
    bool no_carrier = o->p <= 4 && o->origin_pm >= 650 && o->q < 18;
    bool hard_overload = o->p > 44 || (o->p > 35 && o->clip_pm >= 250);
    bool hard_fade = o->p < 8 && o->origin_pm >= 350;
    bool soft_fade = o->p < 17 && o->fade_score >= 250;
    if (!no_carrier && !hard_overload && !hard_fade && !soft_fade &&
        o->p >= 12 && o->p <= 32) return v2->current_gain;

    uint8_t target = no_carrier ? v2->survival_gain :
        choose_target(v2, o, hard_overload || hard_fade);
    target = clamp_gain(v2, target);
    if (target != v2->current_gain) {
        v2->target_gain = target;
        v2->prior_gain = v2->current_gain;
        v2->prior_p = o->p;
        v2->prior_q = o->q;
        v2->prior_origin = o->origin_pm;
        v2->prior_clip = o->clip_pm;
        if (boundary_cost(v2, v2->current_gain, target)) ++v2->boundary_writes;
        v2->current_gain = target;
        v2->state = DIRECT_GAIN_V2_VERIFY;
        ++v2->writes;
        if (!v2->write_us || o->observed_us - v2->write_us > 100000u)
            v2->verify_corrections = 0;
        else if (v2->verify_corrections < 2u) ++v2->verify_corrections;
        v2->write_us = o->observed_us;
    }
    return v2->current_gain;
}

void direct_gain_v2_sync_applied(direct_gain_v2_t *v2, uint8_t gain,
                                 uint64_t write_us)
{
    if (!v2) return;
    v2->current_gain = clamp_gain(v2, gain);
    v2->target_gain = v2->current_gain;
    v2->write_us = write_us;
    v2->state = DIRECT_GAIN_V2_VERIFY;
}

void direct_gain_v2_learn_settled(direct_gain_v2_t *v2,
                                  const direct_gain_v2_observation_t *o,
                                  uint8_t observed_gain)
{
    if (!v2 || !o || observed_gain != v2->current_gain) return;
    if (v2->write_us && (o->observed_us <= v2->write_us ||
        o->observed_us - v2->write_us < 30000u)) return;
    if (o->p < 5 || o->q < 50 || o->origin_pm > 350) return;
    direct_gain_v2_node_t *node = &v2->node[observed_gain];
    if (!node->count) {
        node->p = (int16_t)o->p;
        node->q = (int16_t)o->q;
        node->origin_pm = (int16_t)o->origin_pm;
        node->clip_pm = (int16_t)o->clip_pm;
    } else {
        node->p = (int16_t)((3 * node->p + o->p) / 4);
        node->q = (int16_t)((3 * node->q + o->q) / 4);
        node->origin_pm = (int16_t)((3 * node->origin_pm + o->origin_pm) / 4);
        node->clip_pm = (int16_t)((3 * node->clip_pm + o->clip_pm) / 4);
    }
    if (node->count < 15u) ++node->count;
    ++v2->slow_samples;
    if (v2->write_us && v2->last_slow_write_us != v2->write_us) {
        learn_edge(v2, o);
        v2->last_slow_write_us = v2->write_us;
    }
}
