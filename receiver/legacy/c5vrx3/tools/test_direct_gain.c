#include <assert.h>
#include <stdio.h>
#include "direct_gain.h"

int main(void)
{
    arc_gain_table_t table = {.max_index = 81};
    direct_gain_controller_t dg;
    direct_gain_observation_t o = {
        .p_median = 11, .q_phase = 75, .origin_permille = 25,
        .survival_gain = 62,
    };
    direct_gain_reset(&dg, &table, 40);

    /* P is power: halving 22 to 11 requests ~3.7 indices. One isolated
     * window cannot move gain; a second independent weak window can. */
    assert(direct_gain_tick(&dg, &o) == 40);
    assert(dg.target_gain == 44);
    assert(direct_gain_tick(&dg, &o) == 44);
    assert(dg.last_delta_gain == 4);
    direct_gain_sync_applied(&dg, 44);
    assert(dg.current_gain == 44);

    /* A forced physical clamp or external write must replace predicted state. */
    direct_gain_sync_applied(&dg, 43);
    assert(dg.current_gain == 43 && dg.target_gain == 43);
    assert(dg.state == DIRECT_GAIN_SETTLE);

    /* A single outlying weak window followed by a healthy one cannot
     * create a PHY write or a learned calibration offset. */
    direct_gain_reset(&dg, &table, 40);
    assert(direct_gain_tick(&dg, &o) == 40);
    o.p_median = 22;
    assert(direct_gain_tick(&dg, &o) == 40);
    assert(dg.total_writes == 0);

    /* Noise with no carrier is not an overload or a P=0 gain-up oracle. */
    o.p_median = 0;
    o.q_phase = 0;
    o.origin_permille = 950;
    o.clip_permille = 900;
    assert(direct_gain_tick(&dg, &o) == 40);
    assert(direct_gain_tick(&dg, &o) == 40);
    assert(direct_gain_tick(&dg, &o) == 62);
    assert(dg.pending_votes == 0);

    /* A fading weak carrier already above survival keeps its sensitivity
     * briefly rather than immediately jumping down on three noisy windows. */
    direct_gain_reset(&dg, &table, 70);
    for (unsigned i = 0; i < 19; ++i)
        assert(direct_gain_tick(&dg, &o) == 70);
    assert(direct_gain_tick(&dg, &o) == 62);

    /* Outer-bin occupancy alone is not proof of analog saturation. */
    direct_gain_reset(&dg, &table, 40);
    o.p_median = 22;
    o.q_phase = 75;
    o.origin_permille = 0;
    assert(direct_gain_tick(&dg, &o) == 40);
    /* Genuine high-power overload bypasses ordinary direction votes. */
    o.p_median = 50;
    assert(direct_gain_tick(&dg, &o) == 26);
    puts("direct gain: OK");
    return 0;
}
