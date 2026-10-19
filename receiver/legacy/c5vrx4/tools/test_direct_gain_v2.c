#include <assert.h>
#include <stdio.h>
#include "direct_gain_v2.h"

unsigned char phy_param[0x800];

static direct_gain_v2_observation_t obs(int p, int q, int origin, int clip,
                                         uint64_t us)
{
    return (direct_gain_v2_observation_t){
        .p = p, .q = q, .origin_pm = origin, .clip_pm = clip,
        .observed_us = us,
    };
}

int main(void)
{
    arc_gain_table_t table;
    arc_gain_table_from_bytes(&table, NULL, 81u);
    direct_gain_v2_t v2;
    direct_gain_v2_reset(&v2, &table, 40u, 62u);
    assert(v2.tuple[40].rf_stage != v2.tuple[42].rf_stage);

    /* Useful IQ stays locked without a single physical write. */
    for (uint64_t t = 6000; t < 600000; t += 6000) {
        direct_gain_v2_observation_t o = obs(22, 99, 0, 0, t);
        assert(direct_gain_v2_tick(&v2, &o) == 40u);
    }
    assert(v2.writes == 0 && v2.state == DIRECT_GAIN_V2_LOCK);

    /* A small deficit can select an individual Fine index, not a G+2 grid. */
    direct_gain_v2_reset(&v2, &table, 35u, 62u);
    direct_gain_v2_observation_t weak = obs(11, 80, 0, 0, 610000);
    uint8_t fine_target = direct_gain_v2_tick(&v2, &weak);
    assert(fine_target == 38u);
    assert(v2.tuple[fine_target].rf_stage == v2.tuple[35].rf_stage);
    assert(v2.writes == 1u);
    direct_gain_v2_sync_applied(&v2, fine_target, weak.observed_us);
    weak.observed_us += 6000;
    assert(direct_gain_v2_tick(&v2, &weak) == fine_target);
    assert(v2.stale_rejects == 1u);
    weak = obs(22, 99, 0, 0, 624000);
    assert(direct_gain_v2_tick(&v2, &weak) == fine_target);
    assert(v2.state == DIRECT_GAIN_V2_LOCK);
    direct_gain_v2_observation_t settled = obs(22, 99, 0, 0, 650000);
    direct_gain_v2_learn_settled(&v2, &settled, fine_target);
    assert(v2.node[fine_target].count == 1u && v2.slow_samples == 1u);
    direct_gain_v2_learn_settled(&v2, &settled, 35u);
    assert(v2.slow_samples == 1u);

    /* A learned edge is used only after three valid observations and only
     * for its exact from/to pair. Before then the factory prior applies. */
    direct_gain_v2_reset(&v2, &table, 35u, 62u);
    v2.edge[0] = (direct_gain_v2_edge_t){
        .from = 35u, .to = 38u, .count = 2u, .ratio_q10 = 1024u,
    };
    weak = obs(11, 80, 0, 0, 700000);
    assert(direct_gain_v2_tick(&v2, &weak) == 38u);
    direct_gain_v2_reset(&v2, &table, 35u, 62u);
    v2.edge[0] = (direct_gain_v2_edge_t){
        .from = 35u, .to = 38u, .count = 3u, .ratio_q10 = 1024u,
    };
    assert(direct_gain_v2_tick(&v2, &weak) == 39u);

    /* Hard overload is a direct target hop and respects the G20 floor. */
    direct_gain_v2_reset(&v2, &table, 48u, 62u);
    direct_gain_v2_observation_t hot = obs(98, 90, 0, 900, 1000000);
    uint8_t safe = direct_gain_v2_tick(&v2, &hot);
    assert(safe < 48u && safe >= DIRECT_GAIN_V2_FLOOR);
    assert(v2.writes == 1u);

    /* Loss of carrier performs one direct survival hop, then no hunt. */
    direct_gain_v2_reset(&v2, &table, 30u, 62u);
    direct_gain_v2_observation_t lost = obs(1, 0, 980, 0, 2000000);
    assert(direct_gain_v2_tick(&v2, &lost) == 62u);
    direct_gain_v2_sync_applied(&v2, 62u, lost.observed_us);
    lost.observed_us += 20000;
    assert(direct_gain_v2_tick(&v2, &lost) == 62u);
    assert(v2.writes == 1u);

    puts("direct gain v2: OK");
    return 0;
}
