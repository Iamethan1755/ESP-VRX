#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arc_phy.h"

#define DG3_STATES (ARC_VENDOR_GAIN_MAX + 1u)

typedef enum {
    DG3_ACQUIRE = 0,
    DG3_HOLD,
    DG3_SETTLE,
    DG3_VERIFY,
} dg3_state_t;

typedef enum {
    DG3_FINE = 0,
    DG3_BB,
    DG3_RF,
} dg3_transition_t;

typedef struct {
    uint8_t p50, p90, p95;
    uint16_t origin_pm, clip_pm;
    uint8_t coherence;
    uint64_t observed_us;
} dg3_observation_t;

typedef struct {
    arc_gain_table_t table;
    arc_gain_tuple_t tuple[DG3_STATES];
    uint16_t relative_power_q10[DG3_STATES];
    uint16_t tuple_settle_us[DG3_STATES];
    uint16_t uncertainty_pm[DG3_STATES];
    uint16_t artifact_score[DG3_STATES];
    uint8_t confidence[DG3_STATES];
    uint8_t bad_state[DG3_STATES]; /* diagnostic clipping count; never a ban */
    uint16_t settle_us[3];
    /* Continuous requested gain relative to the current physical tuple. */
    int32_t virtual_gain_q8;
    uint8_t current_gain, target_gain, survival_gain;
    uint8_t prior_gain, corrections;
    uint8_t high_windows, weak_windows;
    uint8_t last_direction;
    dg3_transition_t transition;
    dg3_state_t state;
    dg3_observation_t before, before_previous, previous, last_tracking;
    uint8_t stable_windows;
    uint64_t write_us;
    uint32_t writes, holds, verified, learned, overloads;
    /* V5 anti-hunt: direction reversals of consecutive writes. */
    int8_t last_write_dir;
    uint8_t reversals;
    uint64_t dir_write_us, damp_until_us;
    uint32_t damp_events;
    /* Range lanes: finer IQ bit sets above the table's maximum analog gain.
     * Lane k scales the Q4 amplitude by exactly 2^k (power 4^k). Lanes are
     * entered only at the table's maximum analog gain; after that the analog
     * gain fine-tunes between the 6 dB lane steps. */
    uint8_t lane, lane_max, junk_windows;
    /* Noise-referenced lane cap: receiver noise r^2 (P50 - 1) at maximum
     * analog gain, learned (Q4 fixed point, lane-0 units) from quiet windows.
     * Lanes finer than the one where noise reaches ~1 step only resolve
     * noise and cost fold headroom. 0 = not yet measured (no cap). */
    uint16_t noise_p50_q4;
    uint8_t lane_cap;
    uint64_t lane_us, lane_hold_until_us, last_fold_us;
    uint8_t fold_streak;      /* consecutive fold drops -> longer hold-off */
    uint32_t lane_changes, fold_drops;
    /* Transitions whose measured ratio disagreed with the tuple model's
     * fine-step prediction (learned anyway, see learn_transition). */
    uint32_t model_mismatches;
    uint32_t magic;           /* DG3_MAGIC once reset: the map may be kept */
    /* Strong-signal radius boost: on a strong, steady carrier the healthy
     * band moves from P50 13..32 (r ~3.5..5.6 cells) to 30..46 (r ~5.4..6.8)
     * for finer phase quantization; any clip/P95/jump warning drops it at
     * once, with a doubling hold-off. Off unless enabled. */
    bool boost_enabled, boost;
    uint8_t boost_streak;
    uint16_t boost_ok_windows;
    uint64_t boost_hold_until_us, boost_exit_us;
    uint32_t boost_entries, boost_exits;
} direct_gain_v3_t;

void direct_gain_v3_reset(direct_gain_v3_t *v3, const arc_gain_table_t *table,
                          uint8_t current_gain, uint8_t survival_gain);

/* Measured gain map, kept across tracking resets on the same table and
 * persisted in NVS so V5 does not explore blindly after every reset/boot.
 * Powers are relative to each other (Q10); only confident states count. */
#define DG3_MAP_VERSION 1u
typedef struct {
    uint8_t version, max_index;
    uint8_t confidence[DG3_STATES];
    uint8_t bad_state[DG3_STATES]; /* v1 compatibility: export zero, ignore on import */
    uint16_t power_q10[DG3_STATES];
    uint16_t uncertainty_pm[DG3_STATES];
} dg3_map_blob_t;
/* Number of confident states written to the blob (0 = nothing worth saving). */
unsigned direct_gain_v3_export_map(const direct_gain_v3_t *v3, dg3_map_blob_t *blob);
/* Applies a blob to a freshly reset controller on the same table. Needs the
 * current gain to be in the blob (the anchor of every relative power). */
bool direct_gain_v3_import_map(direct_gain_v3_t *v3, const dg3_map_blob_t *blob);
/* Strong-signal radius boost on/off (cleared by reset). */
void direct_gain_v3_enable_boost(direct_gain_v3_t *v3, bool enabled);
/* Allow lanes 0..lane_max (0 disables range lanes). Resets to lane 0. */
void direct_gain_v3_enable_lanes(direct_gain_v3_t *v3, uint8_t lane_max);

dg3_observation_t direct_gain_v3_measure(const uint8_t *samples, size_t bytes,
                                          const uint8_t phase8_lut[256],
                                          uint64_t observed_us);
uint8_t direct_gain_v3_tick(direct_gain_v3_t *v3,
                            const dg3_observation_t *observation);
void direct_gain_v3_sync_applied(direct_gain_v3_t *v3, uint8_t gain,
                                 uint64_t write_us);
