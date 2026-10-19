/* C5VRX by Twotoz and contributors: native AGC acquisition witness.
 *
 * MODEM_DIAG carries the RF dump word: Q[9:0], I[9:0], the RX gain index in
 * bits 20..27 and the native AGC state machine in bits 28..31 (ESPARGOS
 * esp-sdr word format; DIAG[20..31] matched the dump bit-exactly in
 * tools/phy_phase_tap_probe.md, static there because gain was forced: G52,
 * state 1). For calibration PARLIO captures one byte per sample with the gain
 * index DIAG[20..26] in bits 0..6 and one state bit DIAG[28+s] in bit 7.
 * Gain changes mark acquisitions; the state bit that best separates
 * acquisition from trapped samples becomes the per-sample hold flag.
 * Pure functions: host-tested, no hardware access. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AGC_WITNESS_MERGE   160u  /* changes <= 4 us apart: one acquisition */
#define AGC_WITNESS_SETTLE  24u   /* ~0.6 us after the last change */
#define AGC_WITNESS_GUARD   48u   /* trapped = this far from any change */
#define AGC_WITNESS_EDGE    64u   /* flag edge search around an acquisition */
#define AGC_WITNESS_RATE_HZ 40000000u

typedef struct {
    uint32_t samples[4], acq[4], trapped[4];
    uint32_t ones_acq[4], ones_trapped[4];
    /* Per bit and polarity: flag-active lead before the first change and
     * lag after the last change, in samples (sums and counts). */
    int32_t lead_sum[4][2], lag_sum[4][2];
    uint32_t edge_count[4][2];
    uint32_t acquisitions, acq_samples, windows;
    uint8_t gain_min_acq, trapped_min, trapped_max;
} agc_witness_t;

typedef struct {
    int bit;            /* 0..3 -> DIAG[28 + bit], -1 none */
    int invert;         /* flag = !state_bit */
    unsigned separation_pm, lead_samples, lag_samples;
    unsigned active_acq_pm, active_trapped_pm; /* flag share for the choice */
    unsigned acq_per_ms_x10, acq_us_x10, acq_share_pm;
    unsigned gain_min_acq, trapped_min, trapped_max;
} agc_witness_result_t;

static inline void agc_witness_init(agc_witness_t *w)
{
    *w = (agc_witness_t){0};
    w->gain_min_acq = 255u;
    w->trapped_min = 255u;
}

/* One contiguous window captured with DIAG[28 + bit] in data bit 7. */
static inline void agc_witness_add(agc_witness_t *w, const uint8_t *s, size_t n, unsigned bit)
{
    if (bit > 3u || n < 2u) return;
    /* Acquisition spans [first change, last change]. */
    enum { MAX_ACQ = 64 }; /* >= 4092 samples / (MERGE + 1) */
    uint32_t first[MAX_ACQ], last[MAX_ACQ];
    unsigned count = 0;
    for (size_t k = 1; k < n; ++k) {
        if ((s[k] & 127u) == (s[k - 1] & 127u)) continue;
        if (count && k - last[count - 1] <= AGC_WITNESS_MERGE) last[count - 1] = (uint32_t)k;
        else if (count < MAX_ACQ) { first[count] = last[count] = (uint32_t)k; ++count; }
    }
    ++w->windows;
    w->samples[bit] += (uint32_t)n;
    size_t a = 0;
    for (size_t k = 0; k < n; ++k) {
        /* a: first acquisition whose guarded end is not yet behind k. */
        while (a < count && k > last[a] + AGC_WITNESS_SETTLE + AGC_WITNESS_GUARD) ++a;
        unsigned v = (s[k] >> 7) & 1u;
        unsigned gain = s[k] & 127u;
        /* Settle and guard zones around an acquisition belong to neither
         * class: the flag edge may legitimately lead or lag there. */
        int in_acq = a < count && k >= first[a] && k <= last[a];
        int near = a < count && k + AGC_WITNESS_GUARD >= first[a];
        if (in_acq) {
            ++w->acq[bit]; w->ones_acq[bit] += v;
            if (bit == 0u) { ++w->acq_samples; if (gain < w->gain_min_acq) w->gain_min_acq = (uint8_t)gain; }
        } else if (!near) {
            ++w->trapped[bit]; w->ones_trapped[bit] += v;
            if (bit == 0u) {
                if (gain < w->trapped_min) w->trapped_min = (uint8_t)gain;
                if (gain > w->trapped_max) w->trapped_max = (uint8_t)gain;
            }
        }
    }
    if (bit == 0u) w->acquisitions += count;
    /* Edges of the active flag for both polarities around each acquisition. */
    for (unsigned i = 0; i < count; ++i) {
        if (first[i] < AGC_WITNESS_EDGE || last[i] + AGC_WITNESS_EDGE >= n) continue;
        for (unsigned pol = 0; pol < 2u; ++pol) {
            /* Active = (v == 1) for pol 0, (v == 0) for pol 1 (inverted). */
            size_t rise = first[i];
            while (rise > first[i] - AGC_WITNESS_EDGE &&
                   ((((s[rise - 1] >> 7) & 1u) ^ pol) == 1u)) --rise;
            size_t fall = last[i];
            while (fall < last[i] + AGC_WITNESS_EDGE &&
                   ((((s[fall + 1] >> 7) & 1u) ^ pol) == 1u)) ++fall;
            if ((((s[first[i]] >> 7) & 1u) ^ pol) != 1u) continue; /* not active at start */
            w->lead_sum[bit][pol] += (int32_t)(first[i] - rise);
            w->lag_sum[bit][pol] += (int32_t)(fall - last[i]);
            ++w->edge_count[bit][pol];
        }
    }
}

