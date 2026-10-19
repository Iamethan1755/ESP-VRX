#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Snapshot estimator, never a source of live phase/PHY/output writes. */
typedef struct {
    unsigned pairs, ambiguous_pm, origin_pm, clip_pm;
    int mean_i_mcell, mean_q_mcell;
    unsigned pulses, repeated, period_raw;
    int sync_bins, blank_bins, span_bins, sync_mad_bins, blank_mad_bins;
    int sync_mv, blank_mv, sync_depth_mv;
    unsigned suggested_scale_q10;
    bool levels_valid;
} c5v4_cvbs_stats_t;
/* transfer: 0 STD150, 1 LEGACY_FULL, 2 CVBS150 (nominal-voltage estimates). */
void c5v4_cvbs_analyze(const uint8_t *raw, size_t n, bool history, unsigned transfer,
                       c5v4_cvbs_stats_t *out);
/* Native AGC acquisition mask active: the Q LSB is the AGC witness, so STATIC
 * snapshots decode Q3 at its cell centre exactly like the masked program. */
void c5v4_cvbs_set_mask_decode(bool enabled);
/* Phase8 phase per raw byte: the static decoder table, or the acquisition-mask
 * variant that ignores data bit 0. */
const uint8_t *c5v4_cvbs_phase_table(bool mask);
