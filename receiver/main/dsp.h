/* C5VRX-5 by Twotoz and the C5VRX contributors: pure helpers over raw
 * Q4/I4 bytes (I high nibble, Q low nibble, signed), from C5VRX-4
 * firmware/include/predemod.h. Observers only, never the 40 MS/s path. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int dsp_i(uint8_t b) { return (int8_t)(b & 0xf0u) >> 4; }
static inline int dsp_q(uint8_t b) { return (int8_t)(uint8_t)(b << 4) >> 4; }

/* Fourth-difference probe (zerowidth/C5VRX PR #3): a read that lands on a
 * MODEM_DIAG transition mixes old and new bits and spreads error evenly,
 * while the receive filter leaves little above ~10 MHz. */
static inline void dsp_hf4_sums(const uint8_t *s, size_t n, uint64_t *d4)
{
    static const int w[5] = {1, -4, 6, -4, 1};
    for (size_t k = 2; k + 2 < n; ++k) {
        int di = 0, dq = 0;
        for (int j = -2; j <= 2; ++j) {
            di += w[j + 2] * (2 * dsp_i(s[k + j]) + 1);
            dq += w[j + 2] * (2 * dsp_q(s[k + j]) + 1);
        }
        *d4 += (uint64_t)(di * di + dq * dq);
    }
}

/* Second-difference energy over the same samples as dsp_hf4_sums: the
 * reference for the level-independent HF4/HF2 whiteness (sphase_logic.h). */
static inline void dsp_hf2_sums(const uint8_t *s, size_t n, uint64_t *d2)
{
    for (size_t k = 2; k + 2 < n; ++k) {
        int di = (2 * dsp_i(s[k - 1]) + 1) - 2 * (2 * dsp_i(s[k]) + 1) + (2 * dsp_i(s[k + 1]) + 1);
        int dq = (2 * dsp_q(s[k - 1]) + 1) - 2 * (2 * dsp_q(s[k]) + 1) + (2 * dsp_q(s[k + 1]) + 1);
        *d2 += (uint64_t)(di * di + dq * dq);
    }
}

/* Excess fourth-difference energy, milli-cell^2 per sample, rounding share
 * removed. Board scans 2026-10-08, fixed ultrafine: clean positions
 * -70..+42, mid-change positions 391..3742. */
static inline int dsp_hf4_excess_milli(uint64_t d4, uint64_t count)
{
    if (!count) return 0;
    double excess = (double)d4 / (double)count / 70.0 / 4.0 - 2.0 / 12.0;
    return (int)(1000.0 * excess);
}

/* Receiver DC under a carrier: a constant-envelope FM carrier lies on a
 * circle around the true DC; the Kasa circle centre does not depend on
 * where the phase dwells (the plain mean does, up to ~0.3 cell). */
typedef struct { int64_t n, x, y, xx, yy, xy, z, zx, zy, zz; } dsp_circle_t;
static inline void dsp_circle_sums(const uint8_t *s, size_t n, dsp_circle_t *c)
{
    for (size_t k = 0; k < n; ++k) {
        int64_t x = 2 * dsp_i(s[k]) + 1, y = 2 * dsp_q(s[k]) + 1, z = x * x + y * y;
        c->n++; c->x += x; c->y += y; c->xx += x * x; c->yy += y * y; c->xy += x * y;
        c->z += z; c->zx += z * x; c->zy += z * y; c->zz += z * z;
    }
}

/* DC in milli-cells: the circle centre when the envelope is clearly
 * constant (r^2 mean^2/var >= 2; noise gives 1), else the plain mean.
 * Returns 1 when the circle centre was used. */
