/* C5VRX by Twotoz and contributors: integrated range policy regressions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rf.h"
#include "direct_gain_v3.h"
#include "c5vrx4.h"
unsigned char phy_param[0x800];
static uint8_t fixed = C5VRX4_LANE_ADAPTIVE;
static rf_iq_lane_stats_t lane_stats;
uint8_t c5vrx4_fixed_lane(void) { return fixed; }
bool c5vrx4_history_enabled(void) { return false; }
uint8_t rf_get_iq_lanes(void) { return lane_stats.last_to; }
void rf_get_iq_lane_stats(rf_iq_lane_stats_t *s) { *s = lane_stats; }
#include "../lanes.c"

int main(void)
{
    uint8_t fit[256], outside[256];
    memset(fit, 0x11, sizeof(fit)); memset(outside, 0x33, sizeof(outside));
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1000) == 0);
    assert(c5vrx4_lane_target(0, 2, outside, sizeof(outside), 1200) == 0);
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1400) == 0);
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1600) == 1);
    assert(c5vrx4_lane_target(2, 0, NULL, 0, 1800) == 0);
    lane_stats.switches = 1; lane_stats.last_switch_us = 2000;
    assert(!c5vrx4_lane_window_ready(1999));
    assert(!c5vrx4_lane_window_ready(2209));
    assert(c5vrx4_lane_window_ready(2210));

    arc_gain_table_t table; arc_gain_table_from_bytes(&table, NULL, 81);
    direct_gain_v3_t v;
    dg3_observation_t severe = {.p50=65, .p95=113, .clip_pm=738,
                               .coherence=0, .observed_us=10000};
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.writes == 1);
    severe.observed_us += 100; /* Old saturated IQ after the drop: no hunt. */
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.writes == 1);
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2); v.lane = 2;
    severe.observed_us = 20000;
    assert(direct_gain_v3_tick(&v, &severe) == 81 && v.lane == 0);
    severe.observed_us += 1000;
    assert(direct_gain_v3_tick(&v, &severe) == 20);
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    severe.clip_pm = 200; severe.observed_us = 30000;
    assert(direct_gain_v3_tick(&v, &severe) > 20); /* Moderate: physical stage cut. */
    direct_gain_v3_reset(&v, &table, 35, 62);
    dg3_observation_t good = {.p50=22, .p95=40, .coherence=99};
    for (unsigned n=0;n<100;n++) {
        good.observed_us = 40000 + n*1000;
        assert(direct_gain_v3_tick(&v, &good) == 35);
    }
    assert(v.writes == 0);

    /* Default fixed fine {9,7,6,5}: the lane never changes, not to listen on
     * ultrafine without a carrier, not to escape rail codes, not when the
     * envelope is starved or high; analog gain does all the tracking. */
    fixed = 1u;
    /* A moving indoor channel must not permanently remove a whole gain bank.
     * Reproduce three settled low-coherence visits, then the old saved bank
     * blacklist: a weak carrier must still reach the remaining gain range. */
    memset(&v, 0, sizeof(v));
    direct_gain_v3_reset(&v, &table, 63, 62);
    dg3_observation_t fade = {.p50=9, .p90=13, .p95=17, .origin_pm=400,
                              .coherence=20, .observed_us=90000};
    for (unsigned n = 0; n < 3; ++n) {
        direct_gain_v3_sync_applied(&v, 63, fade.observed_us);
        fade.observed_us += 1000;
        (void)direct_gain_v3_tick(&v, &fade);
    }
    assert(v.bad_state[63] == 0u && v.learned == 0u);
    memset(&v, 0, sizeof(v));
    direct_gain_v3_reset(&v, &table, 62, 62);
    for (unsigned g = 63; g <= 73; ++g) v.bad_state[g] = 3u;
    fade.origin_pm = 100; fade.coherence = 90;
    for (unsigned n = 0; n < 2000; ++n) {
        fade.observed_us += 200;
        uint8_t gain = direct_gain_v3_tick(&v, &fade);
        if (v.state == DG3_SETTLE && v.write_us == fade.observed_us)
            direct_gain_v3_sync_applied(&v, gain, fade.observed_us);
    }
    assert(v.current_gain == table.max_index && v.writes > 0u);
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    assert(v.lane == 1u && v.lane_cap == 1u);
    dg3_observation_t none = {.p50=1, .p95=4, .origin_pm=900, .coherence=5,
                              .observed_us=100000};
    assert(direct_gain_v3_tick(&v, &none) == 81 && v.lane == 1u);
    dg3_observation_t starved = {.p50=3, .p95=12, .origin_pm=200, .coherence=80,
                                 .observed_us=101000};
    for (unsigned n = 0; n < 20; ++n, starved.observed_us += 1000)
        assert(direct_gain_v3_tick(&v, &starved) == 81 && v.lane == 1u);
    dg3_observation_t rail = {.p50=30, .p95=80, .clip_pm=40, .coherence=90,
                              .observed_us=130000};
    for (unsigned n = 0; n < 20; ++n, rail.observed_us += 1000) {
        (void)direct_gain_v3_tick(&v, &rail);
        assert(v.lane == 1u);
    }
    assert(v.lane_changes == 0u && v.fold_drops == 0u);
    /* Severe overdrive or fold on the fixed lane takes the coarse-lane G20
     * floor instead of a lane escape, and refuses stale saturated IQ. */
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    severe.observed_us = 200000; severe.clip_pm = 738;
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.lane == 1u);
    severe.observed_us += 100;
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.writes == 1);
    /* Fixed ultrafine keeps the earlier Z comparison behaviour. */
    fixed = 2u;
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    assert(v.lane == 2u && v.lane_cap == 2u);
    puts("PASS: protected lanes, freshness, severe coarse overload floor, stale drop refusal, zero-write clean tracking and fixed fine/ultrafine lanes");
}
