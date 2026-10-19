#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "arc_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIRECT_GAIN_SETTLE = 0,    /* One control tick (~50 ms); video is not blanked */
    DIRECT_GAIN_HOLD,          /* Optimal envelope reached, zero writes */
    DIRECT_GAIN_SEEK,          /* Estimating input level & executing 1 write */
} direct_gain_state_t;

typedef struct {
    int p_median;              /* Q4/I4 vector median radius [0..63] */
    int q_phase;               /* Phase coherence [0..100%] */
    int clip_permille;         /* Outer Q4-bin occupancy, not proven ADC saturation */
    int origin_permille;       /* Near-zero origin passages [0..1000 permille] */
    int winding_permille;      /* Winding rate [0..1000 permille] */
    int rssi_dbm;              /* Wideband RSSI in dBm (-127 if invalid) */
    bool rssi_valid;           /* True if hardware wideband RSSI is available */
    uint8_t survival_gain;     /* Known acquisition state on this vendor table */
} direct_gain_observation_t;

typedef struct {
    arc_gain_table_t table;
    uint8_t current_gain;
    uint8_t target_gain;
    direct_gain_state_t state;

    uint8_t settle_ticks;      /* Settle countdown after a write (typically 1 tick) */
    uint16_t hold_ticks;       /* Consecutive ticks held in optimal envelope */
    uint8_t no_carrier_ticks;
    int8_t pending_direction; /* Direction awaiting a second independent window */
    uint8_t pending_votes;

    /* Diagnostics & Telemetry */
    int last_p;
    int last_delta_gain;
    int last_estimated_input_dbm;
    bool last_rssi_used;
    uint32_t total_writes;
    uint32_t hold_cycles;
} direct_gain_controller_t;

/**
 * Initialize / Reset Direct Gain controller.
 */
void direct_gain_reset(direct_gain_controller_t *dg,
                       const arc_gain_table_t *table,
                       uint8_t initial_gain);

/**
 * Main Direct Gain execution tick.
 * Evaluates raw RF / Inverse-Q4 power, maps directly to target gain via transfer LUT,
 * executes 1-write hops, and validates resulting quality.
 *
 * Returns the desired hardware gain.
 */
uint8_t direct_gain_tick(direct_gain_controller_t *dg,
                         const direct_gain_observation_t *obs);

/* Reconcile the predicted state with the gain actually submitted to the PHY. */
void direct_gain_sync_applied(direct_gain_controller_t *dg, uint8_t applied_gain);

/**
 * Returns a human-readable state name ("HOLD", "SEEK", "SETTLE").
 */
const char *direct_gain_state_name(direct_gain_state_t state);

#ifdef __cplusplus
}
#endif
