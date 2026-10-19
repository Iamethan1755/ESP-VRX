/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the verified
 * kp ladder policy (main/kp_ladder.h) and of the LUT tables it moves
 * between (main/kp_tables.h).
 *
 *   cc -O2 -std=c11 -I../main test_kp_ladder.c -o t && ./t
 */
#include <stdio.h>
#include <stdlib.h>
#include "kp_ladder.h"
#include "kp_tables.h"
#include "kp_switch_guard.h"

/* U85Q-only ladder (K mode 1): the joint update with LP disallowed. */
static uint8_t kp_ladder_update(kp_ladder_t *l, int cn_x10, bool ok, int dev, int64_t t)
{
    return kp_ladder_update_lp(l, cn_x10, ok, dev, t, false);
}
static uint8_t kp_ladder_target(int cn_x10, int dev) { return kp_ladder_target_lp(cn_x10, dev, false); }

static unsigned s_fail, s_run;
static void check(const char *name, int ok)
{
    ++s_run;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
    s_fail += !ok;
}

#define TICK_US 50000LL

/* Runs `seconds` of control windows at a C/N (x10) with estimate noise. */
static uint8_t run(kp_ladder_t *l, int cn_x10, int noise_x10, int dev, double seconds, int64_t *t, unsigned *changes)
{
    uint8_t last = l->level;
    for (int64_t end = *t + (int64_t)(seconds * 1e6); *t < end; *t += TICK_US) {
        int cn = cn_x10 + (noise_x10 ? rand() % (2 * noise_x10 + 1) - noise_x10 : 0);
        uint8_t lv = kp_ladder_update(l, cn, true, dev, *t);
        if (changes && lv != last) ++*changes;
        last = lv;
    }
    return l->level;
}

