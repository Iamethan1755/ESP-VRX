/* C5VRX by Twotoz and contributors: pre-demodulation evidence helpers.
 * Pure functions over completed raw Q4/I4 bytes (I high nibble, Q low nibble,
 * signed). Observers only: nothing here touches the 40 MS/s path. */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

static inline int predemod_i(uint8_t b) { return (int8_t)(b & 0xf0u) >> 4; }
static inline int predemod_q(uint8_t b) { return (int8_t)(uint8_t)(b << 4) >> 4; }
static inline int predemod_abs(int v) { return v < 0 ? -v : v; }

/* One axis of sample n is a glitch when it jumps at least `limit` cells away
 * from both neighbours while those neighbours agree within limit / 2. A read
 * that lands on a MODEM_DIAG transition mixes old and new bits; at a zero
 * crossing that turns -1/0 into -8/+7 (zerowidth PR #3, Logicenios link
 * monitor). A real carrier rotates far less in 25 ns, so the rate compares
 * sampling-phase positions; it is not an absolute error rate. */
static inline unsigned predemod_glitches(const uint8_t *s, size_t n, int limit)
{
    unsigned count = 0;
    for (size_t k = 1; k + 1 < n; ++k) {
        int a[3] = {predemod_i(s[k - 1]), predemod_i(s[k]), predemod_i(s[k + 1])};
        int b[3] = {predemod_q(s[k - 1]), predemod_q(s[k]), predemod_q(s[k + 1])};
        for (unsigned axis = 0; axis < 2; ++axis) {
            const int *x = axis ? b : a;
            if (predemod_abs(x[1] - x[0]) >= limit &&
                predemod_abs(x[1] - x[2]) >= limit &&
                predemod_abs(x[0] - x[2]) <= limit / 2) { ++count; break; }
        }
    }
    return count;
}

/* Cell-centre mean (2v+1)/2 per axis, in milli-cells of the current lane. */
static inline void predemod_dc_mcells(const uint8_t *s, size_t n, int *i, int *q)
{
    int32_t si = 0, sq = 0;
    for (size_t k = 0; k < n; ++k) {
        si += 2 * predemod_i(s[k]) + 1;
        sq += 2 * predemod_q(s[k]) + 1;
    }
    *i = n ? (int)(si * 500 / (int32_t)n) : 0;
    *q = n ? (int)(sq * 500 / (int32_t)n) : 0;
}

/* RX DC calibration point used by the pinned libphy for a 5 GHz channel.
 * phy_set_rx_gain_cal_dc() calibrates these seven frequencies when
 * phy_param[0x2a] != 0, otherwise only 2432 MHz (disassembly, IDF 6.0.2
 * esp-phy-lib 59c1234). FPV channels above 5855 MHz use the 5855 point. */
static inline uint16_t predemod_dc_cal_point(uint16_t mhz, int multi_point)
{
    static const uint16_t points[7] = {5210, 5290, 5530, 5610, 5690, 5775, 5855};
    if (!multi_point) return 2432u;
    uint16_t best = points[0];
    for (unsigned k = 1; k < 7u; ++k)
        if (predemod_abs((int)mhz - (int)points[k]) < predemod_abs((int)mhz - (int)best))
            best = points[k];
    return best;
}

/* Relative RC filter-capacitor step for BBTOP 0x67 registers 6..13: keep the
 * per-chip calibrated value as the base, add an offset in the 6-bit field and
 * saturate at 60, the code phy_11p_set() writes. Upper bits are preserved. */
static inline uint8_t predemod_filter_code(uint8_t calibrated, int offset)
{
    int code = (calibrated & 63) + offset;
    if (code < 0) code = 0;
    if (code > 60) code = 60;
    return (uint8_t)((calibrated & ~63u) | (unsigned)code);
}

/* Closed-loop DC step for two DC DACs with a measured 2x2 response matrix
 * (milli-cells per DAC code). Returns 0 when the matrix is ill-conditioned.
 * Steps are bounded by `limit` codes per axis. */
