#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_gain_v3.h"

unsigned char phy_param[0x800];

static dg3_observation_t obs(int p50, int p95, int origin, int clip,
                              int coherence, uint64_t us)
{
    return (dg3_observation_t){
        .p50 = (uint8_t)p50, .p95 = (uint8_t)p95,
        .origin_pm = (uint16_t)origin, .clip_pm = (uint16_t)clip,
        .coherence = (uint8_t)coherence, .observed_us = us,
    };
}

/* Radius-boost plant: every state is measured (0.5 dB per index), the ring
 * power follows the gain exactly, spread and rail codes are scenario input. */
static int boost_p50(const direct_gain_v3_t *v3, double base)
{
    double r = (double)v3->relative_power_q10[v3->current_gain] / 1024.0;
    int p = (int)(base * r + 0.5);
    return p > 113 ? 113 : p < 1 ? 1 : p;
}

static void boost_plant(direct_gain_v3_t *v3)
{
    for (unsigned g = 20u; g <= v3->table.max_index; ++g) {
        double db = ((double)g - 40.0) * 0.5;
        double r = 1.0;
        for (int k = 0; k < (int)(db * 100.0 + (db >= 0 ? 0.5 : -0.5)); ++k) r *= 1.0023052;
        for (int k = 0; k > (int)(db * 100.0 + (db >= 0 ? 0.5 : -0.5)); --k) r /= 1.0023052;
        v3->relative_power_q10[g] = (uint16_t)(r * 1024.0 + 0.5);
        v3->confidence[g] = 3u;
        v3->uncertainty_pm[g] = 40u;
    }
}

static dg3_observation_t boost_window(direct_gain_v3_t *v3, double base, int spread,
                                      int clip, int coherence, uint64_t us)
{
    int p50 = boost_p50(v3, base);
    int p95 = p50 + spread > 113 ? 113 : p50 + spread;
    dg3_observation_t o = obs(p50, p95, 0, clip, coherence, us);
    o.p90 = (uint8_t)(p50 + spread * 3 / 4);
    return o;
}

/* Run windows at 200 us; returns the number of gain writes. */
static unsigned boost_run(direct_gain_v3_t *v3, double base, int spread, int clip,
                          int coherence, unsigned windows, uint64_t *t)
{
    uint32_t w0 = v3->writes;
    for (unsigned w = 0; w < windows; ++w) {
        dg3_observation_t o = boost_window(v3, base, spread, clip, coherence, *t += 200u);
        uint8_t g = direct_gain_v3_tick(v3, &o);
        if (g != o.p50 && v3->state == DG3_SETTLE && v3->write_us == *t)
            direct_gain_v3_sync_applied(v3, g, *t);
    }
    return v3->writes - w0;
}

