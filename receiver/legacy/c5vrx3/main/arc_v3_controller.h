#pragma once

#include <stdint.h>
#include "arc_phy.h"

#define ARC_V3_FILTER_SAMPLES 5u

typedef enum {
    ARC_V3_ACQUIRE = 0,
    ARC_V3_LOCK,
    ARC_V3_RF_LIMIT,
} arc_v3_state_t;

typedef enum {
    ARC_V3_Q4_STARVED = 0,
    ARC_V3_Q4_TARGET,
    ARC_V3_Q4_HIGH,
    ARC_V3_Q4_OVERLOAD,
} arc_v3_q4_state_t;

typedef struct {
    int p_median;
    int q_phase;
    int clip_permille;
    int origin_permille;
    int winding_permille;
} arc_v3_observation_t;

typedef struct {
    arc_gain_table_t table;
    uint8_t gain;
    arc_v3_state_t state;

    /* Physical-write hold plus robust temporal decision state. */
    unsigned settle;
    unsigned same_class_ticks;
    unsigned bad_lock_ticks;
    unsigned rf_limit_ticks;
    unsigned severe_ticks;
    unsigned up_guard_ticks;
    arc_v3_q4_state_t last_class;

    /* Five completed control windows are reduced component-wise by median
     * before ordinary gain decisions. Raw windows never directly request
     * gain-up; only repeated severe overload may use the fast path. */
    arc_v3_observation_t history[ARC_V3_FILTER_SAMPLES];
    unsigned history_count;
    unsigned history_pos;
    arc_v3_observation_t filtered;
    uint8_t filtered_valid;
} arc_v3_controller_t;

void arc_v3_controller_reset(arc_v3_controller_t *arc,
                             const arc_gain_table_t *table,
                             uint8_t gain);

arc_v3_q4_state_t arc_v3_classify(const arc_v3_observation_t *o);

uint8_t arc_v3_controller_tick(arc_v3_controller_t *arc,
                               const arc_v3_observation_t *o);

const char *arc_v3_state_name(arc_v3_state_t state);
const char *arc_v3_q4_state_name(arc_v3_q4_state_t state);