static inline int predemod_dco_step(const float j[4], float di, float dq,
                                    int limit, int *step_a, int *step_b)
{
    float det = j[0] * j[3] - j[1] * j[2];
    if (det > -1e-3f && det < 1e-3f) return 0;
    float a = (-j[3] * di + j[1] * dq) / det;
    float b = (j[2] * di - j[0] * dq) / det;
    int sa = (int)(a < 0 ? a - 0.5f : a + 0.5f);
    int sb = (int)(b < 0 ? b - 0.5f : b + 0.5f);
    if (sa > limit) sa = limit;
    if (sa < -limit) sa = -limit;
    if (sb > limit) sb = limit;
    if (sb < -limit) sb = -limit;
    *step_a = sa; *step_b = sb;
    return 1;
}

/* Static Phase8 decoder word for one raw IQ byte with the I/Q centre moved
 * by (di, dq) milli-cells of the current lane. Identical to
 * generate_phase8.py at (0, 0): cell centre +31.5/64, half-even rounding.
 * Odd LUT banks hold ((128 + p) & 255) | ((-p & 255) << 8). Digital
 * recentring only moves the decode geometry; it cannot restore samples that
 * folded or clipped before Q4. */
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static inline uint8_t predemod_phase8(uint8_t raw, int di, int dq)
{
    double i = predemod_i(raw) + 31.5 / 64 - di / 1000.0;
    double q = predemod_q(raw) + 31.5 / 64 - dq / 1000.0;
    return (uint8_t)((int)rint(atan2(q, i) * 128 / M_PI) & 255);
}
static inline uint16_t predemod_decoder_word(uint8_t raw, int di, int dq)
{
    uint8_t p = predemod_phase8(raw, di, dq);
    return (uint16_t)(((128u + p) & 255u) | (((256u - p) & 255u) << 8));
}

/* Decision for one evaluation of the measured centre (milli-cells). A new
 * centre is applied only after two consecutive evaluations agree within
 * `agree` and it differs from the applied one by at least `step` on an axis;
 * |centre| is clamped to `limit`. Returns 1 and writes *out when applying. */
typedef struct { int last[2]; int have_last; } predemod_dc_filter_t;
static inline int predemod_dc_decide(predemod_dc_filter_t *f, const int measured[2],
                                     const int applied[2], int agree, int step,
                                     int limit, int out[2])
{
    int m[2];
    for (unsigned a = 0; a < 2; ++a)
        m[a] = measured[a] > limit ? limit : measured[a] < -limit ? -limit : measured[a];
    int stable = f->have_last && predemod_abs(m[0] - f->last[0]) <= agree &&
                 predemod_abs(m[1] - f->last[1]) <= agree;
    int target[2] = {(m[0] + f->last[0]) / 2, (m[1] + f->last[1]) / 2};
    f->last[0] = m[0]; f->last[1] = m[1]; f->have_last = 1;
    if (!stable) return 0;
    if (predemod_abs(target[0]) < step && predemod_abs(target[1]) < step) {
        target[0] = target[1] = 0; /* deadband: stay on the pristine table */
    }
    if (target[0] == applied[0] && target[1] == applied[1]) return 0;
    if (predemod_abs(target[0] - applied[0]) < step &&
        predemod_abs(target[1] - applied[1]) < step &&
        (target[0] || target[1])) return 0;
    out[0] = target[0]; out[1] = target[1];
    return 1;
}

/* ---- Fixed analog bandwidth calibration ---------------------------------
 * The RC filter capacitor codes are calibrated per chip, so "code X = N MHz"
 * is not portable. Measure instead: at maximum gain without a carrier the
 * receiver noise is shaped by the analog filter. A 64-point complex FFT of
 * 40 MS/s Q4/I4 regions (625 kHz bins, +-20 MHz span) is averaged and the
 * full -3 dB width estimated. Q4 quantization adds a flat floor ~12 dB down
 * at the measured noise level, below the -3 dB threshold. */
