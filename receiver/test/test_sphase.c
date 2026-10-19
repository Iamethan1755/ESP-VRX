/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the
 * sampling-phase verdict (main/sphase_logic.h) and its probe metrics
 * (dsp_hf4_sums / dsp_hf2_sums, main/dsp.h).
 *
 * Synthetic MODEM_DIAG stream: band-limited complex noise at 80 MS/s,
 * quantised to signed Q4/I4. PARLIO keeps every second word; a read on a
 * transition takes every bit at random from the old or the new word
 * (zerowidth/C5VRX PR #3). Intrinsic broadband noise (white component) is
 * added before quantisation. Model, not board data: it checks that
 * mid-transition reads are told apart from intrinsic noise, not the board's
 * absolute numbers.
 *
 *   cc -O2 -std=c11 -I../main test_sphase.c -lm -o test_sphase && ./test_sphase
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "sphase_logic.h"

/* Whiteness separation reference (diagnostic only; not a firmware gate). */
#define WHITE_REF_PM 600

#define N (48u * 256u)   /* one collect(48) of probe bytes */
#define TAPS 31

static unsigned s_fail, s_run;
static void check(const char *name, int ok)
{
    ++s_run;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
    s_fail += !ok;
}

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
static uint8_t word(double i, double q) { return (uint8_t)(((cell(i) & 15) << 4) | (cell(q) & 15)); }

/* 80 MS/s band-limited noise (windowed sinc, cutoff fc_hz), sigma cells
 * after filtering, plus a white component; PARLIO takes every second word.
 * bad: every read lands on a transition (bitwise old/new mix). */
static void stream(uint8_t *out, size_t n, double fc_hz, double sigma, double white, bool bad)
{
    static double h[TAPS];
    double norm = 0;
    for (int k = 0; k < TAPS; ++k) {
        double x = k - (TAPS - 1) / 2.0, w = 0.54 - 0.46 * cos(2 * M_PI * k / (TAPS - 1));
        h[k] = (x == 0 ? 2 * fc_hz / 80e6 : sin(2 * M_PI * fc_hz / 80e6 * x) / (M_PI * x)) * w;
        norm += h[k] * h[k];
    }
    const double scale = sigma / sqrt(norm);
    double zi[TAPS] = {0}, zq[TAPS] = {0};
    uint8_t prev = 0;
    for (size_t k = 0, m = 0; m < n; ++k) {
        memmove(zi + 1, zi, (TAPS - 1) * sizeof(double));
        memmove(zq + 1, zq, (TAPS - 1) * sizeof(double));
        zi[0] = gauss();
        zq[0] = gauss();
        double i = 0, q = 0;
        for (int t = 0; t < TAPS; ++t) { i += h[t] * zi[t]; q += h[t] * zq[t]; }
        uint8_t now = word(i * scale + white * gauss(), q * scale + white * gauss());
        if (k >= TAPS && (k & 1u)) {
            uint8_t v = now;
            if (bad) {
                uint8_t mask = (uint8_t)(uniform() * 256.0);
                v = (uint8_t)((prev & mask) | (now & (uint8_t)~mask));
            }
            out[m++] = v;
        }
        prev = now;
    }
}

typedef struct { int excess, white; } probe_t;
static probe_t probe(const uint8_t *s, size_t n)
{
    uint64_t d4 = 0, d2 = 0, count = 0;
    for (size_t r = 0; r + 256 <= n; r += 256) {
        dsp_hf4_sums(s + r, 256, &d4);
        dsp_hf2_sums(s + r, 256, &d2);
        count += 256 - 4;
    }
    int e = dsp_hf4_excess_milli(d4, count);
    return (probe_t){e > 0 ? e : 0, sphase_white_permille(d4, d2)};
}

static uint8_t s_buf[N];

/* A nine-position scan: positions repeat every three ticks, one of three
 * reads on the transition. */
static sphase_scan_t scan(double fc, double sigma, double white, bool absolute)
{
    sphase_scan_t s;
    sphase_scan_reset(&s);
    for (unsigned pos = 0; pos < 9u; ++pos) {
        stream(s_buf, N, fc, sigma, white, pos % 3u == 1u);
        probe_t p = probe(s_buf, N);
        sphase_scan_add(&s, absolute ? p.excess : p.white);
    }
    return s;
}

int main(void)
{
    char name[160];
    /* Metrics. */
    for (size_t k = 0; k < N; ++k) s_buf[k] = word(4 * gauss(), 4 * gauss());
    probe_t white = probe(s_buf, N);
    snprintf(name, sizeof(name), "i.i.d. IQ reads as white: %d per mille", white.white);
    check(name, white.white > 930 && white.white < 1070);
    stream(s_buf, N, 6.5e6, 2.5, 0.0, false);
    probe_t clean = probe(s_buf, N);
    stream(s_buf, N, 6.5e6, 2.5, 0.0, true);
    probe_t mixed = probe(s_buf, N);
    snprintf(name, sizeof(name), "13 MHz noise, clean read: excess %d < %d, white %d per mille", clean.excess,
             SPHASE_LIMIT, clean.white);
    check(name, clean.excess < SPHASE_LIMIT && clean.white < WHITE_REF_PM);
    snprintf(name, sizeof(name), "mid-transition read: excess %d >= %d, white %d >= %d", mixed.excess, SPHASE_LIMIT,
             mixed.white, WHITE_REF_PM);
    check(name, mixed.excess >= SPHASE_LIMIT && mixed.white >= WHITE_REF_PM);
    stream(s_buf, N, 6.5e6, 2.5, 0.6, false);
    probe_t noisy = probe(s_buf, N);
    snprintf(name, sizeof(name), "intrinsic white noise, clean read: excess %d above the absolute limit",
             noisy.excess);
    check(name, noisy.excess >= SPHASE_LIMIT);

    /* Verdicts on modeled scans. */
    sphase_scan_t s = scan(6.5e6, 2.5, 0.0, true);
    snprintf(name, sizeof(name), "band-limited noise scan: best %d worst %d, contrast", s.best, s.worst);
    check(name, sphase_contrast(&s));
    check("clean read below the limit: settled without a scan",
          sphase_classify(clean.excess, SPHASE_LIMIT, NULL) == SPHASE_SETTLED);
    check("mid-transition read without a scan: unsettled (repair)",
          sphase_classify(mixed.excess, SPHASE_LIMIT, NULL) == SPHASE_UNSETTLED);
    check("mid-transition read against a contrasting scan: unsettled",
          sphase_classify(mixed.excess, SPHASE_LIMIT, &s) == SPHASE_UNSETTLED);
    sphase_scan_t n = scan(6.5e6, 2.5, 0.6, true);
    stream(s_buf, N, 6.5e6, 2.5, 0.6, false);
    int noisy_best = probe(s_buf, N).excess;
    snprintf(name, sizeof(name), "intrinsic noise scan: best %d worst %d, contrast=%u", n.best, n.worst,
             sphase_contrast(&n));
    check(name, n.valid);
    snprintf(name, sizeof(name), "intrinsic noise, best position (excess %d): %s, not unsettled", noisy_best,
             sphase_verdict_name(sphase_classify(noisy_best, SPHASE_LIMIT, &n)));
    check(name, sphase_classify(noisy_best, SPHASE_LIMIT, &n) != SPHASE_UNSETTLED);
    /* Flat scan above the limit: every position equivalent. */
    sphase_scan_t flat = {.valid = true, .positions = 9, .best = 900, .worst = 1000};
    check("flat scan above the limit: noise_limited (no repair)",
          sphase_classify(980, SPHASE_LIMIT, &flat) == SPHASE_NOISE_LIMITED);
    sphase_scan_t steep = {.valid = true, .positions = 9, .best = 400, .worst = 3000};
    check("contrast above the limit, read near the best: settled",
          sphase_classify(480, SPHASE_LIMIT, &steep) == SPHASE_SETTLED);
    check("contrast above the limit, read far from the best: unsettled",
          sphase_classify(1500, SPHASE_LIMIT, &steep) == SPHASE_UNSETTLED);
    sphase_scan_t one;
    sphase_scan_reset(&one);
    sphase_scan_add(&one, 50);
    check("one measured position is no scan", !one.valid &&
          sphase_classify(800, SPHASE_LIMIT, &one) == SPHASE_UNSETTLED);
    /* The logged whiteness shows the same contrast. */
    sphase_scan_t w = scan(6.5e6, 2.5, 0.0, false);
    snprintf(name, sizeof(name), "normalized scan: best %d worst %d per mille, contrast", w.best, w.worst);
    check(name, sphase_contrast(&w));

    /* Live drift repair on the carrier (sphase_live_*). */
    check("carrier gate: table maximum is receiver noise, not carrier evidence",
          !sphase_live_carrier(true, 83, 83) && !sphase_live_carrier(true, 80, 83));
    check("carrier gate: sync and gain >= 6 below the maximum",
          sphase_live_carrier(true, 77, 83) && !sphase_live_carrier(false, 60, 83));
    stream(s_buf, N, 6.5e6, 4.0, 0.0, false);
    probe_t carrier_clean = probe(s_buf, N);
    stream(s_buf, N, 6.5e6, 4.0, 0.0, true);
    probe_t carrier_mixed = probe(s_buf, N);
    snprintf(name, sizeof(name), "radius-4 stream: clean %d below, mid-transition %d above the limit",
             carrier_clean.excess, carrier_mixed.excess);
    check(name, carrier_clean.excess < SPHASE_LIMIT && carrier_mixed.excess >= SPHASE_LIMIT);
    snprintf(name, sizeof(name), "radius-4 stream: clean %d below the live limit %d", carrier_clean.excess,
             SPHASE_LIVE_LIMIT);
    check(name, carrier_clean.excess < SPHASE_LIVE_LIMIT);
    sphase_scan_t lsb;
    sphase_scan_reset(&lsb);
    sphase_scan_add(&lsb, 40);
    sphase_scan_add(&lsb, 180);   /* one LSB lane mixing in some reads */
    sphase_scan_add(&lsb, 45);
    check("carrier contrast sees one mixing LSB lane the noise rule ignores",
          sphase_carrier_contrast(&lsb) && !sphase_contrast(&lsb));
    sphase_live_t live;
    sphase_live_reset(&live);
    const int64_t s1 = 1000000LL;
    check("noise windows never start a scan", !sphase_live_tick(&live, 3000, false, 0) &&
          live.strikes == 0 && live.carrier_checks == 0);
    check("the first carrier window of a boot chooses the phase", sphase_live_tick(&live, 20, true, s1));
    sphase_live_done(&live, true, false, s1);
    check("proactive choice is no repair and no false alarm", live.qualified && live.moved == 0 &&
          live.false_alarms == 0);
    check("a clean carrier read: nothing to do", !sphase_live_tick(&live, carrier_clean.excess, true, 2 * s1));
    check("one strike only arms the quick recheck",
          !sphase_live_tick(&live, 200, true, 3 * s1) && live.strikes == 1 &&
          sphase_live_next_us(&live) == SPHASE_LIVE_RECHECK_US);
    check("a clean read clears the strike",
          !sphase_live_tick(&live, 30, true, 5 * s1) && sphase_live_next_us(&live) == SPHASE_LIVE_CHECK_US);
    (void)sphase_live_tick(&live, 200, true, 70 * s1);
    check("a noise window between strikes neither counts nor clears",
          !sphase_live_tick(&live, 0, false, 71 * s1) && live.strikes == 1);
    check("two carrier strikes after the gap repair", sphase_live_tick(&live, 200, true, 72 * s1));
    sphase_live_done(&live, true, true, 72 * s1);
    check("a repair that moved the phase keeps the short gap", live.moved == 1 && live.gap_us == SPHASE_LIVE_GAP_US);
    (void)sphase_live_tick(&live, 200, true, 80 * s1);
    check("no repair within the gap", !sphase_live_tick(&live, 200, true, 82 * s1));
    int64_t t = 72 * s1 + SPHASE_LIVE_GAP_US;
    check("after the gap: repair again (no per-boot cap)", sphase_live_tick(&live, 200, true, t));
    unsigned alarms = 0;
    for (unsigned k = 0; k < 8u; ++k) {   /* repeated false alarms back off, never stop */
        sphase_live_done(&live, false, true, t);
        ++alarms;
        t += live.gap_us;
        (void)sphase_live_tick(&live, 200, true, t - 1);
        if (!sphase_live_tick(&live, 200, true, t)) break;
    }
    check("false alarms double the gap up to 16 min and still rescan",
          live.false_alarms == alarms && live.gap_us == SPHASE_LIVE_GAP_MAX_US && alarms == 8u);
    sphase_live_done(&live, true, true, t);
    check("a real repair resets the gap", live.gap_us == SPHASE_LIVE_GAP_US);
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
