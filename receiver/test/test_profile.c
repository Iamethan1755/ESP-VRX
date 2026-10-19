/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the VTX
 * deviation estimator and profile choice (main/profile_logic.h).
 *
 * The narrow profiles (10 MHz, kp 0.6 / 0.5) hurt a wide VTX badly, so the
 * test tries hard to push wide VTXs into them: estimate noise, outlier
 * windows (gain transitions, multipath, a wrong sync pick), a step in the
 * middle of the measurement. A wide VTX (deviation >= 1.15) must never
 * leave DEFAULT and a medium one (>= 0.95) must never reach kp 0.5;
 * clean narrow VTXs must still get their profile.
 *
 *   cc -O2 -std=c11 -I../main test_profile.c -lm -o test_profile && ./test_profile
 */
#include <math.h>
#include <stdio.h>
#include "profile_logic.h"
#include "kp_tables.h"

static uint64_t s_rng = 0x2545f4914f6cdd1dull;
static double uniform(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2.0 * log(uniform())) * cos(6.283185307179586 * uniform()); }

typedef struct {
    double dev, noise_pct, outlier_frac, step_to;   /* step_to > 0: deviation changes halfway */
} scenario_t;

/* Feeds spans until a decision or 2000 windows (a startup budget of about
 * 15 s of burst-confirmed AFC windows). Returns the profile, -1 = none. */
static int run(const scenario_t *sc, int *dev_out)
{
    dev_estimator_t e;
    dev_reset(&e);
    for (unsigned w = 0; w < 2000u; ++w) {
        double dev = sc->step_to > 0 && w >= 48u ? sc->step_to : sc->dev;
        double span = dev * DEV_SYNC_KHZ * (1.0 + sc->noise_pct / 100.0 * gauss());
        if (uniform() < sc->outlier_frac) span = 300.0 + 3500.0 * uniform();   /* anything */
        int r = dev_add(&e, (int32_t)span);
        if (r >= 0) { *dev_out = r; return profile_class(r, true, true); }
    }
    *dev_out = -1;
    return -1;
}

static int s_fail;
static void check(const char *name, bool ok, const char *detail)
{
    printf("%-4s %-58s %s\n", ok ? "ok" : "FAIL", name, detail);
    s_fail += !ok;
}

