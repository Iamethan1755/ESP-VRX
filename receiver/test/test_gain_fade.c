/* C5VRX-5 by Twotoz and the C5VRX contributors: Direct Gain V5 (main/gain.c)
 * under fast fading, host model.
 *
 * Open field, moving drone: ground-reflection and multipath nulls last a few
 * milliseconds, then the carrier returns at its old level. The test drives
 * the real dg_tick() with observations generated from a simple receiver
 * model and measures how long the picture stays unusable after the carrier
 * is back, plus the gain writes spent, and the price of the fade hold: a
 * carrier that stays weaker after the drop recovers up to ~10 ms later.
 * Also checks that a carrier that is
 * really gone (VTX off) still reaches the table maximum, that a slow fade
 * (distance) is tracked, and that a sudden strong carrier is cut at once.
 *
 * Model (rough, monotonic; not a calibrated C5 gain table): gain in dB from
 * the decoded 5 GHz tuple (6 dB per RF stage, 6 dB per BB bit, 0.83 dB per
 * fine step); receiver noise reads P50 ~9 at the table maximum (gain.c
 * comment, board 2026-10-11); a carrier radius above ~7.5 cells folds in the
 * ultrafine window. Usable picture: carrier radius 2.5..7.5 cells.
 *
 *   cc -O2 -std=c11 -I../main test_gain_fade.c ../main/gain.c ../main/arc_phy.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gain.h"

unsigned char phy_param[2048];     /* arc_phy.c reference; unused for the 5 GHz table */

#define TICK_US 200u
static direct_gain_t g;
static arc_gain_table_t table;
static double s_db[DG_STATES];
static unsigned s_fail, s_run;

static void build_model(void)
{
    arc_gain_table_from_bytes(&table, NULL, ARC_5G_GAIN_MAX);
    for (unsigned k = 0; k <= table.max_index; ++k) {
        arc_gain_tuple_t t;
        arc_gain_tuple_decode(&table, (uint8_t)k, &t);
        s_db[k] = 6.0 * t.rf_stage + 6.0 * log2(t.bb_code + 1.0) + 0.83 * (5 - (int)t.fine_code);
    }
}

/* Carrier radius in cells at gain k for a carrier level (dB, relative). */
static double radius(double level_db, unsigned k) { return level_db <= -200 ? 0 : pow(10.0, (level_db + s_db[k]) / 20.0); }
static double noise_radius(unsigned k) { return 3.6 * pow(10.0, (s_db[k] - s_db[table.max_index]) / 20.0); }

static dg_observation_t observe(double level_db, unsigned k, uint64_t t)
{
    dg_observation_t o;
    memset(&o, 0, sizeof(o));
    o.observed_us = t;
    double r = radius(level_db, k), rn = noise_radius(k);
    double p50 = r * r + 0.69 * rn * rn;
    bool folded = r + rn > 7.5;
    o.p50 = (uint8_t)(p50 > 113 ? 113 : p50);
    o.p90 = (uint8_t)(p50 * 1.3 > 113 ? 113 : p50 * 1.3);
    o.p95 = (uint8_t)(p50 * 1.5 > 113 ? 113 : p50 * 1.5);
    o.clip_pm = (uint16_t)(folded ? fmin(1000.0, 150.0 * (r + rn) / 7.5) : (r + 2 * rn > 7.5 ? 10 : 0));
    double spread = r > 0.5 ? 0 : rn;
    o.origin_pm = (uint16_t)(r > 2.0 ? 20 : 1000.0 * exp(-(spread * spread + r * r) / 1.5));
    o.coherence = (uint8_t)(folded ? 30 : r > 2.5 ? 95 : r > 1.2 ? 70 : rn > 1.0 ? 45 : 10);
    return o;
}

static bool usable(double level_db, unsigned k)
{
    double r = radius(level_db, k);
    return r >= 2.5 && r + noise_radius(k) <= 7.5;
}

/* Settle at a carrier level, then apply a level profile; returns the time
 * (us) after `back_us` until the picture is usable again, and the writes. */
typedef double (*profile_t)(uint64_t t);
static uint64_t run(double steady_db, profile_t level, uint64_t total_us, uint64_t back_us, unsigned *writes,
                    unsigned *final_gain)
{
    uint64_t t;
    /* Settle on the steady carrier (0.8 s). */
    dg_reset(&g, &table, 60);
    g.learn_enabled = true;    /* a confirmed VTX throughout */
    t = 1000;
    for (unsigned n = 0; n < 4000; ++n, t += TICK_US) {
        dg_observation_t o = observe(steady_db, g.current_gain, t);
        (void)dg_tick(&g, &o);
    }
    const uint32_t w0 = g.writes;
    const uint64_t t0 = t;
    uint64_t recovered = 0;
    bool done = false;
    for (; t - t0 < total_us; t += TICK_US) {
        double lv = level(t - t0);
        dg_observation_t o = observe(lv, g.current_gain, t);
        (void)dg_tick(&g, &o);
        if (!done && t - t0 >= back_us && usable(lv, g.current_gain)) { recovered = t - t0 - back_us; done = true; }
    }
    if (writes) *writes = g.writes - w0;
    if (final_gain) *final_gain = g.current_gain;
    return done ? recovered : UINT64_MAX;
}

