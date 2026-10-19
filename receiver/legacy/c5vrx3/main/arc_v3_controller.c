#include "arc_v3_controller.h"

#include <stdbool.h>

#define ARC_V3_SETTLE_TICKS             10u
#define ARC_V3_LOCK_BAD_TICKS            6u
#define ARC_V3_UP_CONFIRM_TICKS          5u
#define ARC_V3_HARD_UP_CONFIRM_TICKS     3u
#define ARC_V3_OVERLOAD_CONFIRM_TICKS    2u
#define ARC_V3_HIGH_CONFIRM_TICKS        3u
#define ARC_V3_UP_GUARD_TICKS           20u
#define ARC_V3_RF_LIMIT_CONFIRM_TICKS    8u

static uint8_t clamp_gain(const arc_v3_controller_t *arc, int gain)
{
    if (gain < 2) gain = 2;
    if (gain > arc->table.max_index) gain = arc->table.max_index;
    return (uint8_t)gain;
}

static uint8_t step_gain(const arc_v3_controller_t *arc, int delta)
{
    return clamp_gain(arc, (int)arc->gain + delta);
}

static int hard_starved(const arc_v3_observation_t *o)
{
    return o->clip_permille <= 8 &&
           o->p_median <= 4 &&
           o->q_phase < 15 &&
           o->origin_permille >= 800;
}

static int lock_hold_good(const arc_v3_observation_t *o)
{
    return o->clip_permille <= 24 &&
           o->p_median >= 6 && o->p_median <= 40 &&
           o->q_phase >= 45 &&
           o->origin_permille <= 500 &&
           o->winding_permille < 320;
}

static int median_int(const int *values, unsigned count)
{
    int tmp[ARC_V3_FILTER_SAMPLES];

    for (unsigned i = 0; i < count; ++i)
        tmp[i] = values[i];

    for (unsigned i = 1; i < count; ++i) {
        int v = tmp[i];
        unsigned j = i;
        while (j > 0u && tmp[j - 1u] > v) {
            tmp[j] = tmp[j - 1u];
            --j;
        }
        tmp[j] = v;
    }

    return tmp[count / 2u];
}

static void filter_push(arc_v3_controller_t *arc,
                        const arc_v3_observation_t *o)
{
    arc->history[arc->history_pos] = *o;
    arc->history_pos = (arc->history_pos + 1u) % ARC_V3_FILTER_SAMPLES;
    if (arc->history_count < ARC_V3_FILTER_SAMPLES)
        ++arc->history_count;

    if (arc->history_count < ARC_V3_FILTER_SAMPLES) {
        arc->filtered_valid = 0u;
        return;
    }

    int p[ARC_V3_FILTER_SAMPLES];
    int q[ARC_V3_FILTER_SAMPLES];
    int clip[ARC_V3_FILTER_SAMPLES];
    int origin[ARC_V3_FILTER_SAMPLES];
    int winding[ARC_V3_FILTER_SAMPLES];

    for (unsigned i = 0; i < ARC_V3_FILTER_SAMPLES; ++i) {
        p[i] = arc->history[i].p_median;
        q[i] = arc->history[i].q_phase;
        clip[i] = arc->history[i].clip_permille;
        origin[i] = arc->history[i].origin_permille;
        winding[i] = arc->history[i].winding_permille;
    }

    arc->filtered = (arc_v3_observation_t) {
        .p_median = median_int(p, ARC_V3_FILTER_SAMPLES),
        .q_phase = median_int(q, ARC_V3_FILTER_SAMPLES),
        .clip_permille = median_int(clip, ARC_V3_FILTER_SAMPLES),
        .origin_permille = median_int(origin, ARC_V3_FILTER_SAMPLES),
        .winding_permille = median_int(winding, ARC_V3_FILTER_SAMPLES),
    };
    arc->filtered_valid = 1u;
}

static void filter_reset(arc_v3_controller_t *arc)
{
    arc->history_count = 0u;
    arc->history_pos = 0u;
    arc->filtered_valid = 0u;
}

static void note_class(arc_v3_controller_t *arc, arc_v3_q4_state_t cls)
{
    if (arc->last_class == cls) {
        if (arc->same_class_ticks < 255u) ++arc->same_class_ticks;
    } else {
        arc->last_class = cls;
        arc->same_class_ticks = 1u;
    }
}

static uint8_t write_next(arc_v3_controller_t *arc, uint8_t next)
{
    if (next != arc->gain) {
        bool down = next < arc->gain;

        arc->gain = next;
        arc->settle = ARC_V3_SETTLE_TICKS;
        arc->same_class_ticks = 0u;
        arc->rf_limit_ticks = 0u;
        arc->bad_lock_ticks = 0u;
        arc->severe_ticks = 0u;
        filter_reset(arc);

        /* Hardware walk-back data showed a bad short fade can otherwise
         * reverse a legitimate downward trajectory (e.g. G66 -> G81).
         * Downward movement therefore creates a one-second no-up window.
         * True persistent starvation still wins once that guard expires. */
        if (down)
            arc->up_guard_ticks = ARC_V3_UP_GUARD_TICKS;
    }
    return arc->gain;
}

void arc_v3_controller_reset(arc_v3_controller_t *arc,
                             const arc_gain_table_t *table,
                             uint8_t gain)
{
    *arc = (arc_v3_controller_t){0};
    arc->table = *table;
    arc->gain = clamp_gain(arc, gain);
    arc->state = ARC_V3_ACQUIRE;
    arc->settle = ARC_V3_SETTLE_TICKS;
    arc->last_class = ARC_V3_Q4_STARVED;
}

