#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "arc_phy.h"

#define DIRECT_GAIN_V2_MAX_STATES (ARC_VENDOR_GAIN_MAX + 1u)
#define DIRECT_GAIN_V2_EDGE_SLOTS 16u
#define DIRECT_GAIN_V2_FLOOR 20u

typedef enum {
    DIRECT_GAIN_V2_SEEK = 0,
    DIRECT_GAIN_V2_VERIFY,
    DIRECT_GAIN_V2_LOCK,
} direct_gain_v2_state_t;

typedef struct {
    int p;
    int q;
    int origin_pm;
    int clip_pm;
    int fade_score;
    uint64_t observed_us;
} direct_gain_v2_observation_t;

typedef struct {
    uint8_t from, to;
    uint8_t count;
    uint8_t settle_ms;
    int16_t ratio_q10;
    int16_t d_q, d_origin, d_clip;
    uint16_t artifact_score;
} direct_gain_v2_edge_t;

typedef struct {
    int16_t p, q, origin_pm, clip_pm;
    uint8_t count;
} direct_gain_v2_node_t;

typedef struct {
    arc_gain_table_t table;
    arc_gain_tuple_t tuple[DIRECT_GAIN_V2_MAX_STATES];
    direct_gain_v2_node_t node[DIRECT_GAIN_V2_MAX_STATES];
    direct_gain_v2_edge_t edge[DIRECT_GAIN_V2_EDGE_SLOTS];
    uint8_t edge_next;
    uint8_t current_gain, target_gain, survival_gain;
    uint8_t prior_gain, verify_corrections;
    int prior_p, prior_q, prior_origin, prior_clip;
    uint64_t write_us;
    uint64_t last_slow_write_us;
    uint32_t writes, locks, stale_rejects, boundary_writes, slow_samples;
    direct_gain_v2_state_t state;
} direct_gain_v2_t;

void direct_gain_v2_reset(direct_gain_v2_t *v2, const arc_gain_table_t *table,
                          uint8_t current_gain, uint8_t survival_gain);
uint8_t direct_gain_v2_tick(direct_gain_v2_t *v2,
                            const direct_gain_v2_observation_t *observation);
void direct_gain_v2_sync_applied(direct_gain_v2_t *v2, uint8_t applied_gain,
                                 uint64_t write_us);
void direct_gain_v2_learn_settled(direct_gain_v2_t *v2,
                                  const direct_gain_v2_observation_t *observation,
                                  uint8_t observed_gain);