/* Safety first: a flag active on trapped samples would freeze good picture,
 * so only a bit/polarity active on <= 5 % of them qualifies; among those the
 * one covering most acquisition samples wins. Without a qualifying flag the
 * best-separating one is reported (refused). On the C5 the widest-separating
 * flag (DIAG[31] inverted: 95 % of walks, 20 % of trapped) is unusable while
 * DIAG[30] (58 % of walks, 0 % trapped) is the clean one (board, 2026-10-06),
 * so separation alone picked the wrong bit. Every concealed walk sample is a
 * gain; > 50 % coverage is required so a sparse coincidental bit is not used. */
#define AGC_WITNESS_MAX_TRAPPED_PM 50u
#define AGC_WITNESS_MIN_COVER_PM 500u
static inline int agc_witness_choose(const agc_witness_t *w, agc_witness_result_t *r)
{
    *r = (agc_witness_result_t){.bit = -1};
    unsigned best = 0, best_cover = 0;
    int safe = 0;
    for (unsigned bit = 0; bit < 4u; ++bit) {
        if (!w->acq[bit] || !w->trapped[bit]) continue;
        unsigned pa = (unsigned)((uint64_t)w->ones_acq[bit] * 1000u / w->acq[bit]);
        unsigned pt = (unsigned)((uint64_t)w->ones_trapped[bit] * 1000u / w->trapped[bit]);
        for (int invert = 0; invert < 2; ++invert) {
            unsigned cover = invert ? 1000u - pa : pa;
            unsigned leak = invert ? 1000u - pt : pt;
            if (cover <= leak) continue;
            unsigned sep = cover - leak;
            int is_safe = leak <= AGC_WITNESS_MAX_TRAPPED_PM;
            if (is_safe ? (!safe || cover > best_cover) : (!safe && sep > best)) {
                safe |= is_safe;
                best = sep;
                best_cover = cover;
                r->bit = (int)bit;
                r->invert = invert;
                r->separation_pm = sep;
            }
        }
    }
    if (w->samples[0]) {
        r->acq_per_ms_x10 = (unsigned)((uint64_t)w->acquisitions * 10u * AGC_WITNESS_RATE_HZ /
                                       1000u / w->samples[0]);
        r->acq_share_pm = (unsigned)((uint64_t)w->acq_samples * 1000u / w->samples[0]);
    }
    if (w->acquisitions)
        r->acq_us_x10 = (unsigned)((uint64_t)w->acq_samples * 10u * 1000000u /
                                   AGC_WITNESS_RATE_HZ / w->acquisitions);
    r->gain_min_acq = w->gain_min_acq;
    r->trapped_min = w->trapped_min;
    r->trapped_max = w->trapped_max;
    if (r->bit >= 0) {
        unsigned pa = (unsigned)((uint64_t)w->ones_acq[r->bit] * 1000u / w->acq[r->bit]);
        unsigned pt = (unsigned)((uint64_t)w->ones_trapped[r->bit] * 1000u / w->trapped[r->bit]);
        r->active_acq_pm = r->invert ? 1000u - pa : pa;
        r->active_trapped_pm = r->invert ? 1000u - pt : pt;
        unsigned pol = (unsigned)r->invert;
        uint32_t c = w->edge_count[r->bit][pol];
        if (c) {
            r->lead_samples = (unsigned)(w->lead_sum[r->bit][pol] / (int32_t)c);
            r->lag_samples = (unsigned)(w->lag_sum[r->bit][pol] / (int32_t)c);
        }
    }
    return r->bit >= 0 && safe && w->acquisitions >= 8u &&
           r->active_trapped_pm <= AGC_WITNESS_MAX_TRAPPED_PM &&
           r->active_acq_pm >= AGC_WITNESS_MIN_COVER_PM;
}
