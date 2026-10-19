/* C5VRX by Twotoz and contributors: C5VRX-4 sync flywheel.
 *
 * Operator decision 2026-10-05 (AGENTS.md exception): the goggles must never
 * see a moment without valid PAL/NTSC sync. The live path still recovers the
 * transmitted waveform; this only adds the sync pulses the RF lost.
 *
 * It ports the C5VRX-3 flywheel idea (feat/sync-flywheel: PLL line tracker,
 * repair of broken pulses in the raw IQ ring ahead of the TX read) to the
 * C5VRX-4 span75 pipeline, with three changes:
 *   - Detection without demodulation: the 75 ns endpoint phase step
 *     phase[raw[k]] - phase[raw[k-3]] (Phase8 bins; sync ~-38, blanking ~0)
 *     at a few points per line, two table lookups each, valid for any span
 *     alignment. C5VRX-3 demodulated every sample of a window (~0.5 us per
 *     code on the C5) and reached ~1 line in 6.
 *   - Synthesis with a constant phase step per 25 ns sample, so whatever
 *     3-byte grouping the TX BitScrambler uses decodes to the learned sync
 *     (or blanking) step. The wrap residual to the next real sample is
 *     spread over the run, so the edges carry no glitch.
 *   - Vertical sync too: every half-line slot of the vertical interval
 *     (equalizing / broad pulses) is checked against the field timing learned
 *     from real V syncs and rebuilt when missing. Each slot is 32 us, well
 *     inside the ~409 us between the RX write and the TX read.
 * Clean pulses are never written. While the carrier is gone the flywheel
 * keeps coasting (the idle raster takes over after 2 s); when the VTX returns
 * at another phase, two matching full-line scans re-lock it in one step.
 * The colour burst is left alone (HDZero Goggles 2 uses colour-carrier
 * detect for its PAL/NTSC switch). Pure C, host-tested. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 40 MS/s raw samples (one byte = signed Q4 I in bits 7..4, Q in 3..0). */
#define SFW_PAL_LINE_Q8   (2560u * 256u)  /* 64 us */
#define SFW_NTSC_LINE_Q8  650809u         /* 63.5556 us = 2542.22 samples */

typedef struct {
    uint8_t *ring;              /* raw IQ ring shared by RX and TX */
    uint32_t ring_bytes;        /* power of two */
    const uint8_t *phase;       /* Phase8 phase per raw byte (static or mask table) */
    /* Native AGC acquisition mask: synthesized bytes keep data bit 0 at this
     * value (the "not flagged" polarity). Ignored when !mask_bit0. */
    bool mask_bit0;
    uint8_t clear_bit0;
} sfw_ring_t;

typedef enum { SFW_ACQUIRE = 0, SFW_TRACK = 1 } sfw_state_t;

typedef struct {
    sfw_state_t state;
    /* acquisition */
    uint64_t scan_pos;
    uint8_t acq_phase;
    uint16_t acq_hist[256];
    uint32_t acq_n;
    uint32_t acq_run;
    uint64_t acq_last_start;
    bool acq_have_last;
    uint8_t acq_runs;
    /* line tracking */
    uint64_t next_q8;           /* predicted next line start, absolute sample Q8 */
    int32_t period_q8, nominal_q8;
    int16_t thr;                /* sync/not-sync threshold, Phase8 bins per span */
    int16_t sync_q4, blank_q4;  /* learned levels, Phase8 bins per span, Q4 */
    bool levels;
    uint16_t clean_since_acq;
    uint32_t lines_since_clean;
    /* Rebuild every pulse while any recent pulse decoded noisy (a sampled
     * check cannot see every 75 ns span); off after 64 clean lines. */
    uint16_t rebuild_lines, pristine_run;
    /* re-lock while coasting */
    int32_t relock_off;
    uint32_t relock_line;
    bool relock_have;
    /* vertical interval, on the PLL line grid */
    uint32_t grid_line;         /* index of the line at next_q8 */
    bool v_valid;               /* field phase known */
    bool v_armed;               /* slots of the coming vertical interval in progress */
    bool v_half;                /* the next anchor sits half a line after its line start */
    uint32_t v_a_line;          /* grid line of the next anchor */
    uint64_t v_next_q8;         /* armed: anchor = first broad pulse start, Q8 */
    int16_t v_slot;             /* next slot, -npre .. nbroad+npost-1 */
    bool v_found;               /* real broad pulses seen in this field */
    uint64_t v_end_q8;          /* end of the last handled interval: its lines stay vertical */
    /* counters */
    uint32_t lines, clean, repaired, missed, slots_repaired, vsyncs, v_coasted, v_parity,
             acquisitions, relocks, skipped_lines, fast_lines, skipped_floor, rebuilt;
    uint32_t evals;             /* lowm evaluations in the last call */
} sync_flywheel_t;

void sfw_init(sync_flywheel_t *f);
/* Process predicted lines / V slots whose data is complete (< avail_end,
 * absolute sample index, exclusive). Writes only samples >= write_floor
 * (ahead of the TX read). At most budget_evals detection evaluations per call;
 * a flywheel that falls behind coasts lines unanalysed (nothing written).
 * Returns the number of lines handled. */
unsigned sfw_run(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end,
                 uint64_t write_floor, bool allow_repair, uint32_t budget_evals);
bool sfw_locked(const sync_flywheel_t *f);
/* 1 = PAL, 2 = NTSC, 0 = not tracking. */
int sfw_standard(const sync_flywheel_t *f);