static inline int dsp_circle_dc(const dsp_circle_t *c, int *i, int *q, unsigned *ratio_x100)
{
    *i = *q = 0;
    if (ratio_x100) *ratio_x100 = 0;
    if (c->n < 64) return 0;
    double n = (double)c->n, mx = c->x / n, my = c->y / n;
    *i = (int)(mx * 500.0); *q = (int)(my * 500.0);
    double sxx = c->xx / n - mx * mx, syy = c->yy / n - my * my, sxy = c->xy / n - mx * my;
    double mz = c->z / n, szx = c->zx / n - mz * mx, szy = c->zy / n - mz * my;
    double det = sxx * syy - sxy * sxy;
    if (!(det > 1e-6)) return 0;
    double a = .5 * (szx * syy - szy * sxy) / det, b = .5 * (szy * sxx - szx * sxy) / det;
    double m1 = mz - 2 * a * mx - 2 * b * my + a * a + b * b;
    double ezz = c->zz / n, ezx = c->zx / n, ezy = c->zy / n, exx = c->xx / n, eyy = c->yy / n, exy = c->xy / n;
    double eu2 = ezz + 4 * a * a * exx + 4 * b * b * eyy - 4 * a * ezx - 4 * b * ezy + 8 * a * b * exy;
    double eu = mz - 2 * a * mx - 2 * b * my, var = eu2 - eu * eu;
    double ratio = var > 0 ? m1 * m1 / var : 99.99;
    if (ratio_x100) *ratio_x100 = ratio > 99.99 ? 9999u : (unsigned)(ratio * 100.0 + .5);
    if (ratio < 2.0 || fabs(a) > 15.0 || fabs(b) > 15.0) return 0;
    *i = (int)(a * 500.0); *q = (int)(b * 500.0);
    return 1;
}

/* Carrier test independent of sync and receiver DC: after removing the
 * capture's DC, complex noise has an exponential r^2 (mean^2/var = 1.00,
 * x100 = 100); an FM carrier raises it (1.33 at 0 dB SNR). */
static inline unsigned dsp_envelope_ratio_x100(const uint8_t *s, size_t n)
{
    if (n < 64u) return 0u;
    int64_t si = 0, sq = 0;
    for (size_t k = 0; k < n; ++k) { si += 2 * dsp_i(s[k]) + 1; sq += 2 * dsp_q(s[k]) + 1; }
    int64_t mi = si * 64 / (int64_t)n, mq = sq * 64 / (int64_t)n;
    double m1 = 0.0, m2 = 0.0;
    for (size_t k = 0; k < n; ++k) {
        double di = (double)((2 * dsp_i(s[k]) + 1) * 64 - mi) / 64.0;
        double dq = (double)((2 * dsp_q(s[k]) + 1) * 64 - mq) / 64.0;
        double r2 = di * di + dq * dq;
        m1 += r2;
        m2 += r2 * r2;
    }
    m1 /= (double)n;
    m2 /= (double)n;
    double var = m2 - m1 * m1;
    if (var <= 0.0) return 9999u;
    double ratio = m1 * m1 / var * 100.0;
    return ratio > 9999.0 ? 9999u : (unsigned)(ratio + 0.5);
}

/* Quiet test of the calibration (receiver noise, no carrier). The envelope
 * statistic of complex noise is 1.00 and an FM carrier raises it (1.32 at
 * 0 dB C/N); rail clipping of strong noise raises it too, so the limit
 * grows with the railed fraction. Not phase coherence: 4-bit noise at the
 * table maximum reads 36..61 % "coherent", which rejected every channel
 * with all VTXs off (board, 2026-10-11). Host test: test/test_quiet.c. */
#define DSP_QUIET_ENV_X100     115u
#define DSP_QUIET_CARRIER_X100 30u    /* above the quiet limit: certainly a carrier */
static inline unsigned dsp_quiet_limit(int clip_pm)
{
    /* Clipped noise: env ~ 100 + 0.3 x clip_pm (host test); 0.4 keeps a
     * 0 dB C/N carrier out at every tested noise level. */
    return DSP_QUIET_ENV_X100 + (unsigned)(clip_pm > 0 ? clip_pm : 0) * 2u / 5u;
}

/* NULL when the bytes are receiver noise, else the reason. */
static inline const char *dsp_quiet_reason(const uint8_t *s, size_t n, unsigned *env_out, int *clip_out)
{
    if (n < 64u) return "probe_failed";
    unsigned origin = 0, rail = 0;
    for (size_t k = 0; k < n; ++k) {
        int i = dsp_i(s[k]), q = dsp_q(s[k]);
        origin += (i == 0 || i == -1) && (q == 0 || q == -1);
        rail += i == -8 || i == 7 || q == -8 || q == 7;
    }
    int clip = (int)(rail * 1000u / n);
    unsigned env = dsp_envelope_ratio_x100(s, n);
    if (env_out) *env_out = env;
    if (clip_out) *clip_out = clip;
    if (origin * 2u > n) return "origin";
    if (rail * 10u > n * 3u) return "rails";     /* > 30 %: not a usable noise measurement */
    return env <= dsp_quiet_limit(clip) ? NULL : "envelope";
}

