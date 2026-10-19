#include "afc_state.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint32_t auto_ctx = afc_context(0, 0, 1, true, 0, 1);
    afc_state_t s;
    afc_reset(&s, auto_ctx);

    /* Needs fresh trusted samples before persistence even starts. */
    for (unsigned i = 0; i < AFC_MIN_FRESH_SAMPLES - 1u; ++i) afc_observe(&s, 120);
    for (unsigned i = 0; i < 40u; ++i) assert(!afc_tick(&s, true));
    afc_observe(&s, 120);
    for (unsigned i = 0; i + 1u < AFC_PERSIST_TICKS; ++i) assert(!afc_tick(&s, true));
    assert(afc_tick(&s, true));
    assert(s.filtered_khz == 120);

    /* Issue #115 item 5: 19/20 persistence must not survive AUTO->HOLD->AUTO. */
    afc_reset(&s, auto_ctx);
    for (unsigned i = 0; i < AFC_MIN_FRESH_SAMPLES; ++i) afc_observe(&s, 200);
    for (unsigned i = 0; i + 1u < AFC_PERSIST_TICKS; ++i) assert(!afc_tick(&s, true));
    const uint32_t hold_ctx = afc_context(1, 0, 1, true, 0, 1);
    assert(afc_sync_context(&s, hold_ctx));
    assert(afc_sync_context(&s, auto_ctx));
    afc_observe(&s, 200);
    assert(!afc_tick(&s, true));
    assert(s.persistence == 0 && s.samples == 1);

    /* Every context component invalidates: channel, profile, BW, offset, RF. */
    const uint32_t variants[5] = {
        afc_context(0, 1, 1, true, 0, 1), afc_context(0, 0, 2, true, 0, 1),
        afc_context(0, 0, 1, false, 0, 1), afc_context(0, 0, 1, true, 50, 1),
        afc_context(0, 0, 1, true, 0, 2),
    };
    for (unsigned v = 0; v < 5u; ++v) {
        assert(variants[v] != auto_ctx);
        afc_reset(&s, auto_ctx);
        afc_observe(&s, 90);
        assert(afc_sync_context(&s, variants[v]));
        assert(s.samples == 0 && s.filtered_khz == 0);
        assert(!afc_sync_context(&s, variants[v]));
    }

    /* Issue #115 item 4: a transition wipes remembered transients. */
    afc_reset(&s, auto_ctx);
    for (unsigned i = 0; i < 20u; ++i) afc_observe(&s, 900);
    afc_invalidate(&s);
    assert(s.samples == 0 && s.filtered_khz == 0 && s.persistence == 0);

    /* Deadband and ineligible ticks never write and clear persistence. */
    afc_reset(&s, auto_ctx);
    for (unsigned i = 0; i < 20u; ++i) afc_observe(&s, 30);
    for (unsigned i = 0; i < 50u; ++i) assert(!afc_tick(&s, true));
    for (unsigned i = 0; i < 20u; ++i) afc_observe(&s, 300);
    for (unsigned i = 0; i < 10u; ++i) assert(!afc_tick(&s, true));
    assert(!afc_tick(&s, false));
    assert(s.persistence == 0);
    afc_observe(&s, 5000);
    assert(s.filtered_khz <= AFC_CFO_LIMIT_KHZ);

    puts("AFC state hygiene (#115 items 4/5): passed");
    return 0;
}
