/* C5VRX-5 by Twotoz and the C5VRX contributors: Direct Gain V5 core.
 *
 * The Direct Gain V3 core (centred Q4 envelope, Phase8 coherence, exact
 * vendor 5 GHz gain tuples, zero-write healthy band) plus V5's direct
 * first-window correction, table-maximum listening without a carrier,
 * anti-hunt damping, the interference-burst gate (zerowidth/C5VRX PR #3)
 * and staged overload recovery (Louis Hitchcock, C5VRX #182/#184).
 * C5VRX-5 holds the ultrafine IQ lane for the whole session, so the
 * adaptive-lane and radius-boost paths of C5VRX-4 are not carried. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arc_phy.h"

#define DG_STATES (ARC_VENDOR_GAIN_MAX + 1u)

typedef enum { DG_ACQUIRE = 0, DG_HOLD, DG_SETTLE, DG_VERIFY } dg_state_t;
typedef enum { DG_FINE = 0, DG_BB, DG_RF } dg_transition_t;

typedef struct {
    uint8_t p50, p90, p95;
    uint16_t origin_pm, clip_pm;
    uint8_t coherence;
    uint16_t repeat_pm;     /* samples equal to the one before, per mille */
    uint8_t span;           /* code range of the wider IQ axis over the window */
    uint16_t env_ratio_x100; /* DC-removed envelope mean^2/var of |z|^2, x100 (noise 100) */
    uint64_t observed_us;
} dg_observation_t;

/* A fixed IQ point is not a carrier. Below one quantizer step of receiver
 * noise (DCO FLOOR rows, G54-69 on the board) the uncorrected DC is a
 * constant code: coherence 99-100 %, P50 ~15, inside the healthy band, and
 * Direct Gain held there with no VTX (board walk 2026-10-13, docs/FINDINGS.md).
 * IQSNAP: VTX off at G59 repeats 476-873 per mille per 64 bytes and spans
 * 1-2 codes per 256-byte probe; a weak carrier in noise at G73-75 repeats
 * 15-301 and spans 7-12. Repeats alone are not enough: a strong clean
 * carrier at 40 MS/s repeats often too (half the windows at G27, 1 m from
 * the VTX, read as a fixed point and sent the ladder into LP; board
 * 2026-10-13). A carrier draws a circle around the DC, so it spans many
 * codes even when neighbouring samples repeat.
 *
 * Measurement only: the C/N model reads it as no carrier (receiver.c
 * receiver_obs_cn_x10). Direct Gain does not act on it. Treating it as
 * no carrier sent the gain to the table maximum, where the first window
 * after the write still carried the row's uncorrected DC, clipped, dropped
 * back into the FLOOR rows and looped: ~1400 writes and hundreds of
 * overloads a second with the VTX off (board walk 3, 2026-10-13). The DC
 * of the FLOOR rows has to be corrected in the DCO path instead. */
#define DG_STUCK_PM      400u
#define DG_STUCK_SPAN    2u
static inline bool dg_stuck(const dg_observation_t *o)
{
    return o->repeat_pm >= DG_STUCK_PM && o->span <= DG_STUCK_SPAN;
}

typedef struct {
    arc_gain_table_t table;
    arc_gain_tuple_t tuple[DG_STATES];
    uint16_t relative_power_q10[DG_STATES];
    uint16_t uncertainty_pm[DG_STATES];
    uint16_t artifact_score[DG_STATES];
    uint8_t confidence[DG_STATES];
    uint16_t settle_us[3];
    int32_t virtual_gain_q8;
    uint8_t current_gain, target_gain, prior_gain, corrections;
    uint8_t high_windows, weak_windows, last_direction;
    dg_transition_t transition;
    dg_state_t state;
    dg_observation_t before, before_previous, previous, last_tracking;
    uint8_t stable_windows;
    uint64_t write_us;
    uint32_t writes, holds, verified, learned, overloads;
    bool overload_recovery;       /* staged recovery after an overload drop */
    int8_t last_write_dir;        /* anti-hunt: direction reversals */
    uint8_t reversals;
    uint64_t dir_write_us, damp_until_us, excursion_since_us;
    uint32_t writes_moderate, writes_severe, damp_events, model_mismatches;
    uint64_t saturated_since_us;  /* interference-burst gate */
    bool saturated_run;
    uint32_t bursts_ignored;
    uint64_t last_healthy_us;     /* fade hold: last window in the healthy band */
    /* Map learning only with a confirmed VTX (set by the caller every
     * window). The coherence test alone passes plain 4-bit receiver noise
     * through the 12 / 10 MHz filters (~58 / 61 % coherent), so with the VTX
     * off noise ratios were learned and persisted. */
    bool learn_enabled;
    uint32_t learn_refused;       /* transitions not learned: no confirmed VTX */
    uint32_t fade_holds;          /* windows held through a fade */
    uint32_t magic;
} direct_gain_t;

/* Measured gain map, persisted so a boot does not explore blindly. */
#define DG_MAP_VERSION 4u   /* 4: maps learned from receiver noise (VTX off) are dropped */
typedef struct {
    uint8_t version, max_index;
    uint16_t freq_mhz;            /* context: channel it was measured on */
    uint8_t lane_mode, band5;
    uint8_t confidence[DG_STATES];
    uint8_t reserved[DG_STATES];
    uint16_t power_q10[DG_STATES];
    uint16_t uncertainty_pm[DG_STATES];
} dg_map_blob_t;

void dg_reset(direct_gain_t *g, const arc_gain_table_t *table, uint8_t current_gain);
unsigned dg_export_map(const direct_gain_t *g, dg_map_blob_t *blob);
bool dg_import_map(direct_gain_t *g, const dg_map_blob_t *blob);
/* Low-confidence prior from measured boards (gain_map_preset.h): only for
 * indices without own data; false when it has no entry at the anchor. */
bool dg_import_preset(direct_gain_t *g, const uint16_t *power_q10, unsigned count, uint8_t max_index, uint8_t band5);
dg_observation_t dg_measure(const uint8_t *samples, size_t bytes,
                            const uint8_t phase8_lut[256], uint64_t observed_us);
uint8_t dg_tick(direct_gain_t *g, const dg_observation_t *o);
void dg_sync_applied(direct_gain_t *g, uint8_t gain, uint64_t write_us);