static double s_steady;
static uint64_t s_fade_us;
static double s_depth;
static double fade(uint64_t t) { return t >= 10000 && t < 10000 + s_fade_us ? s_steady + s_depth : s_steady; }
static double vtx_off(uint64_t t) { return t >= 10000 ? -999 : s_steady; }
static double slow(uint64_t t) { return s_steady - 20.0 * (double)t / 2e6; }
static double jump_up(uint64_t t) { return t >= 10000 ? s_steady + 20 : s_steady; }
static double step_down(uint64_t t) { return t >= 10000 ? s_steady + s_depth : s_steady; }

static void check(const char *name, bool ok)
{
    ++s_run;
    if (!ok) { ++s_fail; printf("FAIL %s\n", name); }
}

int main(void)
{
    build_model();
    /* A carrier healthy around G60 (radius ~4.5 cells). */
    s_steady = 20.0 * log10(4.5) - s_db[60];
    printf("model: G60 %.1f dB, G%u %.1f dB (+%.1f dB to the maximum)\n", s_db[60], table.max_index,
           s_db[table.max_index], s_db[table.max_index] - s_db[60]);
    const uint64_t fades[] = {1000, 3000, 10000, 30000};
    const double depths[] = {-40, -12};
    uint64_t worst_full = 0;
    for (unsigned d = 0; d < 2u; ++d)
        for (unsigned f = 0; f < 4u; ++f) {
            s_fade_us = fades[f];
            s_depth = depths[d];
            unsigned writes = 0, gk = 0;
            uint64_t rec = run(s_steady, fade, 400000, 10000 + s_fade_us, &writes, &gk);
            printf("fade %5.1f ms depth %4.0f dB: picture back %s%6.2f ms after the carrier, %u gain writes, "
                   "end G%u\n", fades[f] / 1000.0, depths[d], rec == UINT64_MAX ? "never " : "",
                   rec == UINT64_MAX ? 0.0 : rec / 1000.0, writes, gk);
            if (fades[f] < 10000 && rec > worst_full) worst_full = rec;   /* nulls shorter than the hold */
            check("fade recovers", rec != UINT64_MAX);
        }
    unsigned writes = 0, gk = 0;
    s_steady = 20.0 * log10(4.5) - s_db[60];
    (void)run(s_steady, vtx_off, 400000, 10000, &writes, &gk);
    printf("VTX off: end G%u after 390 ms (%u writes)\n", gk, writes);
    check("carrier gone -> table maximum", gk == table.max_index);
    uint64_t rec = run(s_steady, slow, 2000000, 0, &writes, &gk);
    printf("slow 20 dB fade over 2 s: usable from %.2f ms, %u writes, end G%u\n", rec / 1000.0, writes, gk);
    check("slow fade tracked upward", gk > 60);
    rec = run(s_steady, jump_up, 200000, 10000, &writes, &gk);
    printf("+20 dB jump: picture back %.2f ms later, %u writes, end G%u\n", rec / 1000.0, writes, gk);
    check("strong jump cut", gk < 60 && rec != UINT64_MAX);
    /* The cost: a carrier that stays weaker after the drop (obstacle). */
    uint64_t step_cost[2];
    for (unsigned d = 0; d < 2u; ++d) {
        s_depth = d ? -12 : -22;
        step_cost[d] = run(s_steady, step_down, 400000, 10000, &writes, &gk);
        printf("sustained %3.0f dB step: picture back %.2f ms later, %u writes, end G%u\n", s_depth,
               step_cost[d] == UINT64_MAX ? -1.0 : step_cost[d] / 1000.0, writes, gk);
        check("sustained step recovers", step_cost[d] != UINT64_MAX);
    }
    /* No confirmed VTX (receiver noise, VTX off): the map must not learn,
     * whatever the coherence test says. */
    {
        s_depth = -12;
        (void)run(s_steady, step_down, 400000, 10000, &writes, &gk);
        uint32_t with_vtx = g.learned;
        dg_reset(&g, &table, 60);           /* learn_enabled stays false */
        const uint32_t kept = g.learned;    /* a reset on the same table keeps the map */
        uint64_t t = 1000;
        for (unsigned n = 0; n < 6000; ++n, t += TICK_US) {
            double lv = n < 3000 ? s_steady : s_steady - 12;
            dg_observation_t o = observe(lv, g.current_gain, t);
            (void)dg_tick(&g, &o);
        }
        printf("map learning: %u with a confirmed VTX, %u new without (refused %u)\n", with_vtx, g.learned - kept,
               g.learn_refused);
        check("no map learning without a confirmed VTX", g.learned == kept && g.learn_refused > 0);
    }
    {
        /* VTX off with an uncorrected DC point (DCO FLOOR row, board walk
         * 2026-10-13): one fixed code, coherence 100, P50 ~15, inside the
         * healthy band. It must read as no carrier and listen at the maximum. */
        uint8_t dc_point[256], lut[256];
        for (unsigned k = 0; k < 256u; ++k) { dc_point[k] = 0xd3; lut[k] = 40; }  /* I=-3, Q=+3: one code */
        dg_observation_t stuck = dg_measure(dc_point, sizeof(dc_point), lut, 1000);
        check("fixed DC point: repeat_pm 1000, span 0, coherence 100, stuck",
              stuck.repeat_pm == 1000 && stuck.span == 0 && stuck.coherence == 100 && dg_stuck(&stuck));
        check("fixed DC point has no envelope power", stuck.env_ratio_x100 == 0);
        /* Equal populations at powers 18 and 98: mean 58, variance 1600,
         * so mean^2/variance * 100 = 210.25. A DC translation must leave
         * the result unchanged (both signed nibble axes, without clipping). */
        uint8_t envelope[256], shifted[256];
        for (unsigned k = 0; k < 256u; ++k) {
            int hi = k & 4u ? 3 : 1, lo = -hi - 1;
            int ci = k & 1u ? hi : lo, cq = k & 2u ? hi : lo;
            envelope[k] = (uint8_t)(((unsigned)ci & 15u) << 4 | ((unsigned)cq & 15u));
            shifted[k] = (uint8_t)(((unsigned)(ci + 1) & 15u) << 4 | ((unsigned)(cq - 1) & 15u));
        }
        dg_observation_t env = dg_measure(envelope, sizeof(envelope), lut, 1000);
        dg_observation_t dc_env = dg_measure(shifted, sizeof(shifted), lut, 1000);
        check("envelope moments match analytic ratio", env.env_ratio_x100 == 210);
        check("envelope ratio ignores DC on both IQ axes", dc_env.env_ratio_x100 == env.env_ratio_x100);
        /* Check translated raw moments against a direct DC-centred oracle,
         * including asymmetric means, correlated axes and short windows. */
        bool moments_ok = true;
        uint32_t seed = 19;
        for (unsigned trial = 0; trial < 64u; ++trial) {
            size_t count = 1u + trial * 4u;
            double mx = 0, my = 0, sp = 0, spp = 0;
            for (size_t k = 0; k < count; ++k) {
                seed = seed * 1664525u + 1013904223u;
                envelope[k] = (uint8_t)(seed >> 24);
                if (trial & 1u) envelope[k] = (uint8_t)((envelope[k] & 15u) * 17u);
                mx += 2 * ((int8_t)(envelope[k] & 240u) >> 4) + 1;
                my += 2 * ((int8_t)((envelope[k] & 15u) << 4) >> 4) + 1;
            }
            mx /= count; my /= count;
            for (size_t k = 0; k < count; ++k) {
                double x = 2 * ((int8_t)(envelope[k] & 240u) >> 4) + 1 - mx;
                double y = 2 * ((int8_t)((envelope[k] & 15u) << 4) >> 4) + 1 - my;
                double p = x * x + y * y;
                sp += p; spp += p * p;
            }
            double mp = sp / count, var = spp / count - mp * mp;
            double ratio = mp <= 0 ? 0 : var > 0 ? 100 * mp * mp / var : 65535;
            int expected = ratio > 65535 ? 65535 : (int)(ratio + .5);
            dg_observation_t measured = dg_measure(envelope, count, lut, 1000);
            moments_ok = moments_ok && abs((int)measured.env_ratio_x100 - expected) <= 1;
        }
        check("integer envelope moments match the centred oracle (64 asymmetric/correlated probes)", moments_ok);
        /* A strong clean carrier: radius 4 cells, slow rotation, so most
         * neighbours repeat, but the circle spans many codes (board, G27). */
        uint8_t circle[256];
        for (unsigned k = 0; k < 256u; ++k) {
            double a = 6.2831853 * k / 128.0;
            int ci = (int)lround(4.0 * cos(a) - 0.5), cq = (int)lround(4.0 * sin(a) - 0.5);
            circle[k] = (uint8_t)(((unsigned)ci & 15u) << 4 | ((unsigned)cq & 15u));
        }
        dg_observation_t strong = dg_measure(circle, sizeof(circle), lut, 1000);
        printf("strong slow carrier: repeat_pm %u span %u\n", strong.repeat_pm, strong.span);
        check("a strong slowly rotating carrier is not a fixed point", strong.repeat_pm >= 400 && !dg_stuck(&strong));
        check("clean rotating carrier has a constant envelope", strong.env_ratio_x100 > 1543);
        /* The same envelope with noise on it (weak carrier at G73-75) is not stuck. */
        dg_observation_t noisy = stuck;
        noisy.repeat_pm = 300;
        noisy.span = 9;
        check("a weak carrier in noise is not a fixed point", !dg_stuck(&noisy));
    }
#ifdef EXPECT_FADE_HOLD
    check("sustained step: at most ~the 10 ms hold more than without it", step_cost[0] <= 25000 && step_cost[1] <= 15000);
    /* With the fade hold a short deep null costs no recovery time. */
    check("null shorter than the hold: picture back within 1 ms", worst_full <= 1000);
#endif
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
