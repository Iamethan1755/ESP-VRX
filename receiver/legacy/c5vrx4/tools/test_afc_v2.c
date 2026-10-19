#include "afc_v2.h"
#include "phase8_gain_lut.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Synthetic FM-CVBS through the real Q4/I4 quantiser. Frequency deviation is
 * applied per IRE so that a known carrier offset appears at blanking level. */
#define FS_HZ 40e6
#define KHZ_PER_IRE 57.0 /* ~8 MHz over 140 IRE */

typedef struct {
    double line_us, sync_us, burst_start_us, burst_len_us, porch_end_us, fsc_hz, front_porch_us;
} standard_t;

static const standard_t NTSC = {63.556, 4.7, 5.3, 2.5, 9.4, 3579545.0, 1.5};
static const standard_t PAL = {64.0, 4.7, 5.6, 2.25, 10.4, 4433618.75, 1.65};

static uint32_t rng = 12345u;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }
static double gauss(void) { double u = urand() + 1e-9, v = urand(); return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }

static int q4(double x)
{
    int c = (int)floor(x);
    return c < -8 ? -8 : c > 7 ? 7 : c;
}

/* Returns IRE level at time t within a line; active video is random blocks. */
static double cvbs_ire(const standard_t *st, double t_us, double active_level)
{
    if (t_us < st->sync_us) return -40.0;
    if (t_us >= st->burst_start_us && t_us < st->burst_start_us + st->burst_len_us)
        return 20.0 * sin(2 * M_PI * st->fsc_hz * t_us * 1e-6);
    if (t_us < st->porch_end_us) return 0.0;
    if (t_us >= st->line_us - st->front_porch_us) return 0.0; /* front porch */
    return active_level;
}

static afc2_result_t run(const standard_t *st, double cfo_khz, int polarity,
                         double radius, double noise, double offset_us)
{
    static uint8_t buf[4092];
    double phase = urand() * 2 * M_PI;
    double active = 50.0;
    const double audio_phase[2] = {urand() * 2 * M_PI, urand() * 2 * M_PI};
    for (unsigned n = 0; n < sizeof(buf); ++n) {
        double t_us = fmod(n / 40.0 + offset_us, st->line_us);
        if (n % 400u == 0) active = urand() * 100.0; /* 10 us picture blocks */
        /* RTC6705-class VTX audio subcarriers: 6.0 / 6.5 MHz at -25 dBc,
         * i.e. ~0.7 MHz peak carrier deviation each (datasheet VTAA). */
        double audio_khz = 700.0 * sin(2 * M_PI * 6.0e6 * n / FS_HZ + audio_phase[0]) +
                           700.0 * sin(2 * M_PI * 6.5e6 * n / FS_HZ + audio_phase[1]);
        double f_khz = cfo_khz + audio_khz +
                       polarity * KHZ_PER_IRE * cvbs_ire(st, t_us, active);
        phase += 2 * M_PI * f_khz * 1e3 / FS_HZ;
        double i = radius * cos(phase) + noise * gauss();
        double q = radius * sin(phase) + noise * gauss();
        buf[n] = (uint8_t)(((q4(i) & 15) << 4) | (q4(q) & 15));
    }
    return afc2_measure(buf, sizeof(buf), c5vrx_phase8_gain_lut);
}

int main(void)
{
    const standard_t *standards[2] = {&NTSC, &PAL};
    const double cfos[4] = {0.0, 250.0, -400.0, 900.0};
    unsigned checks = 0, timing_checks = 0;
    for (unsigned si = 0; si < 2; ++si)
        for (int pol = -1; pol <= 1; pol += 2)
            for (unsigned ci = 0; ci < 4; ++ci)
                for (unsigned off = 0; off < 3; ++off) {
                    double cfo = cfos[ci];
                    afc2_result_t r = run(standards[si], cfo, pol, 4.5, 0.35, off * 17.0);
                    if (r.lines < 1)
                        fprintf(stderr, "NOLINES std=%u pol=%d cfo=%.0f off=%u got_pol=%d\n",
                                si, pol, cfo, off, r.polarity);
                    assert(r.lines >= 1);
                    /* Sync is -40 IRE: with positive FM polarity it is the
                     * low-frequency extreme, reported as polarity -1. */
                    if (r.polarity != -pol)
                        fprintf(stderr, "POL std=%u pol=%d cfo=%.0f off=%u got=%d lines=%u sync=%d porch=%d\n",
                               si, pol, cfo, off, r.polarity, r.lines, (int)r.sync_khz, (int)r.porch_khz);
                    assert(r.polarity == -pol);
                    assert(r.standard == si + 1u);
                    assert(r.burst_x10 >= AFC2_BURST_MIN_X10);
                    double sync_expect = cfo + pol * KHZ_PER_IRE * -40.0;
                    if (fabs(r.porch_khz - cfo) > 90.0 || fabs(r.sync_khz - sync_expect) > 90.0) {
                        printf("std=%u pol=%d cfo=%.0f off=%u lines=%u porch=%d sync=%d (expect %.0f)\n",
                               si, pol, cfo, off, r.lines, (int)r.porch_khz, (int)r.sync_khz, sync_expect);
                        assert(0);
                    }
                    assert(r.width_40m >= AFC2_SYNC_MIN && r.width_40m <= AFC2_SYNC_MAX);
                    if (r.period_40m) {
                        assert(r.lines >= 2);
                        assert(fabs(r.period_40m - standards[si]->line_us * 40.0) < 9.0);
                        ++timing_checks;
                    }
                    ++checks;
                }

    assert(timing_checks >= 8);

    /* Small native-AGC radius still works once averaged. */
    afc2_result_t small = run(&PAL, 300.0, 1, 2.3, 0.35, 5.0);
    assert(small.lines >= 1 && fabs(small.porch_khz - 300.0) < 150.0);

    /* Pure noise must not produce a confident sync. */
    static uint8_t noise_buf[4092];
    for (unsigned n = 0; n < sizeof(noise_buf); ++n)
        noise_buf[n] = (uint8_t)(((q4(gauss() * 0.7) & 15) << 4) | (q4(gauss() * 0.7) & 15));
    afc2_result_t none = afc2_measure(noise_buf, sizeof(noise_buf), c5vrx_phase8_gain_lut);
    assert(none.lines == 0 && none.period_40m == 0 && none.width_40m == 0);

    /* Settling/transition evidence is conservative IQ gating, not decoded
     * native gain telemetry. Origin collapse and a large envelope jump fail. */
    static uint8_t envelope[128];
    for (unsigned k = 0; k < sizeof(envelope); ++k) envelope[k] = 0x22;
    assert(afc2_envelope_stationary(envelope, sizeof(envelope)));
    for (unsigned k = 64; k < sizeof(envelope); ++k) envelope[k] = 0x66;
    assert(!afc2_envelope_stationary(envelope, sizeof(envelope)));
    for (unsigned k = 0; k < sizeof(envelope); ++k) envelope[k] = 0x00;
    assert(!afc2_envelope_stationary(envelope, sizeof(envelope)));

    /* Too-short input is rejected. */
    assert(afc2_measure(noise_buf, 100, c5vrx_phase8_gain_lut).lines == 0);

    printf("AFC V2 reference estimator (#115): %u synthetic PAL/NTSC cases passed\n", checks);
    return 0;
}
