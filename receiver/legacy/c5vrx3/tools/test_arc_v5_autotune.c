#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arc_v5_autotune.h"

static arc_gain_table_t table(void)
{
    arc_gain_table_t t = {0};
    t.max_index = 81u;
    return t;
}

static arc_v5_observation_t obs(int p, int q, int clip, int origin,
                                arc_v5_context_t context)
{
    return (arc_v5_observation_t){
        .p_median = p,
        .q_phase = q,
        .clip_permille = clip,
        .origin_permille = origin,
        .winding_permille = 80,
        .context = context,
    };
}

static void teach_local_model(arc_v5_autotune_t *a, unsigned samples)
{
    for (unsigned g = 50; g <= 70; ++g) {
        a->model[g].dp_q8 = 2 * 256;
        a->model[g].dq_q8 = 5 * 256;
        a->model[g].dorigin_q8 = -30 * 256;
        a->model[g].dclip_q8 = 8 * 256;
        a->model[g].samples = (uint16_t)samples;
        a->model[g].settle_ms = 500;
    }
}

int main(void)
{
    arc_gain_table_t t = table();
    arc_v5_autotune_t a;

    arc_v5_autotune_reset(&a, &t, 54u, 62u);
    assert(a.gain == 54u);
    assert(a.state == ARC_V5_HOLD);

    /* Pure noise is not treated as recoverable weak RF. */
    arc_v5_observation_t nc = obs(2, 5, 0, 900, ARC_V5_CONTEXT_NO_CARRIER);
    for (unsigned i = 0; i < 10; ++i)
        (void)arc_v5_autotune_tick(&a, &nc);
    assert(a.gain == 62u);
    assert(a.learned_updates == 0u);

    /* A stable clean observation does not create predictive writes. */
    arc_v5_autotune_rearm(&a, &t, 54u, 62u);
    arc_v5_observation_t clean = obs(40, 98, 0, 40, ARC_V5_CONTEXT_CLEAN);
    for (unsigned i = 0; i < 20; ++i)
        (void)arc_v5_autotune_tick(&a, &clean);
    assert(a.gain == 54u);
    assert(a.state == ARC_V5_LOCK);

    /* Neighbor confidence must not authorize an unseen edge. */
    arc_v5_autotune_rearm(&a, &t, 54u, 62u);
    a.model[53].samples = 64u;
    a.model[54].samples = 0u;
    arc_v5_observation_t weak = obs(12, 70, 0, 280, ARC_V5_CONTEXT_WEAK);
    (void)arc_v5_autotune_tick(&a, &weak);
    (void)arc_v5_autotune_tick(&a, &weak);
    assert(a.gain == 54u);

    /* Once local response confidence exists, V5 reacts in two control windows
     * and skips multiple +1 V3 discovery steps. */
    arc_v5_autotune_rearm(&a, &t, 54u, 62u);
    teach_local_model(&a, 32u);
    (void)arc_v5_autotune_tick(&a, &weak);
    assert(a.gain == 54u);
    (void)arc_v5_autotune_tick(&a, &weak);
    assert(a.gain == 58u);
    assert(a.state == ARC_V5_VERIFY);

    /* Verification learns only the short local actuator response. */
    uint8_t predicted = a.gain;
    arc_v5_observation_t after = obs(20, 90, 0, 150, ARC_V5_CONTEXT_CLEAN);
    for (unsigned i = 0; i < 9; ++i)
        (void)arc_v5_autotune_tick(&a, &after);
    assert(a.gain == predicted);
    assert(a.learned_updates == 0u);
    (void)arc_v5_autotune_tick(&a, &after);
    assert(a.gain == predicted);
    assert(a.learned_updates >= 1u);
    assert(a.dirty_updates >= 1u);

    /* A wrong-direction jump is reversible during VERIFY and is not learned. */
    arc_v5_autotune_rearm(&a, &t, 54u, 62u);
    teach_local_model(&a, 32u);
    (void)arc_v5_autotune_tick(&a, &weak);
    (void)arc_v5_autotune_tick(&a, &weak);
    uint8_t jumped = a.gain;
    assert(jumped > 54u);
    uint32_t learned_before = a.learned_updates;
    arc_v5_observation_t overload = obs(62, 99, 150, 0, ARC_V5_CONTEXT_OVERLOAD);
    (void)arc_v5_autotune_tick(&a, &overload);
    assert(a.gain == 54u);
    assert(a.learned_updates == learned_before);

    /* Persistent model round-trips with fingerprint + CRC protection. */
    arc_v5_persisted_model_t blob;
    assert(arc_v5_export_model(&a, &blob));
    arc_v5_autotune_t b;
    arc_v5_autotune_reset(&b, &t, 62u, 62u);
    assert(arc_v5_import_model(&b, &blob));
    assert(b.model_generation == a.model_generation);
    assert(b.model[54].samples == a.model[54].samples);
    blob.crc ^= 1u;
    assert(!arc_v5_import_model(&b, &blob));

    puts("ARC V5 predictive autotune tests passed");
    return 0;
}