arc_v3_q4_state_t arc_v3_classify(const arc_v3_observation_t *o)
{
    if (o->clip_permille >= 32 || o->p_median > 45)
        return ARC_V3_Q4_OVERLOAD;

    if (o->clip_permille <= 16 &&
        o->p_median >= 8 && o->p_median <= 34 &&
        o->q_phase >= 55 &&
        o->origin_permille <= 350 &&
        o->winding_permille < 300)
        return ARC_V3_Q4_TARGET;

    if (o->clip_permille > 16 || o->p_median > 34)
        return ARC_V3_Q4_HIGH;

    return ARC_V3_Q4_STARVED;
}

uint8_t arc_v3_controller_tick(arc_v3_controller_t *arc,
                               const arc_v3_observation_t *o)
{
    if (arc->up_guard_ticks)
        --arc->up_guard_ticks;

    filter_push(arc, o);

    /* Clipping is the one raw-window signal allowed a faster path. Even then,
     * require two consecutive severe windows and never trust the first two
     * observations after a PHY write. */
    bool severe_overload = o->clip_permille >= 80 || o->p_median > 60;
    if (severe_overload) {
        if (arc->severe_ticks < 255u) ++arc->severe_ticks;
    } else {
        arc->severe_ticks = 0u;
    }

    bool emergency_ready = arc->settle == 0u ||
                           arc->settle <= ARC_V3_SETTLE_TICKS - 2u;
    if (arc->severe_ticks >= 2u && emergency_ready) {
        arc->state = ARC_V3_ACQUIRE;
        return write_next(arc, step_gain(arc, -4));
    }

    if (arc->settle) {
        --arc->settle;
        return arc->gain;
    }

    if (!arc->filtered_valid)
        return arc->gain;

    const arc_v3_observation_t *f = &arc->filtered;
    arc_v3_q4_state_t cls = arc_v3_classify(f);
    note_class(arc, cls);

    if (arc->state == ARC_V3_RF_LIMIT) {
        if (cls == ARC_V3_Q4_STARVED)
            return arc->gain;

        arc->state = ARC_V3_ACQUIRE;
        arc->rf_limit_ticks = 0u;
    }

    if (arc->state == ARC_V3_LOCK) {
        if (lock_hold_good(f)) {
            arc->bad_lock_ticks = 0u;
            return arc->gain; /* Zero-write clean LOCK invariant. */
        }

        if (++arc->bad_lock_ticks < ARC_V3_LOCK_BAD_TICKS)
            return arc->gain;

        /* Keep the already accumulated filtered-class persistence. Six bad
         * rolling medians are enough evidence; do not make ACQUIRE start over
         * from one raw sample. */
        arc->state = ARC_V3_ACQUIRE;
        arc->bad_lock_ticks = 0u;
    }

    if (cls == ARC_V3_Q4_TARGET) {
        if (arc->same_class_ticks >= 3u) {
            arc->state = ARC_V3_LOCK;
            arc->bad_lock_ticks = 0u;
        }
        return arc->gain;
    }

    if (cls == ARC_V3_Q4_OVERLOAD) {
        if (arc->same_class_ticks < ARC_V3_OVERLOAD_CONFIRM_TICKS)
            return arc->gain;
        return write_next(arc, step_gain(arc, -1));
    }

    if (cls == ARC_V3_Q4_HIGH) {
        if (arc->same_class_ticks < ARC_V3_HIGH_CONFIRM_TICKS)
            return arc->gain;
        return write_next(arc, step_gain(arc, -1));
    }

    /* STARVED is deliberately asymmetric with overload:
     * - it is based on the five-window median, never one bad fade;
     * - gain-up requires more persistence than gain-down;
     * - after a legitimate downward move, a one-second reversal guard blocks
     *   the G66 -> G81 style bounce observed while walking toward the VTX. */
    if (arc->gain >= arc->table.max_index) {
        if (arc->rf_limit_ticks < 255u) ++arc->rf_limit_ticks;
        if (arc->rf_limit_ticks >= ARC_V3_RF_LIMIT_CONFIRM_TICKS)
            arc->state = ARC_V3_RF_LIMIT;
        return arc->gain;
    }

    bool hard = hard_starved(f);
    unsigned confirm = hard ?
        ARC_V3_HARD_UP_CONFIRM_TICKS : ARC_V3_UP_CONFIRM_TICKS;

    if (arc->same_class_ticks < confirm)
        return arc->gain;

    if (arc->up_guard_ticks != 0u)
        return arc->gain;

    return write_next(arc, step_gain(arc, hard ? 4 : 1));
}

const char *arc_v3_state_name(arc_v3_state_t state)
{
    switch (state) {
    case ARC_V3_LOCK: return "LOCK";
    case ARC_V3_RF_LIMIT: return "RF_LIMIT";
    default: return "ACQUIRE";
    }
}

const char *arc_v3_q4_state_name(arc_v3_q4_state_t state)
{
    switch (state) {
    case ARC_V3_Q4_TARGET: return "TARGET";
    case ARC_V3_Q4_HIGH: return "HIGH";
    case ARC_V3_Q4_OVERLOAD: return "OVERLOAD";
    default: return "STARVED";
    }
}
