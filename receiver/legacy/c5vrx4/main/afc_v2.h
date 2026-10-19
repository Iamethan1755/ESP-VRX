#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

/*
 * Issue #115 AFC V2 reference measurement (measurement only, no control).
 *
 * The old estimator averaged the WBFM phase slope over arbitrary picture
 * content, so video modulation leaked into "carrier offset". This one only
 * measures instantaneous frequency inside intervals whose CVBS level is fixed
 * by the video standard:
 *
 *   sync tip   : sync_start + 0.5 us .. sync_end - 0.5 us
 *   back porch : sync_end + 3.4 us .. sync_end + 4.4 us
 *
 * The back-porch window starts after the colour burst for both standards
 * (NTSC burst ends ~3.1 us after sync end, PAL ~3.15 us) and ends before
 * active video (NTSC porch ends at 4.7 us, PAL at 5.7 us).
 *
 * Sync is found on the demodulated frequency itself, independent of FM
 * polarity: a 3.5-5.5 us plateau at one extreme (inner 15 % of the range),
 * bracketed by front and back porch at one common level. Vertical-interval
 * half-width and broad pulses fail the width test. A white picture block of
 * sync-like width can pass those tests by chance, so every candidate is also
 * scored on the colour burst that only a real sync carries: the frequency
 * signal 1.0-3.0 us after sync end is correlated against the NTSC and PAL
 * subcarriers. The polarity with the stronger burst-bearing evidence wins, and
 * the stronger subcarrier reports the standard.
 *
 * Which of these levels corresponds to the true RF channel centre depends on
 * the VTX modulator and must be characterised on hardware before any
 * correction uses it (#115 section 9). This module only reports the levels.
 *
 * Inputs are raw Q4/I4 bytes at 40 MS/s and the 256-code Phase8 LUT; one
 * phase code between adjacent samples is 40 MHz / 256 = 156.25 kHz.
 */

#define AFC2_SAMPLE_RATE_KHZ   40000
#define AFC2_KHZ_PER_CODE_X100 15625  /* 156.25 kHz */
#define AFC2_MIN_POWER         2u     /* exclude the four origin cells */
#define AFC2_SMOOTH            26u    /* warm-up bound; group delay is 12 samples (0.3 us) */
#define AFC2_SYNC_MIN          140u   /* 3.5 us */
#define AFC2_SYNC_MAX          220u   /* 5.5 us */
#define AFC2_TIP_GUARD         20u    /* 0.5 us */
#define AFC2_PORCH_START       136u   /* 3.4 us after sync end */
#define AFC2_PORCH_LEN         40u    /* 1.0 us */
#define AFC2_MAX_SAMPLES       4096u
#define AFC2_BURST_START       40u    /* 1.0 us after sync end */
#define AFC2_BURST_LEN         80u    /* 2.0 us, inside NTSC and PAL burst */
#define AFC2_BURST_MIN_X10     25     /* 2.5 codes (~390 kHz) burst amplitude */

typedef struct {
    uint16_t lines;          /* sync pulses whose windows were measured */
    int8_t polarity;         /* -1: sync is the low-frequency extreme, +1 high, 0 none */
    int32_t sync_khz;        /* mean instantaneous frequency of the sync tips */
    int32_t porch_khz;       /* mean instantaneous frequency, burst-free porch */
    uint32_t sync_pairs, porch_pairs;
    uint8_t standard;        /* 0 unknown / no burst, 1 NTSC, 2 PAL */
    uint16_t burst_x10;      /* mean burst amplitude in phase codes x10 */
    uint16_t period_40m;     /* measured accepted-run spacing; 0 without two agreeing runs */
    uint16_t width_40m;      /* mean accepted sync width, smoothed detector */
} afc2_result_t;

/* Burst amplitude (codes x10) at one subcarrier over the burst window, using
 * only valid sample pairs. `standard` 1 = NTSC 3.579545 MHz, 2 = PAL. */
static inline int afc2_burst_x10(const uint8_t *s, size_t n, size_t end,
                                 const uint8_t lut[256], int standard);

static inline unsigned afc2_power(uint8_t raw)
{
    int i = (int)(int8_t)(raw & 0xF0u) >> 4;
    int q = (int)(int8_t)((raw & 0x0Fu) << 4) >> 4;
    int ci = 2 * i + 1, cq = 2 * q + 1;
    return (unsigned)(ci * ci + cq * cq + 2) / 4u;
}

/* Conservative IQ stationarity gate for AFC only. It rejects large changes
 * in block-averaged Q4 power; it does NOT decode the native gain state or
 * identify their cause. Whole 32-sample blocks reduce cell-rotation artifacts. */
