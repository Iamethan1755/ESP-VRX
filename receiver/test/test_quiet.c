/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the
 * calibration quiet test (dsp_quiet_reason, main/dsp.h).
 *
 * Synthetic Q4/I4 bytes as the ultrafine lane delivers them: band-limited
 * complex noise (receiver noise at different gains, DC offsets, rail
 * clipping) and an FM video carrier plus noise. Receiver noise with every
 * VTX off must read quiet at every gain; a carrier at >= 0 dB C/N must not;
 * dead and railing IQ must be refused with their reason.
 *
 *   cc -O2 -std=c11 -I../main test_quiet.c -lm -o test_quiet && ./test_quiet
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

#define N (24u * 256u)   /* one noise_check(): 24 probe windows */

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static double uniform(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2.0 * log(uniform())) * cos(2.0 * M_PI * uniform()); }

static int cell(double v)
{
    int c = (int)floor(v);
    return c < -8 ? -8 : c > 7 ? 7 : c;
}

/* Two-pole low-pass at ~10 MHz of 40 MS/s, unit output power. */
static void lowpass(double *x, size_t n)
{
    double a = 0.55, y1 = 0, y2 = 0, p = 0;
    for (size_t k = 0; k < n; ++k) { y1 += a * (x[k] - y1); y2 += a * (y1 - y2); x[k] = y2; p += y2 * y2; }
    p = sqrt(p / (double)n);
    for (size_t k = 0; k < n; ++k) x[k] /= p;
}

/* Complex noise of the given rms (cells per axis x sqrt2), optional FM
 * carrier at C/N cn_db (-100: none), DC in cells. */
static void make(uint8_t *out, double rms, double dc, double cn_db)
{
    static double ni[N], nq[N];
    for (size_t k = 0; k < N; ++k) { ni[k] = gauss(); nq[k] = gauss(); }
    lowpass(ni, N);
    lowpass(nq, N);
    double c = cn_db > -99.0 ? pow(10.0, cn_db / 10.0) : 0.0, phase = 0.0;
    for (size_t k = 0; k < N; ++k) {
        /* Video-like deviation: line-rate sawtooth +/- 2.5 MHz, -1 MHz centre. */
        double t = (double)k / 40e6, f = -1e6 + 2.5e6 * (2.0 * fmod(t * 15625.0, 1.0) - 1.0);
        phase += 2.0 * M_PI * f / 40e6;
        double si = ni[k] / sqrt(2.0) + sqrt(c) * cos(phase), sq = nq[k] / sqrt(2.0) + sqrt(c) * sin(phase);
        double g = rms / sqrt(1.0 + c);
        int i = cell(si * g + dc), q = cell(sq * g + dc);
        out[k] = (uint8_t)(((i & 15) << 4) | (q & 15));
    }
}

static int s_fail;
static void expect(const char *name, const uint8_t *s, const char *want)
{
    unsigned env = 0;
    int clip = 0;
    const char *got = dsp_quiet_reason(s, N, &env, &clip);
    bool ok = want ? got && !strcmp(got, want) : got == NULL;
    printf("%-4s %-34s env_x100=%3u limit=%3u clip_pm=%3d -> %s\n", ok ? "ok" : "FAIL", name, env,
           dsp_quiet_limit(clip), clip, got ? got : "quiet");
    s_fail += !ok;
}

int main(void)
{
    static uint8_t s[N];
    char name[64];
    /* All VTXs off: receiver noise from low to table-maximum gain, with
     * and without residual DC, up to visibly railing noise. */
    const double rms[] = {1.5, 2.0, 3.0, 3.5, 4.0, 5.0, 6.0};
    const double dc[] = {0.0, 1.0, 1.5};
    for (unsigned a = 0; a < sizeof(rms) / sizeof(rms[0]); ++a)
        for (unsigned b = 0; b < sizeof(dc) / sizeof(dc[0]); ++b) {
            make(s, rms[a], dc[b], -100.0);
            snprintf(name, sizeof(name), "noise rms=%.1f dc=%.1f", rms[a], dc[b]);
            expect(name, s, NULL);
        }
    /* VTX on: a carrier must never pass as quiet. */
    const double cn[] = {0.0, 3.0, 6.0, 10.0, 20.0};
    for (unsigned a = 0; a < sizeof(cn) / sizeof(cn[0]); ++a)
        for (unsigned b = 0; b < 2u; ++b) {
            make(s, b ? 4.5 : 3.0, 0.5, cn[a]);
            snprintf(name, sizeof(name), "carrier cn=%.0f dB rms=%.1f", cn[a], b ? 4.5 : 3.0);
            expect(name, s, "envelope");
        }
    /* Dead IQ (no RF, held at the origin) and railing IQ. */
    make(s, 0.3, 0.0, -100.0);
    expect("dead iq (origin)", s, "origin");
    make(s, 12.0, 0.0, -100.0);
    expect("railing iq", s, "rails");
    printf("%s (%d failures)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail != 0;
}
