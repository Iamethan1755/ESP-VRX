#include <assert.h>
#include <stdio.h>

#include "rx_auto_lab.h"

static rx_auto_observation_t obs(int p, int q, int clip, int origin,
                                 int skew, int cross, int wind, int sync)
{
    return (rx_auto_observation_t) {
        .p_median = p,
        .q_phase = q,
        .clip_permille = clip,
        .origin_permille = origin,
        .iq_skew_permille = skew,
        .iq_cross_permille = cross,
        .winding_permille = wind,
        .sync_quality = sync,
        .transport_faults = 0,
    };
}

int main(void)
{
    rx_auto_observation_t g62 = obs(17, 100, 0, 0, 22, 11, 0, 24);
    rx_auto_observation_t g65 = obs(32, 100, 0, 0, 58, 103, 0, 0);
    rx_auto_observation_t g66 = obs(53, 99, 257, 0, 49, 14, 0, 12);
    rx_auto_observation_t far = obs(1, 0, 0, 1000, 91, 306, 0, 0);

    assert(rx_auto_classify(&g62) == RX_AUTO_SWEET);
    assert(rx_auto_classify(&g65) == RX_AUTO_SWEET);
    assert(rx_auto_classify(&g66) == RX_AUTO_REJECT);
    assert(rx_auto_classify(&far) == RX_AUTO_POOR);

    /* P=53 must never beat a clean P=17/32 state merely because it is larger. */
    assert(rx_auto_better(&g62, &g66));
    assert(rx_auto_better(&g65, &g66));

    /* Same class: preserve clipping headroom, coherence and target placement. */
    rx_auto_observation_t centered = obs(24, 95, 0, 5, 20, 20, 0, 20);
    rx_auto_observation_t too_big = obs(34, 95, 0, 5, 20, 20, 0, 20);
    assert(rx_auto_better(&centered, &too_big));

    assert(rx_auto_is_rf_limit(&far));
    assert(!rx_auto_is_rf_limit(&g62));
    assert(rx_auto_is_overload(&g66));
    assert(!rx_auto_is_overload(&g62));

    rx_auto_observation_t ref2 = g62;
    ref2.q_phase = 92;
    ref2.p_median = 22;
    ref2.origin_permille = 80;
    assert(rx_auto_reference_stable(&g62, &ref2));
    ref2.origin_permille = 400;
    assert(!rx_auto_reference_stable(&g62, &ref2));

    rx_auto_observation_t fault = g62;
    fault.transport_faults = 1;
    assert(rx_auto_classify(&fault) == RX_AUTO_REJECT);

    puts("rx_auto_lab tests passed");
    return 0;
}