int main(void)
{
    arc_gain_table_t table;
    arc_gain_table_from_bytes(&table, NULL, 81u);
    direct_gain_v3_t v3;
    direct_gain_v3_reset(&v3, &table, 35u, 62u);

    uint8_t phase[256] = {0};
    uint8_t raw[256];
    memset(raw, 0x00, sizeof(raw));
    dg3_observation_t m = direct_gain_v3_measure(raw, sizeof(raw), phase, 1u);
    assert(m.p50 == 1u && m.p95 == 1u && m.origin_pm == 1000u);
    memset(raw, 0x77, sizeof(raw));
    m = direct_gain_v3_measure(raw, sizeof(raw), phase, 2u);
    assert(m.p50 == 113u && m.p95 == 113u && m.clip_pm == 1000u);

    /* A broad healthy envelope is a strict zero-write zone. */
    for (uint64_t us = 1000u; us < 100000u; us += 1000u) {
        dg3_observation_t good = obs(22, 40, 0, 0, 99, us);
        assert(direct_gain_v3_tick(&v3, &good) == 35u);
    }
    assert(v3.writes == 0u && v3.state == DG3_HOLD);

    /* A moderate excursion (P50 10, band 13..32) must hold for two windows
     * before a write: single multipath dips no longer move the gain. */
    dg3_observation_t weak = obs(10, 17, 200, 0, 90, 199000u);
    uint8_t next = direct_gain_v3_tick(&v3, &weak);
    assert(next == 35u && v3.writes == 0u);
    weak.observed_us += 200u;
    next = direct_gain_v3_tick(&v3, &weak);
    assert(next != 35u && v3.writes == 1u);
    /* Learning needs a stable pre-write pair; run the move once more from a
     * stable weak history to exercise it. */
    direct_gain_v3_reset(&v3, &table, 35u, 62u);
    dg3_observation_t pre = obs(22, 40, 0, 0, 99, 198000u);
    assert(direct_gain_v3_tick(&v3, &pre) == 35u);          /* in band */
    weak.observed_us = 199000u;
    weak.p50 = 22; weak.p95 = 40; weak.origin_pm = 0;       /* same as pre */
    assert(direct_gain_v3_tick(&v3, &weak) == 35u);          /* still in band */
    weak = obs(10, 17, 200, 0, 90, 199800u);
    v3.last_tracking = obs(10, 17, 200, 0, 90, 199500u);     /* stable weak prior */
    assert(direct_gain_v3_tick(&v3, &weak) == 35u);          /* first window holds */
    weak.observed_us = 200000u;
    next = direct_gain_v3_tick(&v3, &weak);
    assert(next != 35u && v3.writes == 1u &&
           v3.tuple[next].rf_stage == v3.tuple[35].rf_stage &&
           v3.tuple[next].bb_code == v3.tuple[35].bb_code);
    direct_gain_v3_sync_applied(&v3, next, 200000u);
    /* One stable window after the physical settle guard verifies. */
    dg3_observation_t settled = obs(17, 28, 30, 0, 91, 200600u);
    assert(direct_gain_v3_tick(&v3, &settled) == next);
    assert(v3.state == DG3_HOLD && v3.verified == 1u && v3.learned == 1u);
    /* ...and a steady carrier in band stays write-free. */
    uint32_t steady_writes = v3.writes;
    for (unsigned k = 0; k < 200u; ++k) {
        settled.observed_us += 1000u;
        assert(direct_gain_v3_tick(&v3, &settled) == next);
    }
    assert(v3.writes == steady_writes);
    assert(v3.confidence[next] > 0u && v3.settle_us[DG3_FINE] > 0u);

    /* Poor phase with a healthy envelope is not a reason to pump gain. */
    dg3_observation_t multipath = obs(20, 35, 50, 0, 20, 300000u);
    assert(direct_gain_v3_tick(&v3, &multipath) == next);
    assert(v3.writes == 1u);
    for (unsigned k = 0; k < 1000u; ++k) {
        multipath.observed_us += 200u;
        multipath.coherence = (uint8_t)(k % 101u);
        assert(direct_gain_v3_tick(&v3, &multipath) == next);
    }
    assert(v3.writes == 1u);

    /* Saturation drops sensitivity; carrier loss listens at maximum gain
     * (the table maximum, not the G62 survival trap). */
    dg3_observation_t clipped = obs(50, 105, 0, 200, 90, 600000u);
    uint8_t down = direct_gain_v3_tick(&v3, &clipped);
    assert(down != next && v3.overloads == 1u);
    dg3_observation_t lost = obs(1, 2, 950, 0, 0, 700000u);
    assert(direct_gain_v3_tick(&v3, &lost) == table.max_index);
    /* Still no carrier at maximum gain: stays there, no further writes. */
    uint32_t lost_writes = v3.writes;
    for (unsigned k = 0; k < 20u; ++k) {
        lost.observed_us += 5000u;
        assert(direct_gain_v3_tick(&v3, &lost) == table.max_index);
    }
    assert(v3.writes == lost_writes);
    /* A strong carrier appearing at maximum gain is dropped at once. */
    dg3_observation_t strong = obs(60, 110, 0, 300, 90, lost.observed_us + 5000u);
    assert(direct_gain_v3_tick(&v3, &strong) < table.max_index);

    /* Measured tuple response wins over numeric index order. G34 is marked
     * stronger than G35; G33 is the useful measured gain-down destination. */
    direct_gain_v3_reset(&v3, &table, 35u, 62u);
    v3.relative_power_q10[34] = 1200u;
    v3.relative_power_q10[33] = 620u;
    v3.confidence[34] = v3.confidence[33] = 3u;
    dg3_observation_t high = obs(41, 55, 0, 0, 95, 600000u) /* severe: acts on one window */;
    assert(direct_gain_v3_tick(&v3, &high) == 33u);          /* direct */

    /* At a Fine boundary, a known BB+Fine tuple is selected directly. */
    direct_gain_v3_reset(&v3, &table, 21u, 62u);
    assert(v3.tuple[21].rf_stage == v3.tuple[20].rf_stage);
    assert(v3.tuple[21].bb_code != v3.tuple[20].bb_code);
    v3.relative_power_q10[20] = 700u;
    v3.confidence[20] = 3u;
    high.observed_us += 100000u;
    assert(direct_gain_v3_tick(&v3, &high) == 20u);          /* direct */
    assert(v3.transition == DG3_BB);

    direct_gain_v3_reset(&v3, &table, 35u, 62u);
    v3.bad_state[34] = 3u;
    high.observed_us += 100000u;
    assert(direct_gain_v3_tick(&v3, &high) == 34u);          /* old fade ban ignored */

    /* A rapidly changing input around the write must not teach a false
     * receiver gain ratio, even if the post-write envelope stabilizes. */
    direct_gain_v3_reset(&v3, &table, 35u, 62u);
    v3.last_tracking = obs(10, 17, 200, 0, 90, 802000u);
    weak = obs(7, 13, 200, 0, 90, 803000u);                  /* input moved */
    next = direct_gain_v3_tick(&v3, &weak);
    assert(next != 35u);
    direct_gain_v3_sync_applied(&v3, next, weak.observed_us);
    settled = obs(17, 28, 30, 0, 91, 804000u);
    assert(direct_gain_v3_tick(&v3, &settled) == next);
    assert(v3.verified == 1u && v3.learned == 0u);

    /* V5 anti-hunt: a level dithering across both band edges makes the
     * writes reverse direction; after two quick reversals every out-of-band
     * decision needs two windows for 200 ms, then direct mode returns. */
    direct_gain_v3_reset(&v3, &table, 40u, 62u);
    uint64_t t = 2000000u;
    uint32_t w0 = v3.writes;
    for (unsigned k = 0; k < 40u; ++k) {
        dg3_observation_t o = (k & 1u) ? obs(8, 14, 100, 0, 90, t)
                                       : obs(41, 60, 0, 0, 95, t);
        uint8_t g = direct_gain_v3_tick(&v3, &o);
        direct_gain_v3_sync_applied(&v3, g, t);
        t += 1000u;
    }
    assert(v3.damp_events >= 1u);
    /* Undamped this would have written on (nearly) every window. */
    assert(v3.writes - w0 < 30u);
    /* After the damp period a single out-of-band window acts again. */
    t += 300000u;
    dg3_observation_t late = obs(8, 14, 100, 0, 90, t);
    uint32_t writes_before_late = v3.writes;
    (void)direct_gain_v3_tick(&v3, &late);   /* first window: direct again */
    assert(v3.writes == writes_before_late + 1u);
    /* Saturation is never damped. */
    v3.damp_until_us = late.observed_us + 1000000u;
    dg3_observation_t sat = obs(60, 110, 0, 300, 90, late.observed_us + 5000u);
    uint8_t pre_sat = v3.current_gain;
    assert(direct_gain_v3_tick(&v3, &sat) < pre_sat);

    /* Range lanes: continuous total gain beyond the table maximum. */
    {
        uint8_t max = table.max_index;
        uint64_t t = 20000000u;
        direct_gain_v3_reset(&v3, &table, max, 62u);
        /* Disabled by default: a starved envelope changes nothing. */
        dg3_observation_t starved = obs(2, 2, 400, 0, 30, t);
        assert(direct_gain_v3_tick(&v3, &starved) == max && v3.lane == 0u);

        direct_gain_v3_enable_lanes(&v3, 2u);
        starved.observed_us = t += 1000u;
        /* P50 2 -> x16 = 32: the finest lane lands in band in one step. */
        assert(direct_gain_v3_tick(&v3, &starved) == max && v3.lane == 2u);
        /* Windows that may hold pre-switch samples are ignored. */
        dg3_observation_t early = obs(60, 110, 0, 300, 90, t + 100u);
        assert(direct_gain_v3_tick(&v3, &early) == max && v3.lane == 2u);
        /* A single-window burst of rail codes is ignored ... */
        dg3_observation_t rail = obs(30, 70, 0, 40, 95, t += 1000u);
        uint32_t drops = v3.fold_drops;
        assert(direct_gain_v3_tick(&v3, &rail) == max && v3.lane == 2u);
        dg3_observation_t quiet = obs(8, 20, 300, 0, 30, t += 200u);
        (void)direct_gain_v3_tick(&v3, &quiet);
        assert(v3.lane == 2u && v3.junk_windows == 0u);
        /* ... persisting rail codes drop to coarse. */
        rail.observed_us = t += 200u;
        (void)direct_gain_v3_tick(&v3, &rail);
        rail.observed_us = t += 200u;
        assert(direct_gain_v3_tick(&v3, &rail) == max && v3.lane == 0u);
        assert(v3.fold_drops == drops + 1u);
        /* Re-entry is held off for 5 ms after a fold drop. */
        dg3_observation_t none0 = obs(1, 3, 980, 0, 5, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &none0);
        assert(v3.lane == 0u);
        none0.observed_us = t += 5000u;
        (void)direct_gain_v3_tick(&v3, &none0);
        assert(v3.lane == 2u);

        /* Folded junk (incoherent, wide, not quiet) also drops the lane. */
        direct_gain_v3_enable_lanes(&v3, 2u);
        v3.lane = 2u; v3.lane_us = 0u;
        dg3_observation_t junk = obs(20, 60, 100, 10, 12, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &junk);
        junk.observed_us = t += 200u;
        assert(direct_gain_v3_tick(&v3, &junk) == max && v3.lane == 0u);

        /* P50 12 cannot land in the <6 dB band: take one lane (48) and let
         * the analog gain trim the overshoot instead of dropping the lane. */
        direct_gain_v3_reset(&v3, &table, max, 62u);
        direct_gain_v3_enable_lanes(&v3, 2u);
        dg3_observation_t twelve = obs(10, 11, 0, 0, 95, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &twelve);
        assert(v3.lane == 1u);
        dg3_observation_t over = obs(48, 70, 0, 0, 95, t += 1000u);
        uint8_t g = direct_gain_v3_tick(&v3, &over);
        assert(v3.lane == 1u && g < max);

        /* No carrier at the maximum: listen on the finest lane. */
        direct_gain_v3_reset(&v3, &table, max, 62u);
        direct_gain_v3_enable_lanes(&v3, 2u);
        dg3_observation_t none = obs(1, 3, 980, 0, 5, t += 1000u);
        assert(direct_gain_v3_tick(&v3, &none) == max && v3.lane == 2u);
        /* A strong VTX switching on folds: junk -> coarse. */
        dg3_observation_t vtx = obs(40, 100, 50, 250, 20, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &vtx);
        assert(v3.lane == 0u);

        /* Lanes are never entered below the analog maximum. */
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        direct_gain_v3_enable_lanes(&v3, 2u);
        dg3_observation_t weak = obs(8, 14, 100, 0, 90, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &weak);
        assert(v3.lane == 0u);
    }

    /* Noise cap: hardware VTX-off on ultrafine read P50 7 (sigma ~0.56
     * coarse step). Fine already puts noise at ~1 step, so a carrier is
     * held at fine; listening stays on ultrafine. */
    {
        uint8_t max = table.max_index;
        uint64_t t = 40000000u;
        direct_gain_v3_reset(&v3, &table, max, 62u);
        direct_gain_v3_enable_lanes(&v3, 2u);
        dg3_observation_t none = obs(1, 3, 980, 0, 5, t);
        (void)direct_gain_v3_tick(&v3, &none);
        assert(v3.lane == 2u);
        for (unsigned k = 0; k < 40u; ++k) {
            dg3_observation_t noise = obs(7, 25, 340, 0, 31, t += 1000u);
            (void)direct_gain_v3_tick(&v3, &noise);
        }
        assert(v3.lane == 2u && v3.lane_cap == 1u);
        /* A weak carrier appears (coherent): come down to the cap. */
        dg3_observation_t carrier_on_ultra = obs(9, 20, 100, 0, 80, t += 1000u);
        (void)direct_gain_v3_tick(&v3, &carrier_on_ultra);
        assert(v3.lane == 1u);
    }

    /* Overload removes BB gain before the RF stage (noise figure). */
    {
        uint8_t max = table.max_index;
        direct_gain_v3_reset(&v3, &table, max, 62u);
        const arc_gain_tuple_t top = v3.tuple[max];
        dg3_observation_t sat = obs(70, 110, 0, 300, 90, 50000000u);
        uint8_t g = direct_gain_v3_tick(&v3, &sat);
        assert(g < max);
        if (top.bb_code > 1u) assert(v3.tuple[g].rf_stage == top.rf_stage);
    }
    /* Saturation during the settle of an upward write acts at once. */
    {
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        dg3_observation_t weak = obs(8, 14, 100, 0, 90, 60000000u);
        uint8_t up = direct_gain_v3_tick(&v3, &weak);
        assert(up > 40u);
        direct_gain_v3_sync_applied(&v3, up, weak.observed_us);
        assert(v3.state == DG3_SETTLE);
        dg3_observation_t sat = obs(70, 110, 0, 300, 90, weak.observed_us + 50u);
        assert(direct_gain_v3_tick(&v3, &sat) < up);
        /* After a downward write a stale saturated window inside the 300 us
         * floor is ignored (no double drop). */
        uint8_t down = v3.current_gain;
        direct_gain_v3_sync_applied(&v3, down, sat.observed_us);
        dg3_observation_t stale = obs(70, 110, 0, 300, 90, sat.observed_us + 100u);
        assert(direct_gain_v3_tick(&v3, &stale) == down);
    }

    /* Hardware regression: a weak carrier whose noise tails touch the rail
     * on a finer lane every ~10th window must not hunt (was ~150 lane
     * changes/s). One second of 200 us windows. */
    {
        uint8_t max = table.max_index;
        uint64_t t = 70000000u;
        direct_gain_v3_reset(&v3, &table, max, 62u);
        direct_gain_v3_enable_lanes(&v3, 2u);
        uint32_t changes0 = v3.lane_changes;
        for (unsigned w = 0; w < 5000u; ++w) {
            unsigned scale = 1u << (2u * v3.lane);
            unsigned p50 = 1u * scale, p95 = 4u * scale;
            unsigned clip = (v3.lane && w % 10u < 2u) ? 25u : 0u;
            dg3_observation_t o = obs((int)(p50 > 113u ? 113u : p50),
                                      (int)(p95 > 113u ? 113u : p95), 300,
                                      (int)clip, 60, t += 200u);
            (void)direct_gain_v3_tick(&v3, &o);
        }
        printf("hunting regression: %u lane changes in 1 s, fold_streak %u\n",
               (unsigned)(v3.lane_changes - changes0), v3.fold_streak);
        assert(v3.lane_changes - changes0 < 60u);
    }

    /* Strong-signal radius boost. */
    {
        uint64_t t = 90000000u;
        /* Disabled: a strong tight ring at P50 ~22 is never boosted. */
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        boost_plant(&v3);
        assert(boost_run(&v3, 22.0, 4, 0, 90, 2000u, &t) == 0u && !v3.boost);

        /* Enabled: 20 ms of strong, tight HOLD, then one move into 30..46. */
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        boost_plant(&v3);
        direct_gain_v3_enable_boost(&v3, true);
        assert(boost_run(&v3, 22.0, 4, 0, 90, 99u, &t) == 0u && !v3.boost);
        unsigned moves = boost_run(&v3, 22.0, 4, 0, 90, 400u, &t);
        int p50 = boost_p50(&v3, 22.0);
        printf("radius boost: entries=%u moves=%u gain=%u p50=%d\n",
               (unsigned)v3.boost_entries, moves, v3.current_gain, p50);
        assert(v3.boost && v3.boost_entries == 1u && moves >= 1u && moves <= 2u);
        assert(p50 >= 30 && p50 <= 46);
        /* Steady boosted carrier: zero writes for 1 s. */
        assert(boost_run(&v3, 22.0, 4, 0, 90, 5000u, &t) == 0u && v3.boost);

        /* First rail codes: boost off at once, gain back into 13..32. */
        uint8_t boosted_gain = v3.current_gain;
        assert(boost_run(&v3, 22.0, 4, 25, 90, 1u, &t) == 1u);
        assert(!v3.boost && v3.boost_exits == 1u && v3.current_gain < boosted_gain);
        boost_run(&v3, 22.0, 4, 0, 90, 50u, &t);
        p50 = boost_p50(&v3, 22.0);
        assert(p50 >= 13 && p50 <= 32);
        /* Hold-off: no re-entry within 200 ms even on a clean ring. */
        assert(boost_run(&v3, 22.0, 4, 0, 90, 900u, &t) == 0u && !v3.boost);
        boost_run(&v3, 22.0, 4, 0, 90, 600u, &t);
        assert(v3.boost && v3.boost_entries == 2u);

        /* A sudden level jump (fade recovery, +3 dB) exits through P50/P95. */
        assert(boost_run(&v3, 44.0, 4, 0, 90, 1u, &t) == 1u && !v3.boost);
        /* Second exit within 10 s: the hold-off doubles to 400 ms. */
        assert(v3.boost_streak == 2u &&
               v3.boost_hold_until_us - v3.boost_exit_us == 400000u);

        /* Saturation inside the boost takes the emergency path. */
        t += 20000000u;
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        boost_plant(&v3);
        direct_gain_v3_enable_boost(&v3, true);
        boost_run(&v3, 22.0, 4, 0, 90, 500u, &t);
        assert(v3.boost);
        dg3_observation_t sat = obs(80, 110, 0, 300, 90, t += 200u);
        assert(direct_gain_v3_tick(&v3, &sat) < 40u && !v3.boost && v3.overloads == 1u);

        /* Never entered: a wide ring (noisy), low coherence, rail codes or a
         * P95 that would not fit the boost band. */
        static const int spread[] = {12, 4, 4, 9};
        static const int coh[] = {90, 70, 90, 90};
        static const int clip[] = {0, 0, 5, 0};
        for (unsigned k = 0; k < 4u; ++k) {
            direct_gain_v3_reset(&v3, &table, 40u, 62u);
            boost_plant(&v3);
            direct_gain_v3_enable_boost(&v3, true);
            double base = k == 3u ? 14.0 : 22.0; /* P95/P50 = 23/14 > 1.35 */
            boost_run(&v3, base, spread[k], clip[k], coh[k], 3000u, &t);
            assert(!v3.boost && v3.boost_entries == 0u);
        }
        /* Disabling drops an active boost. */
        direct_gain_v3_reset(&v3, &table, 40u, 62u);
        boost_plant(&v3);
        direct_gain_v3_enable_boost(&v3, true);
        boost_run(&v3, 22.0, 4, 0, 90, 500u, &t);
        assert(v3.boost);
        direct_gain_v3_enable_boost(&v3, false);
        assert(!v3.boost);
    }

    /* Gray zone (neither carrier() nor no_carrier): a weak, not yet
     * recognised carrier must climb, not hold (it used to stay at G52). */
    {
        uint64_t t = 30000000u;
        direct_gain_v3_reset(&v3, &table, 52u, 62u);
        uint8_t start = v3.current_gain;
        for (unsigned k = 0; k < 2000u; ++k) {
            dg3_observation_t gray = obs(7, 13, 300, 0, 30, t += 5000u);
            uint8_t g = direct_gain_v3_tick(&v3, &gray);
            if (v3.state == DG3_SETTLE && v3.write_us == t) direct_gain_v3_sync_applied(&v3, g, t);
        }
        assert(v3.current_gain > start && v3.writes > 0u);
        direct_gain_v3_reset(&v3, &table, 52u, 62u);
        for (unsigned k = 0; k < 2000u; ++k) {
            dg3_observation_t edge = obs(3, 6, 400, 0, 20, t += 5000u);
            uint8_t g = direct_gain_v3_tick(&v3, &edge);
            if (v3.state == DG3_SETTLE && v3.write_us == t) direct_gain_v3_sync_applied(&v3, g, t);
        }
        assert(v3.current_gain > 52u);
    }

    /* The measured map survives a tracking reset on the same table and
     * round-trips through the NVS blob; an unknown anchor drops it. */
    {
        memset(&v3, 0, sizeof(v3)); /* earlier scenarios left a learned map */
        direct_gain_v3_reset(&v3, &table, 60u, 62u);
        v3.relative_power_q10[61] = 1300u;
        v3.confidence[61] = 3u;
        v3.uncertainty_pm[61] = 100u;
        v3.learned = 5u;
        v3.bad_state[61] = 3u; /* old indoor fade history */
        dg3_map_blob_t blob;
        assert(direct_gain_v3_export_map(&v3, &blob) == 2u);
        assert(blob.bad_state[61] == 0u);
        blob.bad_state[61] = 3u; /* a legacy on-device v1 map */
        direct_gain_v3_reset(&v3, &table, 60u, 62u);
        assert(v3.confidence[61] == 3u && v3.relative_power_q10[61] == 1300u);
        assert(v3.bad_state[61] == 0u);
        direct_gain_v3_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        direct_gain_v3_reset(&fresh, &table, 60u, 62u);
        assert(!fresh.confidence[61]);
        assert(direct_gain_v3_import_map(&fresh, &blob) && fresh.relative_power_q10[61] == 1300u);
        assert(fresh.bad_state[61] == 0u);
        direct_gain_v3_reset(&fresh, &table, 70u, 62u);
        assert(!fresh.confidence[61]);
        direct_gain_v3_reset(&fresh, &table, 70u, 62u);
        assert(!direct_gain_v3_import_map(&fresh, &blob)); /* anchor 70 unknown */
    }

    /* A severe excursion still acts on one window: P50 6 (< band - 4). */
    {
        memset(&v3, 0, sizeof(v3));
        direct_gain_v3_reset(&v3, &table, 35u, 62u);
        dg3_observation_t starved = obs(6, 12, 300, 0, 90, 900000u);
        assert(direct_gain_v3_tick(&v3, &starved) != 35u && v3.writes == 1u);
    }

    puts("direct gain v3 core: OK");
    return 0;
}
