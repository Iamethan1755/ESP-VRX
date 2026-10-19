/* C5VRX by Twotoz and contributors: issue #199 synthetic regression through
 * the same quiet and 96-window filter-measurement helpers as production.
 * Matches reported DC/noise; no reporter raw snapshot was attached. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "calibration_noise.h"
#define N (24u * 256u)
static uint64_t rng = 199;
static double uniform(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((rng >> 11) + .5) / 9007199254740992.;
}
static double gauss(void) { return sqrt(-2 * log(uniform())) * cos(2 * M_PI * uniform()); }
static int cell(double x)
{
    int c = (int)floor(x);
    return c < -8 ? -8 : c > 7 ? 7 : c;
}
static void make(uint8_t *s, double alpha, double cn_db)
{
    static double noise[2][N];
    for (unsigned axis = 0; axis < 2; ++axis) {
        double y1 = 0, y2 = 0, power = 0;
        for (unsigned k = 0; k < N; ++k) {
            y1 += alpha * (gauss() - y1); y2 += alpha * (y1 - y2);
            noise[axis][k] = y2;
            power += y2 * y2;
        }
        double scale = sqrt(N / power);
        for (unsigned k = 0; k < N; ++k) noise[axis][k] *= scale;
    }
    double ratio = cn_db > -99 ? pow(10., cn_db / 10.) : 0;
    double scale = 1.7 / sqrt(1 + ratio), phase = 0;
    for (unsigned k = 0; k < N; ++k) {
        double f = -1e6 + 2.5e6 * (2 * fmod(k * 15625. / 40e6, 1.) - 1);
        phase += 2 * M_PI * f / 40e6;
        /* Add half a cell before floor so captured means match +1.23/-3.26. */
        int i = cell(1.23 + .5 + scale * (noise[0][k] / sqrt(2.) + sqrt(ratio) * cos(phase)));
        int q = cell(-3.26 + .5 + scale * (noise[1][k] / sqrt(2.) + sqrt(ratio) * sin(phase)));
        s[k] = (uint8_t)(((i & 15) << 4) | (q & 15));
    }
}
static int clip(const uint8_t *s)
{
    unsigned rail = 0;
    for (unsigned k = 0; k < 256; ++k) {
        int i = dsp_i(s[k]), q = dsp_q(s[k]);
        rail += i == -8 || i == 7 || q == -8 || q == 7;
    }
    return (int)(rail * 1000u / 256u);
}
int main(void)
{
    static uint8_t s[N];
    unsigned width, nbw, env;
    int clipping;
    unsigned eligible = 0;
    for (unsigned setting = 0; setting < 100; ++setting) {
        make(s, .3 + setting * .004, -100);
        assert(cal_channel_quiet(s, 24, false, &env, &clipping) == NULL);
        cal_noise_t a = {0};
        for (unsigned w = 0; w < 96; ++w) {
            const uint8_t *part = s + (w % 24) * 256;
            cal_noise_add(&a, part, 256, clip(part));
        }
        assert(cal_noise_finish(&a, &width, &nbw, &env, &clipping));
        /* As in the real sweep, noisy/too-narrow settings may be skipped;
         * only a definite carrier aborts it. Do not relax quiet thresholds. */
        assert(width && nbw && !cal_setting_carrier(env, clipping));
        eligible += width >= 12000 && cal_setting_quiet(env, clipping);
    }
    assert(eligible > 0);
    for (int cn = -1; cn <= 3; ++cn) {
        make(s, .55, cn);
        assert(cal_channel_quiet(s, 24, false, NULL, NULL) != NULL);
        cal_noise_t a = {0};
        for (unsigned w = 0; w < 96; ++w) {
            const uint8_t *part = s + (w % 24) * 256;
            cal_noise_add(&a, part, 256, clip(part));
        }
        assert(cal_noise_finish(&a, &width, &nbw, &env, &clipping));
        assert(!cal_setting_quiet(env, clipping));
    }
    cal_noise_t missing = {0};
    for (unsigned k = 0; k < 47; ++k) cal_noise_add(&missing, s, 256, clip(s));
    cal_noise_add(&missing, NULL, 256, 0);
    cal_noise_add(&missing, s, 255, 0);
    assert(!cal_noise_finish(&missing, &width, &nbw, &env, &clipping));
    assert(!strcmp(cal_channel_quiet(s, 15, false, NULL, NULL), "probe_failed"));
    assert(!strcmp(cal_channel_quiet(s, 24, true, NULL, NULL), "video_sync"));
    memset(s, 0, sizeof(s));
    assert(!strcmp(cal_channel_quiet(s, 24, false, NULL, NULL), "origin"));
    memset(s, 0x77, sizeof(s));
    assert(!strcmp(cal_channel_quiet(s, 24, false, NULL, NULL), "rails"));
    puts("PASS: issue #199 DC/noise, 100 synthetic settings, weak FM, missing probes, sync/origin/rails");
    return 0;
}