int main(void)
{
    static const char *const names[] = {"DEFAULT", "KP0.6", "KP0.5", "WIDE"};
    char name[96], detail[96];
    /* Wide and medium VTXs, adversarial conditions, 2000 trials each. */
    const double devs[] = {0.95, 1.0, 1.15, 1.2, 1.4, 1.45, 1.8};
    const double noises[] = {2.0, 5.0, 8.0, 15.0};
    const double outliers[] = {0.0, 0.1, 0.3};
    for (unsigned a = 0; a < 7u; ++a)
        for (unsigned b = 0; b < 4u; ++b)
            for (unsigned c = 0; c < 3u; ++c) {
                scenario_t sc = {devs[a], noises[b], outliers[c], 0.0};
                unsigned count[PROFILES] = {0}, none = 0, worst = PROFILE_DEFAULT;
                for (unsigned t = 0; t < 2000u; ++t) {
                    int dev, p = run(&sc, &dev);
                    if (p < 0) { ++none; continue; }
                    ++count[p];
                    if (p == PROFILE_KP050 || (p == PROFILE_KP060 && worst == PROFILE_DEFAULT)) worst = (unsigned)p;
                }
                /* Never narrow on a wide VTX, never kp 0.5 on a medium one,
                 * never WIDE below 1.45 (it would cost ~0.5-0.8 dB there). */
                bool ok = (devs[a] >= 1.15 ? count[PROFILE_KP060] + count[PROFILE_KP050] == 0
                                           : count[PROFILE_KP050] == 0) &&
                          (devs[a] > 1.45 || count[PROFILE_WIDE] == 0);
                snprintf(name, sizeof(name), "dev=%.2f noise=%2.0f%% outliers=%2.0f%%", devs[a], noises[b],
                         outliers[c] * 100);
                snprintf(detail, sizeof(detail), "default=%u kp06=%u kp05=%u wide=%u undecided=%u", count[0],
                         count[1], count[2], count[3], none);
                check(name, ok, detail);
            }
    /* A VTX that changes mid-measurement (channel neighbour, camera switch)
     * must not yield a mixed, narrower result. */
    for (unsigned t = 0, bad = 0;; ++t) {
        if (t == 2000u) { check("dev 1.40 -> 0.69 halfway: never narrow from mixed data", bad == 0, ""); break; }
        scenario_t sc = {1.4, 3.0, 0.0, 0.69};
        int dev, p = run(&sc, &dev);
        /* A decision must come from the new VTX only (all three batches). */
        if (p >= 0 && dev > 75) ++bad;
    }
    /* Clean narrow VTXs still get their profile. */
    const struct { double dev; int want; } clean[] = {{0.69, PROFILE_KP050}, {0.75, PROFILE_KP050},
                                                     {1.0, PROFILE_KP060}, {1.02, PROFILE_KP060}, {1.4, PROFILE_DEFAULT},
                                                     {1.7, PROFILE_WIDE}, {2.0, PROFILE_WIDE}};
    for (unsigned a = 0; a < 7u; ++a) {
        unsigned hit = 0;
        for (unsigned t = 0; t < 500u; ++t) {
            scenario_t sc = {clean[a].dev, 3.0, 0.05, 0.0};
            int dev;
            hit += run(&sc, &dev) == clean[a].want;
        }
        snprintf(name, sizeof(name), "clean dev=%.2f -> %s", clean[a].dev, names[clean[a].want]);
        snprintf(detail, sizeof(detail), "%u/500", hit);
        check(name, hit >= 490u, detail);
    }
    /* No narrow filter calibrated: always DEFAULT. */
    check("no narrow filter -> DEFAULT", profile_class(69, false, true) == PROFILE_DEFAULT, "");
    check("no wide filter -> DEFAULT", profile_class(180, true, false) == PROFILE_DEFAULT, "");
    check("unknown deviation -> DEFAULT", profile_class(0, true, true) == PROFILE_DEFAULT, "");
    /* Continuous VTX matching (report only): one cycle every 30 s. */
    {
        dev_monitor_t m;
        dev_monitor_reset(&m, 0);
        check("monitor: first cycle collects at once", m.collecting && !dev_monitor_tick(&m, 1000), "");
        dev_monitor_decided(&m, 100, PROFILE_KP060, PROFILE_DEFAULT, 6000000);
        check("monitor: idle until 30 s after the cycle start", !dev_monitor_tick(&m, 29999999) && !m.collecting, "");
        check("monitor: next cycle at 30 s", dev_monitor_tick(&m, 30000000) && m.collecting, "");
        check("monitor: no timeout before 120 s", !dev_monitor_tick(&m, 149999999), "");
        check("monitor: a cycle without a decision restarts after 120 s",
              dev_monitor_tick(&m, 150000000) && m.timeouts == 1u, "");
        dev_monitor_decided(&m, 101, PROFILE_KP060, PROFILE_DEFAULT, 160000000);
        check("monitor: agree and mismatch streaks", m.agree_streak == 2u && m.mismatch_streak == 2u, "");
        dev_monitor_decided(&m, 140, PROFILE_DEFAULT, PROFILE_DEFAULT, 200000000);
        check("monitor: a matching result clears the mismatch streak",
              m.agree_streak == 1u && m.mismatch_streak == 0u && m.decisions == 3u, "");
        /* NVS wear policy. */
        check("store: class change at once", dev_monitor_should_store(100, 170, true, 0, 1), "");
        check("store: small change never", !dev_monitor_should_store(100, 102, false, 0, 600000000), "");
        check("store: larger change waits for the gap", !dev_monitor_should_store(100, 104, false, 0, 299999999) &&
              dev_monitor_should_store(100, 104, false, 0, 300000000), "");
        check("store: unchanged or invalid never", !dev_monitor_should_store(100, 100, true, 0, 1) &&
              !dev_monitor_should_store(100, -1, true, 0, 1), "");
    }
    /* Ten minutes of live windows (50 ms, 60 % pass the gates, 5 % noise,
     * 5 % outliers); the VTX widens from 1.00 to 1.70 after five minutes.
     * Every cycle reports; after the change the wide VTX is reported within
     * two cycles, and no decision mixes the two VTXs into a narrow one. */
    {
        dev_monitor_t m;
        dev_estimator_t e;
        dev_monitor_reset(&m, 0);
        dev_reset(&e);
        unsigned before = 0, after = 0, mixed = 0;
        int64_t first_wide = -1;
        for (int64_t t = 0; t < 600000000LL; t += 50000) {
            if (dev_monitor_tick(&m, t)) dev_reset(&e);
            if (!m.collecting || uniform() > 0.6) continue;
            const double dev = t < 300000000LL ? 1.0 : 1.7;
            double span = dev * DEV_SYNC_KHZ * (1.0 + 0.05 * gauss());
            if (uniform() < 0.05) span = 300.0 + 3500.0 * uniform();
            int r = dev_add(&e, (int32_t)span);
            if (r < 0) continue;
            int want = profile_class(r, true, true);
            dev_monitor_decided(&m, r, want, PROFILE_KP060, t);
            if (t < 300000000LL) before += want == PROFILE_KP060;
            else {
                after += want == PROFILE_WIDE;
                if (first_wide < 0 && want == PROFILE_WIDE) first_wide = t;
                mixed += want == PROFILE_KP050;
            }
        }
        snprintf(detail, sizeof(detail), "decisions=%u timeouts=%u kp06_before=%u wide_after=%u first_wide_s=%.1f",
                 m.decisions, m.timeouts, before, after, first_wide / 1e6);
        check("live 10 min: a report every 30 s", m.decisions >= 19u && m.timeouts == 0u, detail);
        check("live 10 min: widened VTX reported within two cycles",
              first_wide >= 0 && first_wide - 300000000LL <= 60000000LL + 15000000LL && mixed == 0u, detail);
        check("live 10 min: mismatch streak counts the wide reports", m.mismatch_streak >= 8u, detail);
    }
    /* Live profile switch: widen at once, narrow after two agreeing results. */
    check("live switch: report-only never switches",
          !profile_live_switch_wanted(false, PROFILE_DEFAULT, PROFILE_KP050, 5u), "");
    check("live switch: narrow -> DEFAULT on the first result",
          profile_live_switch_wanted(true, PROFILE_DEFAULT, PROFILE_KP050, 1u), "");
    check("live switch: DEFAULT -> WIDE on the first result",
          profile_live_switch_wanted(true, PROFILE_WIDE, PROFILE_DEFAULT, 1u), "");
    check("live switch: kp0.6 -> kp0.5 waits for agreement",
          !profile_live_switch_wanted(true, PROFILE_KP050, PROFILE_KP060, 1u) &&
          profile_live_switch_wanted(true, PROFILE_KP050, PROFILE_KP060, 2u), "");
    check("live switch: WIDE -> DEFAULT waits for agreement",
          !profile_live_switch_wanted(true, PROFILE_DEFAULT, PROFILE_WIDE, 1u) &&
          profile_live_switch_wanted(true, PROFILE_DEFAULT, PROFILE_WIDE, 2u), "");
    check("live switch: same or invalid profile never",
          !profile_live_switch_wanted(true, PROFILE_DEFAULT, PROFILE_DEFAULT, 9u) &&
          !profile_live_switch_wanted(true, -1, PROFILE_DEFAULT, 9u), "");
    /* The 10-minute run again with the switch applied: a VTX that widens
     * from 0.69 to 1.70 leaves the narrow profile on the first wide result,
     * and a clean narrow VTX is entered only after two agreeing results. */
    {
        dev_monitor_t m;
        dev_estimator_t e;
        dev_monitor_reset(&m, 0);
        dev_reset(&e);
        int active = PROFILE_DEFAULT;
        unsigned switches = 0, narrow_entries = 0;
        int64_t narrow_at = -1, left_at = -1;
        for (int64_t t = 0; t < 600000000LL; t += 50000) {
            if (dev_monitor_tick(&m, t)) dev_reset(&e);
            if (!m.collecting || uniform() > 0.6) continue;
            const double dev = t < 300000000LL ? 0.69 : 1.7;
            double span = dev * DEV_SYNC_KHZ * (1.0 + 0.05 * gauss());
            if (uniform() < 0.05) span = 300.0 + 3500.0 * uniform();
            int r = dev_add(&e, (int32_t)span);
            if (r < 0) continue;
            int want = profile_class(r, true, true);
            dev_monitor_decided(&m, r, want, active, t);
            if (profile_live_switch_wanted(true, want, active, m.agree_streak)) {
                if (profile_width_rank(want) < profile_width_rank(active)) { ++narrow_entries; if (narrow_at < 0) narrow_at = t; }
                else if (left_at < 0 && t >= 300000000LL) left_at = t;
                active = want;
                ++switches;
            }
        }
        snprintf(detail, sizeof(detail), "switches=%u narrow_at_s=%.1f left_narrow_at_s=%.1f", switches,
                 narrow_at / 1e6, left_at / 1e6);
        check("live switch run: narrow entered once, after the second result",
              narrow_entries == 1u && narrow_at >= 30000000LL, detail);
        check("live switch run: wide VTX leaves the narrow profile within one cycle",
              left_at >= 0 && left_at - 300000000LL <= 30000000LL + 15000000LL && active == PROFILE_WIDE, detail);
        check("live switch run: no flapping", switches == 2u, detail);
    }
    {   /* A profile is a filter plus a start level of the one U85Q program: the
         * old KP060/KP050 programs' LUTs are these ladder levels bit-exactly. */
        bool ok = true;
        for (int p = 0; p < PROFILES; ++p) {
            int lv = profile_base_level_for(p);
            ok &= lv >= 0 && lv < KP_LEVELS && kp_level_x100[lv] == profile_base_kp_x100(p);
        }
        check("profile start levels: DEFAULT/WIDE 0.85, KP060 0.60, KP050 0.50",
              ok && profile_base_level_for(PROFILE_KP050) == 7 && profile_base_level_for(PROFILE_KP060) == 5 &&
              profile_base_level_for(PROFILE_DEFAULT) == 0 && profile_base_level_for(PROFILE_WIDE) == 0, "");
    }
    printf("%s (%d failures)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail != 0;
}