static inline bool afc2_envelope_stationary(const uint8_t *s, size_t n)
{
    if (!s || n < 64u) return false;
    unsigned prev = 0;
    for (size_t k = 0; k + 32u <= n; k += 32u) {
        unsigned sum = 0;
        for (size_t j = k; j < k + 32u; ++j) sum += afc2_power(s[j]);
        if (sum < 64u) return false;
        if (prev && (sum * 2u > prev * 5u || prev * 2u > sum * 5u)) return false;
        prev = sum;
    }
    return true;
}

/* Signed phase step in LUT codes, or INT16_MIN when either endpoint is too
 * close to the origin to carry phase. */
static inline int afc2_delta(uint8_t a, uint8_t b, const uint8_t lut[256])
{
    if (afc2_power(a) < AFC2_MIN_POWER || afc2_power(b) < AFC2_MIN_POWER)
        return INT16_MIN;
    return (((int)lut[b] - (int)lut[a] + 128) & 255) - 128;
}

static inline int afc2_codes_to_khz(int64_t sum_codes, uint32_t n)
{
    if (!n) return 0;
    return (int)(sum_codes * AFC2_KHZ_PER_CODE_X100 / 100 / (int64_t)n);
}

/* Smoothed frequency is kept only in a 512-entry ring (sample index & 511):
 * every lookup used below lies within 264 samples of the newest value, so no
 * whole-window array is needed. Values are phase codes x2, clamped to int8. */
#define AFC2_RING 512u
#define AFC2_AT(ring, i) ((int32_t)(ring)[(i) & (AFC2_RING - 1u)])

/* Two cascaded boxcars. Analog FPV transmitters (RTC6705 class) add FM audio
 * subcarriers at 6.0 and 6.5 MHz (-25..-30 dBc, ~0.4-0.7 MHz peak carrier
 * deviation each) that no filter removes before this point. 20 samples are
 * exactly 3 periods of 6.0 MHz (null) and 6 samples are 0.975 periods of
 * 6.5 MHz (-31 dB); their group delay is (19 + 5)/2 = 12 samples (0.3 us). */
#define AFC2_BOX1 20u
#define AFC2_BOX2 6u
#define AFC2_DELAY ((AFC2_BOX1 + AFC2_BOX2 - 2u) / 2u)

typedef struct {
    int32_t sum;
    unsigned count;
    int16_t delta[AFC2_BOX1];
    bool valid[AFC2_BOX1];
    int32_t sum2;
    int16_t stage1[AFC2_BOX2];
} afc2_smoother_t;