/* RC filter capacitor byte: calibrated base + offset, saturating at 60
 * (the code phy_11p_set() writes); upper bits preserved. */
static inline uint8_t dsp_filter_code(uint8_t calibrated, int offset)
{
    int code = (calibrated & 63) + offset;
    if (code < 0) code = 0;
    if (code > 60) code = 60;
    return (uint8_t)((calibrated & ~63u) | (unsigned)code);
}

/* Closed-loop step for two DC DACs with a measured 2x2 response (milli-
 * cells per code); 0 when ill-conditioned. Steps bounded by `limit`. */
static inline int dsp_dco_step(const float j[4], float di, float dq, int limit, int *step_a, int *step_b)
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

/* Receiver-noise spectrum: 64-point complex FFT of 40 MS/s regions (625 kHz
 * bins), averaged; psd[32] is DC, psd[0] is -20 MHz. */
#define DSP_FFT_N 64
#define DSP_BIN_KHZ 625
static inline void dsp_fft64(float re[DSP_FFT_N], float im[DSP_FFT_N])
{
    for (unsigned i = 1, j = 0; i < DSP_FFT_N; ++i) {
        unsigned bit = DSP_FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= DSP_FFT_N; len <<= 1) {
        double ang = -2 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (unsigned i = 0; i < DSP_FFT_N; i += len) {
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

static inline void dsp_psd_accumulate(const uint8_t *s, float psd[DSP_FFT_N])
{
    float re[DSP_FFT_N], im[DSP_FFT_N], mi = 0, mq = 0;
    for (unsigned k = 0; k < DSP_FFT_N; ++k) {
        re[k] = dsp_i(s[k]) + 0.5f; im[k] = dsp_q(s[k]) + 0.5f;
        mi += re[k]; mq += im[k];
    }
    mi /= DSP_FFT_N; mq /= DSP_FFT_N;
    static float hann[DSP_FFT_N];
    if (hann[1] == 0.f)
        for (unsigned k = 0; k < DSP_FFT_N; ++k)
            hann[k] = (float)(0.5 - 0.5 * cos(2 * M_PI * k / DSP_FFT_N));
    for (unsigned k = 0; k < DSP_FFT_N; ++k) {
        re[k] = (re[k] - mi) * hann[k]; im[k] = (im[k] - mq) * hann[k];
    }
    dsp_fft64(re, im);
    for (unsigned k = 0; k < DSP_FFT_N; ++k)
        psd[(k + DSP_FFT_N / 2) % DSP_FFT_N] += re[k] * re[k] + im[k] * im[k];
}

/* In-band reference: median of the bins 1.25..5 MHz from DC. */
static inline float dsp_psd_ref(const float psd[DSP_FFT_N])
{
    const unsigned c = DSP_FFT_N / 2;
    float ref[14];
    unsigned n = 0;
    for (unsigned d = 2; d <= 8; ++d) { ref[n++] = psd[c - d]; ref[n++] = psd[c + d]; }
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && ref[j - 1] > ref[j]; --j) {
            float t = ref[j]; ref[j] = ref[j - 1]; ref[j - 1] = t;
        }
    return (ref[n / 2 - 1] + ref[n / 2]) / 2;
}

/* Full -3 dB width in kHz (40000 = wider than the span, 0 = no reference). */
static inline unsigned dsp_psd_width_khz(const float psd[DSP_FFT_N])
{
    const unsigned c = DSP_FFT_N / 2;
    float half = dsp_psd_ref(psd) / 2;
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
    return (edge[0] + edge[1] + 1) * DSP_BIN_KHZ;
}

/* Equivalent noise bandwidth in kHz: total power / in-band density. PARLIO
 * keeps every second sample of the ~80 MS/s bus unfiltered, so this is the
 * pre-detection noise bandwidth the demod actually sees, alias included. */
static inline unsigned dsp_psd_nbw_khz(const float psd[DSP_FFT_N])
{
    const unsigned c = DSP_FFT_N / 2;
    float ref = dsp_psd_ref(psd);
    if (!(ref > 0)) return 0;
    double sum = 0.5 * ((double)psd[c - 1] + psd[c + 1]);
    for (unsigned k = 0; k < DSP_FFT_N; ++k)
        if (k != c) sum += psd[k];
    double khz = sum / ref * DSP_BIN_KHZ;
    return khz > 4e6 ? 4000000u : (unsigned)(khz + 0.5);
}
