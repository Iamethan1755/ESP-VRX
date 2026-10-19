/* Host test for analog_video_detect (issue #128): real analog video must
 * pass, Wi-Fi-like OFDM energy, noise and an unmodulated carrier must not.
 * IQ is synthesized at 40 MS/s with receiver noise (sigma 0.56 coarse step,
 * measured on the XIAO at maximum gain) and quantized to the live Q4/I4
 * nibbles; video uses the real VTX's shallow deviation (sync ~28 Phase8
 * bins below blanking, as measured on hardware). */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "analog_video_detect.h"
#include "phase8_gain_lut.h"

#define FS 40e6
#define BYTES 8184u
#define PI 3.14159265358979323846

static uint8_t buf[BYTES];

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v);
}

static uint8_t quantize(double i, double q)
{
    int ii = (int)floor(i), qq = (int)floor(q);
    if (ii < -512) ii = -512; if (ii > 511) ii = 511;
    if (qq < -512) qq = -512; if (qq > 511) qq = 511;
    return (uint8_t)((((ii >> 6) & 15) << 4) | ((qq >> 6) & 15));
}

typedef enum { SIG_VIDEO, SIG_OFDM, SIG_NOISE, SIG_CW } sig_t;

/* Frequency (Hz) of the composite at output-sample time t (40 MS/s index). */
static double video_hz(long s, double line_us, bool random_picture, double cfo)
{
    double t_us = s / 40.0;
    long line = (long)(t_us / line_us);
    double x = fmod(t_us, line_us);
    const double per_bin = 20e6 / 256.0;            /* Hz per Phase8 bin */
    double sync = -28 * per_bin, blank = -10 * per_bin;
    double hz;
    if (x < 4.7) hz = sync;
    else if (x >= 5.6 && x < 7.85) hz = blank + 3 * per_bin * sin(2 * PI * 4.43361875 * x);
    else if (x < 10.5 || x > line_us - 1.5) hz = blank;
    else {
        double level;
        if (random_picture) level = (rand() % 1000) / 1000.0;   /* worst case */
        else level = 0.5 + 0.4 * sin(2 * PI * (x / 52.0) * 3 + line * 0.02);
        hz = blank + level * 45 * per_bin;
    }
    return hz + cfo;
}

static analog_video_t run(sig_t sig, double radius, double line_us,
                          bool random_picture, double cfo, unsigned seed)
{
    srand(seed);
    double ph = 0, sigma = 0.56 * 64.0;
    long start = rand() % 100000;
    for (unsigned n = 0; n < BYTES; ++n) {
        double i, q;
        long s = start + n;
        if (sig == SIG_VIDEO || sig == SIG_CW) {
            double hz = sig == SIG_CW ? cfo : video_hz(s, line_us, random_picture, cfo);
            ph += 2 * PI * hz / FS;
            i = radius * 64.0 * cos(ph);
            q = radius * 64.0 * sin(ph);
        } else if (sig == SIG_OFDM) {
            /* OFDM: complex Gaussian with the given rms radius. */
            i = radius * 64.0 * gauss() / sqrt(2.0);
            q = radius * 64.0 * gauss() / sqrt(2.0);
        } else {
            i = q = 0;
        }
        i += sigma * gauss();
        q += sigma * gauss();
        buf[n] = quantize(i, q);
    }
    /* Endpoints: odd bytes (ring offset 0), as the firmware copies them. */
    static uint8_t ep[BYTES / 2u];
    for (unsigned k = 0; k < BYTES / 2u; ++k) ep[k] = buf[2u * k + 1u];
    return analog_video_detect(ep, BYTES / 2u, c5vrx_phase8_gain_lut);
}

static int worst(sig_t sig, double r, double line_us, bool rnd, double cfo, bool max)
{
    int w = max ? -1000 : 1000;
    for (unsigned seed = 1; seed <= 40; ++seed) {
        analog_video_t a = run(sig, r, line_us, rnd, cfo, seed * 7919u);
        if (max ? a.confidence > w : a.confidence < w) w = a.confidence;
    }
    return w;
}

int main(void)
{
    const double pal = 64.0, ntsc = 63.5556;
    struct { const char *name; sig_t sig; double r, line; bool rnd; double cfo; bool must_pass; } cases[] = {
        { "PAL calm picture",          SIG_VIDEO, 4.5, pal,  false, 0,      true  },
        { "PAL random picture",        SIG_VIDEO, 4.5, pal,  true,  0,      true  },
        { "PAL CFO +700 kHz",          SIG_VIDEO, 4.5, pal,  true,  7e5,    true  },
        { "PAL weak (r 2.5)",          SIG_VIDEO, 2.5, pal,  true,  0,      true  },
        { "NTSC random picture",       SIG_VIDEO, 4.5, ntsc, true,  -4e5,   true  },
        { "OFDM Wi-Fi strong",         SIG_OFDM,  6.0, pal,  false, 0,      false },
        { "OFDM Wi-Fi medium",         SIG_OFDM,  3.0, pal,  false, 0,      false },
        { "noise only",                SIG_NOISE, 0.0, pal,  false, 0,      false },
        { "unmodulated carrier",       SIG_CW,    5.0, pal,  false, 3e5,    false },
    };
    setvbuf(stdout, NULL, _IONBF, 0);
    int fails = 0;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        int lo = worst(cases[c].sig, cases[c].r, cases[c].line, cases[c].rnd, cases[c].cfo, false);
        int hi = worst(cases[c].sig, cases[c].r, cases[c].line, cases[c].rnd, cases[c].cfo, true);
        analog_video_t a = run(cases[c].sig, cases[c].r, cases[c].line, cases[c].rnd, cases[c].cfo, 1u);
        printf("%-24s confidence min %4d max %4d  (lag %d std %d offset %d kHz)\n",
               cases[c].name, lo, hi, a.lag, a.standard, a.offset_khz);
        if (cases[c].must_pass) {
            if (lo < ANALOG_VIDEO_MIN_CONFIDENCE) { puts("  FAIL: must pass"); ++fails; }
            if (a.standard != (cases[c].line == pal ? 1 : 2)) { puts("  FAIL: standard"); ++fails; }
        } else if (hi >= ANALOG_VIDEO_MIN_CONFIDENCE) {
            puts("  FAIL: must reject");
            ++fails;
        }
    }
    /* Carrier centring: the offset estimate follows the CFO (the mean of the
     * picture adds a fixed bias, so compare two offsets 700 kHz apart). */
    {
        analog_video_t c0 = run(SIG_VIDEO, 4.5, pal, true, 0, 3u);
        analog_video_t c7 = run(SIG_VIDEO, 4.5, pal, true, 7e5, 3u);
        int step = c7.offset_khz - c0.offset_khz;
        printf("offset step for +700 kHz CFO: %d kHz\n", step);
        if (step < 600 || step > 800) { puts("  FAIL: offset estimate"); ++fails; }
        /* A carrier 15 MHz off (neighbouring channel) aliases to a larger
         * |offset| than the centred one. */
        analog_video_t far = run(SIG_VIDEO, 4.5, pal, true, 15e6, 3u);
        printf("15 MHz off: offset %d kHz, confidence %d\n", far.offset_khz, far.confidence);
        int ac0 = c0.offset_khz < 0 ? -c0.offset_khz : c0.offset_khz;
        int afar = far.offset_khz < 0 ? -far.offset_khz : far.offset_khz;
        if (afar <= ac0 + 2000) { puts("  FAIL: neighbour not further off"); ++fails; }
    }
    assert(fails == 0);
    puts("analog video detect: OK");
    return 0;
}