static inline int afc2_smooth_push(afc2_smoother_t *m, size_t k, int d)
{
    unsigned slot = (unsigned)(k % AFC2_BOX1);
    if (m->valid[slot]) { m->sum -= m->delta[slot]; --m->count; }
    m->valid[slot] = d != INT16_MIN;
    m->delta[slot] = (int16_t)(m->valid[slot] ? d : 0);
    if (m->valid[slot]) { m->sum += d; ++m->count; }
    int v1 = m->count ? (int)(m->sum * 2 / (int32_t)m->count) : 0;
    unsigned slot2 = (unsigned)(k % AFC2_BOX2);
    m->sum2 += v1 - m->stage1[slot2];
    m->stage1[slot2] = (int16_t)v1;
    int v = (int)(m->sum2 / (int32_t)AFC2_BOX2);
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

static inline bool afc2_in_sync_value(int32_t v, int pol, int32_t lo, int32_t hi)
{
    /* Blanking sits 40/140 of the range from the sync extreme with a white
     * picture and further away in darker scenes; 15 % stays inside the tip. */
    int32_t span = hi - lo;
    return pol < 0 ? v <= lo + span * 3 / 20 : v >= hi - span * 3 / 20;
}

/* A run that ended at k (exclusive, smoothed coordinates) is accepted when
 * its width is sync-like and it is bracketed by front and back porch at one
 * common (blanking) level, clearly separated from the sync level. This is
 * scene-independent. Needs ring values up to index k + 14. */
static inline bool afc2_sync_run_ok(const int8_t *ring, size_t n, size_t k,
                                    size_t len, int pol, int32_t lo, int32_t hi)
{
    if (len < AFC2_SYNC_MIN || len > AFC2_SYNC_MAX) return false;
    size_t start = k - len - AFC2_DELAY, end = k - AFC2_DELAY;
    /* Front porch ~0.45-1.0 us before sync; back porch ~0.07-0.65 us after
     * sync end, before the colour burst. */
    if (start < 30u + AFC2_SMOOTH || end + 14u + AFC2_DELAY >= n) return false;
    int32_t front = AFC2_AT(ring, start - 30u + AFC2_DELAY);
    int32_t back = AFC2_AT(ring, end + 14u + AFC2_DELAY);
    int32_t sync = AFC2_AT(ring, (start + end) / 2u + AFC2_DELAY);
    int32_t sep_f = pol < 0 ? front - sync : sync - front;
    int32_t sep_b = pol < 0 ? back - sync : sync - back;
    int32_t depth = (sep_f + sep_b) / 2;
    int32_t mismatch = front > back ? front - back : back - front;
    return sep_f > 0 && sep_b > 0 && depth * 8 >= hi - lo && mismatch * 3 <= depth;
}

static inline int afc2_burst_x10(const uint8_t *s, size_t n, size_t end,
                                 const uint8_t lut[256], int standard)
{
    /* Single-precision tables built once: the C5 FPU has no double support,
     * so per-sample double cos()/sin() would cost milliseconds per window. */
    static float tab_cos[2][AFC2_BURST_LEN], tab_sin[2][AFC2_BURST_LEN];
    static bool tab_ready;
    if (!tab_ready) {
        for (unsigned t = 0; t < 2u; ++t) {
            float fsc = t ? 4433618.75f : 3579545.0f;
            for (unsigned j = 0; j < AFC2_BURST_LEN; ++j) {
                float w = 6.2831853f * fsc * (float)j / 40e6f;
                tab_cos[t][j] = cosf(w);
                tab_sin[t][j] = sinf(w);
            }
        }
        tab_ready = true;
    }
    const unsigned t = standard == 2 ? 1u : 0u;
    size_t j0 = end + AFC2_BURST_START, j1 = j0 + AFC2_BURST_LEN;
    if (j1 + 1u >= n) return 0;
    float si = 0.0f, sq = 0.0f;
    unsigned used = 0;
    for (size_t j = j0; j < j1; ++j) {
        int d = afc2_delta(s[j], s[j + 1], lut);
        if (d == INT16_MIN) continue;
        si += (float)d * tab_cos[t][j - j0];
        sq += (float)d * tab_sin[t][j - j0];
        ++used;
    }
    if (used < AFC2_BURST_LEN / 2u) return 0;
    return (int)(20.0f * sqrtf(si * si + sq * sq) / (float)used);
}

/* Evidence for one accepted run: 1 point, +4 when a colour burst follows. */
static inline unsigned afc2_run_score(const uint8_t *s, size_t n, size_t end,
                                      const uint8_t lut[256], int *best_std,
                                      int *best_amp)
{
    int ntsc = afc2_burst_x10(s, n, end, lut, 1);
    int pal = afc2_burst_x10(s, n, end, lut, 2);
    *best_std = pal > ntsc ? 2 : 1;
    *best_amp = pal > ntsc ? pal : ntsc;
    return *best_amp >= AFC2_BURST_MIN_X10 ? 5u : 0u;
}

#define AFC2_MAX_RUNS 8u

static inline afc2_result_t afc2_measure(const uint8_t *s, size_t n,
                                         const uint8_t lut[256])
{
    afc2_result_t r = {0};
    if (!s || n < AFC2_SMOOTH + AFC2_SYNC_MAX + AFC2_PORCH_START + AFC2_PORCH_LEN ||
        n > AFC2_MAX_SAMPLES)
        return r;

    /* Pass 1: range of the smoothed frequency. */
    afc2_smoother_t m = {0};
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (size_t k = 1; k < n; ++k) {
        int v = afc2_smooth_push(&m, k, afc2_delta(s[k - 1], s[k], lut));
        if (k >= AFC2_SMOOTH && m.count * 2u >= AFC2_BOX1) {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo >= hi) return r;

    /* Pass 2: identical smoothing into a ring; both polarities' sync runs are
     * tracked at once and judged 14 samples after they end. */
    static int8_t ring[AFC2_RING];
    afc2_smoother_t m2 = {0};
    size_t run_len[2] = {0, 0}, pend_k[2] = {0, 0}, pend_len[2] = {0, 0};
    size_t acc_end[2][AFC2_MAX_RUNS], acc_len[2][AFC2_MAX_RUNS];
    unsigned acc_n[2] = {0, 0}, score[2] = {0, 0};
    for (size_t k = 1; k <= n + 14u; ++k) {
        if (k < n) {
            ring[k & (AFC2_RING - 1u)] =
                (int8_t)afc2_smooth_push(&m2, k, afc2_delta(s[k - 1], s[k], lut));
        }
        for (unsigned p = 0; p < 2u; ++p) {
            int pol = p ? 1 : -1;
            if (pend_len[p] && k >= pend_k[p] + 14u) {
                if (afc2_sync_run_ok(ring, n, pend_k[p], pend_len[p], pol, lo, hi)) {
                    int std_id, amp;
                    size_t end = pend_k[p] - AFC2_DELAY;
                    unsigned evidence = afc2_run_score(s, n, end, lut, &std_id, &amp);
                    score[p] += evidence;
                    if (evidence && acc_n[p] < AFC2_MAX_RUNS) {
                        acc_end[p][acc_n[p]] = end;
                        acc_len[p][acc_n[p]] = pend_len[p];
                        ++acc_n[p];
                    }
                }
                pend_len[p] = 0;
            }
            if (k < AFC2_SMOOTH || k > n) continue;
            if (k < n && afc2_in_sync_value(AFC2_AT(ring, k), pol, lo, hi)) {
                ++run_len[p];
            } else {
                if (run_len[p]) { pend_k[p] = k; pend_len[p] = run_len[p]; }
                run_len[p] = 0;
            }
        }
    }
    int best = score[0] > score[1] ? 0 : score[1] > score[0] ? 1 : -1;
    if (best < 0 || !score[best]) return r;
    r.polarity = (int8_t)(best ? 1 : -1);

    int64_t sync_sum = 0, porch_sum = 0;
    int burst_sum = 0, burst_n = 0, std_votes = 0;
    size_t last_start = 0;
    int last_std = 0;
    unsigned width_sum = 0, period_sum = 0, periods = 0;
    for (unsigned a = 0; a < acc_n[best]; ++a) {
        /* The boxcar delays edges by about half its length. */
        size_t end = acc_end[best][a], start = end - acc_len[best][a];
        size_t p0 = end + AFC2_PORCH_START, p1 = p0 + AFC2_PORCH_LEN;
        int std_id, amp;
        (void)afc2_run_score(s, n, end, lut, &std_id, &amp);
        if (amp < AFC2_BURST_MIN_X10 || p1 >= n) continue;
        burst_sum += amp;
        ++burst_n;
        std_votes += std_id == 2 ? 1 : -1;
        uint32_t old_sync_pairs = r.sync_pairs, old_porch_pairs = r.porch_pairs;
        int64_t old_sync_sum = sync_sum, old_porch_sum = porch_sum;
        for (size_t j = start + AFC2_TIP_GUARD; j < end - AFC2_TIP_GUARD; ++j) {
            int d = afc2_delta(s[j], s[j + 1], lut);
            if (d != INT16_MIN) { sync_sum += d; ++r.sync_pairs; }
        }
        for (size_t j = p0; j < p1; ++j) {
            int d = afc2_delta(s[j], s[j + 1], lut);
            if (d != INT16_MIN) { porch_sum += d; ++r.porch_pairs; }
        }
        if ((r.sync_pairs - old_sync_pairs) * 4u < (end - start - 2u * AFC2_TIP_GUARD) * 3u ||
            (r.porch_pairs - old_porch_pairs) * 4u < AFC2_PORCH_LEN * 3u) {
            r.sync_pairs = old_sync_pairs; r.porch_pairs = old_porch_pairs;
            sync_sum = old_sync_sum; porch_sum = old_porch_sum;
            --burst_n; burst_sum -= amp; std_votes -= std_id == 2 ? 1 : -1;
            continue;
        }
        width_sum += (unsigned)(end - start);
        if (r.lines && last_std == std_id) {
            size_t period = start - last_start;
            bool timing_ok = std_id == 2 ? period >= 2554u && period <= 2568u :
                                           period >= 2532u && period <= 2550u;
            if (timing_ok) { period_sum += (unsigned)period; ++periods; }
        }
        last_start = start;
        last_std = std_id;
        ++r.lines;
    }
    r.width_40m = r.lines ? (uint16_t)(width_sum / r.lines) : 0;
    r.period_40m = periods ? (uint16_t)(period_sum / periods) : 0;
    if (burst_n) {
        r.burst_x10 = (uint16_t)(burst_sum / burst_n);
        r.standard = std_votes > 0 ? 2u : std_votes < 0 ? 1u : 0u;
    }
    r.sync_khz = afc2_codes_to_khz(sync_sum, r.sync_pairs);
    r.porch_khz = afc2_codes_to_khz(porch_sum, r.porch_pairs);
    return r;
}