int main(void)
{
    char name[128];
    /* Tables: same token bits at every level, level 0 differs from 1. */
    int tokens_equal = 1, differ = 0;
    for (int lv = 1; lv < KP_LEVELS; ++lv)
        for (int k = 0; k < 1024; ++k) {
            tokens_equal &= (kp_tables[lv][k] >> 11) == (kp_tables[0][k] >> 11);
            differ += kp_tables[lv][k] != kp_tables[0][k];
        }
    check("every level keeps the token bits (one shared controller interpretation)", tokens_equal);
    check("levels differ in the tracker half", differ > 0);
    check("ladder and tables agree on the level count", KP_LEVELS == KP_LADDER_LEVELS);

    kp_ladder_t l;
    int64_t t = 0;
    /* Curves (best_combo.py): kp at a C/N per deviation class. */
    check("deviation 1.0, 20 dB C/N: kp 0.75", kp_level_x100[kp_ladder_target(200, 100)] == 75);
    check("deviation 1.0, 4..8 dB C/N: kp 0.55", kp_level_x100[kp_ladder_target(40, 100)] == 55 &&
                                              kp_level_x100[kp_ladder_target(80, 100)] == 55);
    check("deviation 1.0 near the floor: back to kp 0.75 (lock)", kp_level_x100[kp_ladder_target(0, 100)] == 75);
    check("deviation 0.69, 8 dB C/N: kp 0.50", kp_level_x100[kp_ladder_target(80, 69)] == 50);
    check("wide VTX, any C/N: never below kp 0.80", kp_level_x100[kp_ladder_target(40, 140)] == 80 &&
                                                 kp_level_x100[kp_ladder_target(0, 140)] == 85);
    check("unknown deviation = wide row", kp_ladder_target(40, -1) == kp_ladder_target(40, 140));
    int deepest = 0;
    for (int dev = 0; dev <= 300; dev += 5)
        for (int cn = -50; cn <= 300; cn += 5) {
            int lv = kp_ladder_target(cn, dev);
            if (kp_ladder_class(dev) == KP_CLASS_WIDE && lv > deepest) deepest = lv;
        }
    check("wide row never goes deeper than kp 0.80", kp_level_x100[deepest] >= 80);
    /* Strong carrier, normal VTX: one step to kp 0.80 then 0.75. */
    kp_ladder_reset(&l);
    uint8_t lv = run(&l, 200, 15, 100, 20, &t, NULL);
    check("strong carrier, deviation 1.0: settles on kp 0.75", kp_level_x100[lv] == 75);
    /* Range edge (6 dB), normal VTX: walks down one level at a time. */
    unsigned changes = 0;
    lv = run(&l, 60, 10, 100, 20, &t, &changes);
    snprintf(name, sizeof(name), "6 dB C/N, deviation 1.0: kp 0.%02u in %u single steps", kp_level_x100[lv], changes);
    check(name, kp_level_x100[lv] == 55 && changes == 4);
    check("carrier strong again: back to kp 0.75", kp_level_x100[run(&l, 200, 15, 100, 20, &t, NULL)] == 75);
    check("deviation 0.69 at 6 dB: kp 0.50", kp_level_x100[run(&l, 60, 10, 69, 20, &t, NULL)] == 50);
    kp_ladder_reset(&l);
    check("deviation 1.4 at 6 dB: kp 0.80", kp_level_x100[run(&l, 60, 10, 140, 20, &t, NULL)] == 80);
    /* Deviation becomes unknown (channel change) while deep: back at once. */
    kp_ladder_reset(&l);
    (void)run(&l, 60, 0, 69, 20, &t, NULL);
    lv = kp_ladder_update(&l, 60, true, -1, t);
    check("deviation lost while deep: kp 0.80 immediately", kp_level_x100[lv] == 80);
    /* No hopping: C/N noise of +-3 dB around 8 dB stays within two levels. */
    kp_ladder_reset(&l);
    (void)run(&l, 100, 0, 100, 10, &t, NULL);
    uint8_t lo = l.level, hi = l.level, mid = l.level;
    for (int k = 0; k < 1200; ++k, t += TICK_US) {     /* 60 s */
        uint8_t v = kp_ladder_update(&l, 100 + rand() % 61 - 30, true, 100, t);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    snprintf(name, sizeof(name), "C/N 10 dB +-3 dB noise for 60 s: level %u..%u around %u", lo, hi, mid);
    check(name, hi - lo <= 2);
    /* Collapse: deep (kp 0.50 at 6 dB, deviation 0.69), then the estimate
     * goes noise-like. The ladder must climb back to the floor point (0.70),
     * not freeze at 0.50. */
    kp_ladder_reset(&l);
    (void)run(&l, 60, 0, 69, 20, &t, NULL);
    int64_t t0 = t, back = -1;
    for (int k = 0; k < 100; ++k, t += TICK_US) {     /* 5 s */
        uint8_t v = kp_ladder_update(&l, CN_UNKNOWN_X10, true, 69, t);
        if (back < 0 && kp_level_x100[v] == 70) back = t - t0;
    }
    snprintf(name, sizeof(name), "collapse to noise-like: back to kp 0.%02u after %.2f s", kp_level_x100[l.level],
             back / 1e6);
    check(name, kp_level_x100[l.level] == 70 && back >= 0 && back <= 1500000);
    /* Unusable windows (retune, gain write, menu) hold the level. */
    kp_ladder_reset(&l);
    (void)run(&l, 60, 0, 100, 20, &t, NULL);
    uint8_t held = l.level;
    for (int k = 0; k < 200; ++k, t += TICK_US) (void)kp_ladder_update(&l, CN_UNKNOWN_X10, false, 100, t);
    check("unusable windows: level held", l.level == held);
    /* Window usability: settle, context and a recent gain write only (control.c). */
    check("usable: settled, same context, no gain write for 5 ms",
          kp_ladder_window_usable(true, true, KP_GAIN_WRITE_GUARD_US) && KP_GAIN_WRITE_GUARD_US == 5000);
    check("unusable: settling, context change, gain write within 5 ms",
          !kp_ladder_window_usable(false, true, 1000000) && !kp_ladder_window_usable(true, false, 1000000) &&
          !kp_ladder_window_usable(true, true, KP_GAIN_WRITE_GUARD_US - 1));
    /* A short unusable gap keeps a pending LP change (board walk 2026-10-13:
     * one unusable window used to restart the 1 s hold). Wide class, 5 dB:
     * LP is wanted; every fourth window unusable. */
    kp_ladder_reset(&l);
    (void)run(&l, 200, 0, 0, 40, &t, NULL);            /* settle on a strong carrier, U85Q */
    int64_t lp_at = -1;
    for (int k = 0; k < 200 && lp_at < 0; ++k, t += TICK_US) {
        (void)kp_ladder_update_lp(&l, 50, (k & 3) != 3, 0, t, true);
        if (l.lp) lp_at = t;
    }
    check("LP through 25 % unusable windows", lp_at >= 0);
    /* A long gap drops the pending change: no switch on stale evidence. */
    /* The smoothed C/N passes 8.5 dB after ~11 windows, so the hold starts
     * then; without the restart 4 + 12 + 4 windows would pass the 0.3 s. */
    kp_ladder_reset(&l);
    (void)run(&l, 200, 0, 0, 80, &t, NULL);
    for (int k = 0; k < 15; ++k, t += TICK_US) (void)kp_ladder_update_lp(&l, 50, true, 0, t, true);
    for (int k = 0; k < 12; ++k, t += TICK_US) (void)kp_ladder_update_lp(&l, 50, false, 0, t, true);
    bool early = l.lp;
    for (int k = 0; k < 4; ++k, t += TICK_US) {    /* 0.15 s < KP_LP_ENTER_HOLD_US */
        (void)kp_ladder_update_lp(&l, 50, true, 0, t, true);
        early |= l.lp;
    }
    check("a gap over KP_STALE_US restarts the LP hold", !early);
    /* Step spacing: never two steps closer than the gap. */
    kp_ladder_reset(&l);
    int64_t last = -KP_STEP_GAP_US, min_gap = 1LL << 62;
    uint8_t prev = 0;
    for (int k = 0; k < 600; ++k, t += TICK_US) {
        uint8_t v = kp_ladder_update(&l, k < 300 ? 60 : 200, true, 69, t);
        if (v != prev) { if (t - last < min_gap) min_gap = t - last; last = t; prev = v; }
    }
    check("steps at least KP_STEP_GAP_UP_US apart", min_gap >= KP_STEP_GAP_UP_US);
    /* Joint kp + LP ladder (kp_ladder_update_lp). */
    {
        kp_ladder_t j;
        kp_ladder_reset(&j);
        int64_t tj = 0;
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
        check("joint, deviation 1.0, 20 dB: U85Q kp 0.75", !j.lp && kp_level_x100[j.level] == 75);
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
        snprintf(name, sizeof(name), "joint, deviation 1.0, 4 dB: %s kp 0.%02u", j.lp ? "LP" : "U85Q", kp_level_x100[j.level]);
        check(name, j.lp && kp_level_x100[j.level] == 65);
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
        check("joint, back to 20 dB: U85Q kp 0.75", !j.lp && kp_level_x100[j.level] == 75);
        /* C/N wandering 5..9 dB around the switch point: few program switches. */
        uint32_t sw0 = j.lp_switches;
        for (int k = 0; k < 1200; ++k, tj += TICK_US)       /* 60 s */
            (void)kp_ladder_update_lp(&j, 50 + rand() % 41, true, 100, tj, true);
        snprintf(name, sizeof(name), "joint, C/N 5..9 dB random for 60 s: %u program switches", j.lp_switches - sw0);
        check(name, j.lp_switches - sw0 <= 6);
        for (int k = 0; k < 400; ++k, tj += TICK_US)        /* 20 s at 2..8 dB (mean 5) */
            (void)kp_ladder_update_lp(&j, 20 + rand() % 61, true, 100, tj, true);
        check("joint, C/N 2..8 dB random: LP", j.lp);
        /* Wide VTX at the floor: U85Q keeps lock better. */
        kp_ladder_reset(&j);
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 50, true, 140, tj, true);
        check("joint, wide VTX at 5 dB: LP kp 0.85", j.lp && kp_level_x100[j.level] == 85);
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, CN_UNKNOWN_X10, true, 140, tj, true);
        check("joint, wide VTX at the noise floor: back to U85Q", !j.lp);
        /* Not allowed: never LP, and leaves LP at once when disallowed. */
        kp_ladder_reset(&j);
        for (int k = 0; k < 400; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
        (void)kp_ladder_update_lp(&j, 40, true, 100, tj, false);
        check("joint disallowed: U85Q immediately", !j.lp);
        int64_t last = -1, min_gap = 1LL << 62;
        bool prev = j.lp;
        kp_ladder_reset(&j);
        for (int k = 0; k < 4000; ++k, tj += TICK_US) {
            (void)kp_ladder_update_lp(&j, (k / 30) % 2 ? 30 : 150, true, 100, tj, true);
            if (j.lp != prev) { if (last >= 0 && tj - last < min_gap) min_gap = tj - last; last = tj; prev = j.lp; }
        }
        check("program switches at least KP_LP_ENTER_GAP_US apart", min_gap >= KP_LP_ENTER_GAP_US);
        /* Rescue fast, release slow: into LP within ~1 s of a drop to 4 dB
         * (smoothing + hold), out only after 5 s of a good carrier, and a
         * 1.5 s recovery in between does not throw LP out. */
        kp_ladder_reset(&j);
        for (int k = 0; k < 200; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
        int64_t drop = tj, in_at = -1;
        for (int k = 0; k < 60 && in_at < 0; ++k, tj += TICK_US) {
            (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
            if (j.lp) in_at = tj - drop;
        }
        check("rescue: LP within 1 s of a drop to 4 dB", in_at >= 0 && in_at <= 1000000);
        for (int k = 0; k < 30; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
        check("a 1.5 s recovery keeps LP", j.lp);
        for (int k = 0; k < 40; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
        int64_t good = tj, out_at = -1;
        for (int k = 0; k < 200 && out_at < 0; ++k, tj += TICK_US) {
            (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
            if (!j.lp) out_at = tj - good;
        }
        check("release on a calm link: U85Q after ~2 s of a good carrier",
              out_at >= KP_LP_RELEASE_BASE_US && out_at <= 3000000);
        /* Flapping: LP needed again soon after leaving doubles the hold. */
        for (int k = 0; k < 40; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
        check("flap: back in LP, release hold doubled", j.lp && j.release_hold_us == 2 * KP_LP_RELEASE_BASE_US);
        good = tj; out_at = -1;
        for (int k = 0; k < 400 && out_at < 0; ++k, tj += TICK_US) {
            (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
            if (!j.lp) out_at = tj - good;
        }
        check("flap: release takes the doubled hold", out_at >= 2 * KP_LP_RELEASE_BASE_US);
        for (int k = 0; k < 700; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 200, true, 100, tj, true);
        for (int k = 0; k < 40; ++k, tj += TICK_US) (void)kp_ladder_update_lp(&j, 40, true, 100, tj, true);
        check("calm for 30 s: hold back to 2 s", j.lp && j.release_hold_us == KP_LP_RELEASE_BASE_US);
    }
    /* Envelope C/N model (cn_estimate.h, research/cn_envelope.py medians). */
    check("envelope: noise (115) is unknown, 12 MHz 262 -> 4 dB, 1543 -> 20 dB",
          cn_estimate_env_x10(115, 12) == CN_UNKNOWN_X10 && cn_estimate_env_x10(262, 12) == 40 &&
          cn_estimate_env_x10(1543, 12) == 200 && cn_estimate_env_x10(5000, 12) == 200);
    check("envelope: monotonic across the 12 MHz table",
          cn_estimate_env_x10(300, 12) < cn_estimate_env_x10(600, 12) &&
          cn_estimate_env_x10(600, 12) < cn_estimate_env_x10(1000, 12));
    {
        const int filters[] = {10, 12, 16, 20};
        const int weak[] = {222, 204, 178, 161};
        const int gate[] = {395, 355, 298, 255};
        const int strong[] = {532, 479, 403, 342};
        bool monotonic = true, afc_gate = true;
        for (unsigned f = 0; f < 4u; ++f) {
            int previous = CN_UNKNOWN_X10;
            for (int ratio = 0; ratio <= 65535; ++ratio) {
                int cn = cn_estimate_env_x10(ratio, filters[f]);
                if (cn < previous) monotonic = false;
                previous = cn;
            }
            afc_gate = afc_gate && cn_estimate_env_x10(weak[f], filters[f]) < AFC_MIN_CN_X10 &&
                       cn_estimate_env_x10(gate[f] - 1, filters[f]) < AFC_MIN_CN_X10 &&
                       cn_estimate_env_x10(gate[f], filters[f]) == AFC_MIN_CN_X10 &&
                       cn_estimate_env_x10(strong[f], filters[f]) >= AFC_MIN_CN_X10;
        }
        check("envelope: monotonic over uint16 range for all filters", monotonic);
        check("envelope: AFC rejects weak carriers and opens at 6 dB for all filters", afc_gate);
    }
    {   /* LP by gain: in at G70, out below G65, whatever the C/N model says. */
        kp_ladder_t gl;
        int64_t tg = 0;
        kp_ladder_reset(&gl);
        gl.gain = 30;
        for (int k = 0; k < 100; ++k, tg += TICK_US) (void)kp_ladder_update_lp(&gl, 40, true, 0, tg, true);
        check("gain G30, model 4 dB: U85Q (strong carrier)", !gl.lp);
        gl.gain = 72;
        for (int k = 0; k < 20; ++k, tg += TICK_US) (void)kp_ladder_update_lp(&gl, 200, true, 0, tg, true);
        check("gain G72, model 20 dB: LP within 1 s", gl.lp);
        gl.gain = 67;
        for (int k = 0; k < 200; ++k, tg += TICK_US) (void)kp_ladder_update_lp(&gl, 200, true, 0, tg, true);
        check("gain G67: stays LP (hysteresis)", gl.lp);
        gl.gain = 60;
        for (int k = 0; k < 100; ++k, tg += TICK_US) (void)kp_ladder_update_lp(&gl, 200, true, 0, tg, true);
        check("gain G60: back to U85Q after the release hold", !gl.lp);
    }
    {   /* Gain policy reaches the real switch guard even with no usable IQ:
         * failed copies, unknown C/N, or continuous gain writes. */
        kp_ladder_t gl;
        kp_switch_guard_t guard = {0};
        kp_ladder_reset(&gl);
        gl.gain = 70;
        gl.level = 1;
        bool applied_lp = false;
        int64_t applied_at = -1;
        for (int64_t t = 1000000; t <= 2000000; t += TICK_US) {
            uint8_t level = kp_ladder_update_evidence(&gl, CN_UNKNOWN_X10, false, true, 0, t, true);
            if (kp_switch_confirm_toward(&guard, level, gl.lp, 1, applied_lp, 7, true, t)) {
                applied_lp = gl.lp;
                applied_at = t;
            }
        }
        check("G70 with no usable IQ: LP confirmed within 1 s", applied_lp && applied_at <= 2000000);
        check("missing IQ cannot invent C/N or move kp", !gl.cn_init && gl.level == 1);
        for (int64_t t = 2050000; t <= 4000000; t += TICK_US) {
            gl.gain = t / TICK_US % 2 ? 65 : 83;
            (void)kp_ladder_update_evidence(&gl, 200, false, true, 0, t, true);
        }
        check("G65-G83 with no usable IQ: stays LP even if C/N says 20 dB", gl.lp);
        gl.gain = 64;
        for (int64_t t = 4050000; t <= 10000000; t += TICK_US)
            (void)kp_ladder_update_evidence(&gl, 200, false, true, 0, t, true);
        check("G64 with no usable IQ: releases after the hold", !gl.lp);
        kp_ladder_reset(&gl);
        gl.gain = 83;
        for (int64_t t = 1000000; t <= 3000000; t += TICK_US)
            (void)kp_ladder_update_evidence(&gl, 200, false, false, 0, t, true);
        check("PHY/retune ownership still blocks gain-driven LP", !gl.lp);
        for (int64_t t = 3050000; t <= 4000000; t += TICK_US)
            (void)kp_ladder_update_evidence(&gl, 200, false, true, 0, t, true);
        check("LP starts once ownership is released, without IQ", gl.lp);
        (void)kp_ladder_update_evidence(&gl, 200, false, true, 0, 4050000, false);
        check("manual/no-LP mode still disables LP", !gl.lp);
        kp_ladder_reset(&gl);
        for (int64_t t = 1000000; t <= 3000000; t += TICK_US)
            (void)kp_ladder_update_evidence(&gl, 40, false, true, 0, t, true);
        check("unknown gain plus missing IQ cannot choose LP", !gl.lp);
    }
    {   /* Narrow 10 MHz filter (MATCHED profiles): own curves, LP still allowed. */
        kp_ladder_t n;
        int64_t tn = 0;
        kp_ladder_reset(&n);
        n.narrow = true;
        for (int k = 0; k < 400; ++k, tn += TICK_US) (void)kp_ladder_update_lp(&n, 200, true, 100, tn, true);
        check("narrow, deviation 1.0, 20 dB: U85Q kp 0.85", !n.lp && kp_level_x100[n.level] == 85);
        for (int k = 0; k < 400; ++k, tn += TICK_US) (void)kp_ladder_update_lp(&n, 40, true, 100, tn, true);
        check("narrow, deviation 1.0, 4 dB: LP kp 0.70", n.lp && kp_level_x100[n.level] == 70);
        kp_ladder_reset(&n);
        n.narrow = true;
        for (int k = 0; k < 400; ++k, tn += TICK_US) (void)kp_ladder_update_lp(&n, 120, true, 69, tn, true);
        check("narrow, deviation 0.69, 12 dB: U85Q kp 0.50", !n.lp && kp_level_x100[n.level] == 50);
        for (int k = 0; k < 400; ++k, tn += TICK_US) (void)kp_ladder_update_lp(&n, 30, true, 69, tn, true);
        check("narrow, deviation 0.69, 3 dB: LP kp 0.50", n.lp && kp_level_x100[n.level] == 50);
        check("narrow flag leaves mid/wide rows on the 12 MHz curves",
              kp_ladder_target_f(40, 140, false, true) == kp_ladder_target_lp(40, 140, false) &&
              kp_ladder_target_f(40, 110, true, true) == kp_ladder_target_lp(40, 110, true));
        check("reset clears the narrow flag", (kp_ladder_reset(&n), !n.narrow));
        check("WIDE profile rows: U85Q kp 0.85 close in (not 0.80)",
              kp_level_x100[kp_ladder_target_g(200, 170, false, false, true)] == 85 &&
              kp_level_x100[kp_ladder_target_g(200, 170, false, false, false)] == 80);
        check("WIDE profile rows: LP wanted up to 9.5 dB, 12 MHz wide row only below 8.5",
              kp_ladder_want_lp_g(KP_CLASS_WIDE, 90, false, true) &&
              !kp_ladder_want_lp_g(KP_CLASS_WIDE, 90, false, false) &&
              !kp_ladder_want_lp_g(KP_CLASS_WIDE, 96, false, true));
        check("WIDE profile rows leave the narrow classes alone",
              kp_ladder_target_g(40, 69, false, false, true) == kp_ladder_target_lp(40, 69, false) &&
              kp_ladder_want_lp_g(KP_CLASS_LOW, 70, false, true) == kp_ladder_want_lp(KP_CLASS_LOW, 70, false));
        kp_ladder_t wl;
        kp_ladder_reset(&wl);
        wl.wide = true;
        for (int64_t t = 0; t < 4000000LL; t += 50000) (void)kp_ladder_update_lp(&wl, 90, true, 170, t, true);
        check("WIDE ladder at 9 dB: LP kp 0.85", wl.lp && kp_level_x100[wl.level] == 85);
        check("reset clears the wide flag", (kp_ladder_reset(&wl), !wl.wide));
        check("narrow rows: MATCHED with a ~10 MHz filter or an unknown width",
              kp_ladder_narrow_rows(true, 10000u) && kp_ladder_narrow_rows(true, 11000u) &&
              kp_ladder_narrow_rows(true, 0u));
        check("narrow rows: not for the board's 11.9 MHz narrow entry, nor DEFAULT",
              !kp_ladder_narrow_rows(true, 11875u) && !kp_ladder_narrow_rows(false, 10000u));
    }
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