#define PREDEMOD_FFT_N 64
#define PREDEMOD_BIN_KHZ 625
static inline void predemod_fft64(float re[PREDEMOD_FFT_N], float im[PREDEMOD_FFT_N])
{
    for (unsigned i = 1, j = 0; i < PREDEMOD_FFT_N; ++i) {
        unsigned bit = PREDEMOD_FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= PREDEMOD_FFT_N; len <<= 1) {
        double ang = -2 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (unsigned i = 0; i < PREDEMOD_FFT_N; i += len) {
            float cr = 1, ci = 0;
            for (unsigned k = 0; k < len / 2; ++k) {
                unsigned a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

/* Add one region's Hann-windowed, mean-removed power spectrum to psd[],
 * stored centred: psd[32] is DC, psd[0] is -20 MHz. */
static inline void predemod_psd_accumulate(const uint8_t *s, float psd[PREDEMOD_FFT_N])
{
    float re[PREDEMOD_FFT_N], im[PREDEMOD_FFT_N], mi = 0, mq = 0;
    for (unsigned k = 0; k < PREDEMOD_FFT_N; ++k) {
        re[k] = predemod_i(s[k]) + 0.5f; im[k] = predemod_q(s[k]) + 0.5f;
        mi += re[k]; mq += im[k];
    }
    mi /= PREDEMOD_FFT_N; mq /= PREDEMOD_FFT_N;
    static float hann[PREDEMOD_FFT_N];
    if (hann[1] == 0.f) /* hann[0] is 0; the others are computed once */
        for (unsigned k = 0; k < PREDEMOD_FFT_N; ++k)
            hann[k] = (float)(0.5 - 0.5 * cos(2 * M_PI * k / PREDEMOD_FFT_N));
    for (unsigned k = 0; k < PREDEMOD_FFT_N; ++k) {
        re[k] = (re[k] - mi) * hann[k]; im[k] = (im[k] - mq) * hann[k];
    }
    predemod_fft64(re, im);
    for (unsigned k = 0; k < PREDEMOD_FFT_N; ++k)
        psd[(k + PREDEMOD_FFT_N / 2) % PREDEMOD_FFT_N] += re[k] * re[k] + im[k] * im[k];
}

/* Full -3 dB width in kHz of a centred PSD: reference is the median of the
 * bins 1.25..5 MHz from DC (DC and its neighbour excluded), each side ends
 * after two consecutive bins below half of it. 40000 means wider than the
 * 40 MS/s span; 0 means no usable reference. */
static inline float predemod_psd_ref(const float psd[PREDEMOD_FFT_N])
{
    const unsigned c = PREDEMOD_FFT_N / 2;
    float ref[14];
    unsigned n = 0;
    for (unsigned d = 2; d <= 8; ++d) { ref[n++] = psd[c - d]; ref[n++] = psd[c + d]; }
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && ref[j - 1] > ref[j]; --j) {
            float t = ref[j]; ref[j] = ref[j - 1]; ref[j - 1] = t;
        }
    return (ref[n / 2 - 1] + ref[n / 2]) / 2;
}

static inline unsigned predemod_psd_width_khz(const float psd[PREDEMOD_FFT_N])
{
    const unsigned c = PREDEMOD_FFT_N / 2;
    float half = predemod_psd_ref(psd) / 2;
    if (!(half > 0)) return 0;
    unsigned edge[2];
    for (unsigned side = 0; side < 2; ++side) {
        unsigned last = 1;
        for (unsigned d = 2; d < c; ++d) {
            unsigned k = side ? c + d : c - d;
            unsigned k2 = side ? (d + 1 < c ? c + d + 1 : k) : c - d - 1;
            if (psd[k] < half && psd[k2] < half) break;
            last = d;
        }
        edge[side] = last;
    }
    if (edge[0] >= c - 2 && edge[1] >= c - 2) return 40000u;
    return (edge[0] + edge[1] + 1) * PREDEMOD_BIN_KHZ;
}

/* Equivalent noise bandwidth in kHz of a centred PSD: total power divided by
 * the in-band reference density (the same 1.25..5 MHz median as the width),
 * DC bin replaced by the mean of its neighbours. PARLIO keeps every second
 * sample of the ~80 MS/s MODEM_DIAG bus with no filter in between, so noise
 * from 20..40 MHz is already folded into this view: the result is the
 * pre-detection noise bandwidth the discriminator actually sees, alias
 * included (a brick wall of width W reads W; flat noise reads 40000). The
 * Q4 quantization floor adds a roughly constant bias. 0: no reference. */
static inline unsigned predemod_psd_nbw_khz(const float psd[PREDEMOD_FFT_N])
{
    const unsigned c = PREDEMOD_FFT_N / 2;
    float ref = predemod_psd_ref(psd);
    if (!(ref > 0)) return 0;
    double sum = 0.5 * ((double)psd[c - 1] + psd[c + 1]);
    for (unsigned k = 0; k < PREDEMOD_FFT_N; ++k)
        if (k != c) sum += psd[k];
    double khz = sum / ref * PREDEMOD_BIN_KHZ;
    return khz > 4e6 ? 4000000u : (unsigned)(khz + 0.5);
}

/* Two-stage filter choice by noise bandwidth: entry k is one stage-2 setting
 * (entry 0 the current single-stage choice) with stage 1 re-opened until the
 * -3 dB width covers the target again. Only valid entries (width >= target,
 * noise still incoherent) compete; another entry must lower the noise
 * bandwidth by >= 7 % (0.3 dB) or entry 0 is kept. Returns the index, or -1
 * when entry 0 itself is invalid. */
/* Width the second stage must keep. When even the widest code is narrower
 * than the target (first board, 2026-10-06: 19.4 MHz at every RX0 code; the
 * tap sits ahead of the digital filter), the target cannot be met at all, so
 * the stage may cost at most 7 % of the width the chip actually delivers
 * (operator decision 2026-10-06: there skirt 8 keeps 18.75 MHz and lowers
 * the noise bandwidth 22.2 -> 19.3 MHz, 0.6 dB). */
static inline unsigned predemod_skirt_target_khz(unsigned target_khz, unsigned widest_khz)
{
    return widest_khz >= target_khz ? target_khz : widest_khz * 93u / 100u;
}

static inline int predemod_skirt_choose(const unsigned *nbw_khz, const unsigned *width_khz,
                                        const bool *quiet, unsigned count, unsigned target_khz)
{
    if (!count || !quiet[0] || !nbw_khz[0] || width_khz[0] < target_khz) return -1;
    unsigned best = 0;
    for (unsigned k = 1; k < count; ++k)
        if (quiet[k] && nbw_khz[k] && width_khz[k] >= target_khz && nbw_khz[k] < nbw_khz[best])
            best = k;
    return (uint64_t)nbw_khz[best] * 100u <= (uint64_t)nbw_khz[0] * 93u ? (int)best : 0;
}

/* Edge profile (2026-10-05). At the range edge the noise bandwidth costs
 * twice: pre-detection CNR (FM threshold) and post-detection aliasing, because
 * span75 resamples the endpoint delta at 13.33 MS/s with no anti-alias filter
 * and FM noise grows as f^2 up to the pre-detection half-width. Host model
 * (tools/postdetect_alias_model.py, 3rd-order filter, generated LUTs): a
 * 14 MHz -3 dB width beats 24 MHz by ~2 dB of input at the edge, with >= 28
 * dB nonlinear SDR. Candidates are measured settings (digital filter x analog
 * code, stored skirt kept); the lowest noise bandwidth whose width still
 * covers edge_target, whose noise stays incoherent for V5 NO_CARRIER and that
 * beats the normal setting by >= 7 % (0.3 dB, the second-stage rule; it was
 * 0.5 dB until the normal profile itself used the second stage, 2026-10-06)
 * wins. -1: no edge gear. */
#define PREDEMOD_EDGE_TARGET_KHZ 14000u
static inline int predemod_edge_choose(const unsigned *nbw_khz, const unsigned *width_khz,
                                       const bool *valid, unsigned count,
                                       unsigned edge_target_khz, unsigned normal_nbw_khz)
{
    int best = -1;
    if (!normal_nbw_khz) return -1;
    for (unsigned k = 0; k < count; ++k)
        if (valid[k] && nbw_khz[k] && width_khz[k] >= edge_target_khz &&
            (best < 0 || nbw_khz[k] < nbw_khz[best]))
            best = (int)k;
    if (best < 0) return -1;
    return (uint64_t)nbw_khz[best] * 100u <= (uint64_t)normal_nbw_khz * 93u ? best : -1;
}

/* Noise-bandwidth excess over the -3 dB width, in 0.1 dB. */
static inline int predemod_nbw_excess_db_x10(unsigned nbw_khz, unsigned width_khz)
{
    if (!nbw_khz || !width_khz) return 0;
    return (int)lround(100.0 * log10((double)nbw_khz / (double)width_khz));
}

/* Narrowest filter setting whose measured width still covers the target.
 * widths[] follow the swept codes in increasing (narrowing) order; invalid
 * (0) entries are skipped. Returns the chosen index, or -1 when even the
 * widest setting is narrower than the target (the caller keeps the widest). */
static inline int predemod_bw_choose(const unsigned *widths_khz, unsigned count,
                                     unsigned target_khz)
{
    int best = -1;
    for (unsigned k = 0; k < count; ++k)
        if (widths_khz[k] && widths_khz[k] >= target_khz) best = (int)k;
    return best;
}

/* Reference only: ESPARGOS esp-sdr (GPL-3.0), main/common/rx_bandwidth.h at
 * commit ac627b0b, C5 BBTOP 0x67 regs 6/7 absolute code -> approximate full
 * noise width, median noise FFTs at 2300/5500 MHz, 80 MS/s IQ10. Mode 0 is
 * PHY channel mode 0 (11-23 MHz); mode 1 the wide path (22-48 MHz). Measured
 * on their board; C5VRX measures its own chip and uses this to identify the
 * mode and to report the expected width. */
static inline unsigned predemod_bw_reference_khz(unsigned mode, unsigned code)
{
    static const uint8_t code0[] = {0, 4, 8, 12, 16, 24, 32, 40, 48, 60};
    static const uint8_t mhz0[] = {23, 22, 21, 20, 18, 16, 15, 13, 12, 11};
    static const uint8_t code1[] = {0, 4, 8, 12, 16, 24, 32, 40, 48, 56, 60};
    static const uint8_t mhz1[] = {48, 45, 42, 40, 37, 34, 30, 27, 25, 23, 22};
    const uint8_t *c = mode ? code1 : code0, *m = mode ? mhz1 : mhz0;
    unsigned n = mode ? sizeof(code1) : sizeof(code0);
    if (code >= c[n - 1]) return m[n - 1] * 1000u;
    for (unsigned k = 1; k < n; ++k)
        if (code <= c[k]) {
            unsigned span = c[k] - c[k - 1];
            return (m[k - 1] * 1000u * (c[k] - code) + m[k] * 1000u * (code - c[k - 1]) + span / 2) / span;
        }
    return m[0] * 1000u;
}

/* Which esp-sdr curve the measured widths follow: mean absolute error in kHz
 * per mode, with the reference capped at the 40 MHz our 40-MS/s view can
 * show. Returns the better mode (0/1), or -1 without valid widths. */
static inline int predemod_bw_mode_fit(const uint8_t *codes, const unsigned *widths_khz,
                                       unsigned count, unsigned *error_khz)
{
    unsigned long err[2] = {0, 0};
    unsigned used = 0;
    for (unsigned k = 0; k < count; ++k) {
        if (!widths_khz[k]) continue;
        for (unsigned mode = 0; mode < 2; ++mode) {
            unsigned ref = predemod_bw_reference_khz(mode, codes[k]);
            if (ref > 40000u) ref = 40000u;
            err[mode] += (unsigned long)predemod_abs((int)widths_khz[k] - (int)ref);
        }
        ++used;
    }
    if (!used) return -1;
    int mode = err[1] < err[0] ? 1 : 0;
    if (error_khz) *error_khz = (unsigned)(err[mode] / used);
    return mode;
}
