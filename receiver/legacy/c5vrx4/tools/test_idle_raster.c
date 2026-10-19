/* C5VRX by Twotoz and contributors: no-carrier idle raster decision. */
#include <assert.h>
#include <stdio.h>
#include "idle_raster.h"

static idle_raster_obs_t noise(void)
{
    return (idle_raster_obs_t){.enabled = true, .owner_free = true, .survival_gain = true,
                               .q_phase = 10, .sync_age_ticks = 100};
}

static unsigned ticks_to_enter(idle_raster_t *s, idle_raster_obs_t o, unsigned limit)
{
    for (unsigned t = 1; t <= limit; ++t)
        if (idle_raster_step(s, &o) == IDLE_RASTER_ENTER) return t;
    return 0;
}

int main(void)
{
    /* Receiver noise only: enter after exactly 2 s, once. */
    idle_raster_t s = {0};
    assert(ticks_to_enter(&s, noise(), 200) == IDLE_RASTER_ENTER_TICKS);
    assert(s.active && s.entries == 1);
    idle_raster_obs_t o = noise();
    for (unsigned t = 0; t < 1000; ++t) assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY);

    /* Hysteresis: coherence between the thresholds keeps the raster ... */
    o.q_phase = IDLE_RASTER_CARRIER_Q - 1;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY && s.active);
    /* ... a single carrier window is not enough (fringe flicker) ... */
    o.q_phase = IDLE_RASTER_CARRIER_Q;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY && s.active);
    o.q_phase = 10;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY && s.active);
    /* ... two consecutive carrier windows return to live ... */
    o.q_phase = IDLE_RASTER_CARRIER_Q;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY);
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT && !s.active && s.exits == 1);
    /* ... and the next entry is held off 5 s (+2 s quiet). */
    assert(ticks_to_enter(&s, noise(), 1000) == IDLE_RASTER_REENTRY_TICKS + IDLE_RASTER_ENTER_TICKS);
    o = noise(); o.fresh_sync = true;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT);
    /* Repeated exits double the hold-off (10 s, then 20 s). */
    assert(ticks_to_enter(&s, noise(), 2000) == 2u * IDLE_RASTER_REENTRY_TICKS + IDLE_RASTER_ENTER_TICKS);
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT);
    assert(ticks_to_enter(&s, noise(), 2000) == 4u * IDLE_RASTER_REENTRY_TICKS + IDLE_RASTER_ENTER_TICKS);
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT);
    assert(ticks_to_enter(&s, noise(), 2000) == 4u * IDLE_RASTER_REENTRY_TICKS + IDLE_RASTER_ENTER_TICKS);
    /* ... and the same coherence never enters from live. */
    s = (idle_raster_t){0};
    o = noise(); o.q_phase = IDLE_RASTER_QUIET_Q;
    assert(ticks_to_enter(&s, o, 1000) == 0);

    /* A sync alone (weak carrier, low coherence) returns to live at once. */
    s = (idle_raster_t){0};
    assert(ticks_to_enter(&s, noise(), 200));
    o = noise(); o.fresh_sync = true;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT);

    /* Disabling while idle returns to live. */
    s = (idle_raster_t){0};
    assert(ticks_to_enter(&s, noise(), 200));
    o = noise(); o.enabled = false;
    assert(idle_raster_step(&s, &o) == IDLE_RASTER_EXIT);

    /* Entry blockers: each one alone prevents the raster indefinitely. */
    for (unsigned b = 0; b < 6; ++b) {
        s = (idle_raster_t){0};
        o = noise();
        if (b == 0) o.enabled = false;
        if (b == 1) o.owner_free = false;
        if (b == 2) o.survival_gain = false;
        if (b == 3) o.settling = true;
        if (b == 4) o.sync_age_ticks = IDLE_RASTER_SYNC_TICKS - 1u;
        if (b == 5) o.fresh_sync = true;
        assert(ticks_to_enter(&s, o, 1000) == 0 && !s.active);
    }

    /* Sync fragments every 1.5 s (a weak transmitter at the range edge) keep
     * live video: every fragment restarts both the sync age and the count. */
    s = (idle_raster_t){0};
    unsigned age = 0;
    for (unsigned t = 0; t < 2000; ++t) {
        o = noise();
        o.fresh_sync = t % 30u == 0u;
        age = o.fresh_sync ? 0u : age + 1u;
        o.sync_age_ticks = age;
        assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY);
    }

    /* A carrier blip every 1.9 s also prevents entry (no 2-s quiet run). */
    s = (idle_raster_t){0};
    for (unsigned t = 0; t < 2000; ++t) {
        o = noise();
        if (t % 38u == 0u) o.q_phase = 60;
        assert(idle_raster_step(&s, &o) == IDLE_RASTER_STAY);
    }

    /* Abandon (TX not taken over, or the user opened the menu): no exit
     * counted, and a fresh 2-s quiet run is needed again. */
    s = (idle_raster_t){0};
    assert(ticks_to_enter(&s, noise(), 200));
    idle_raster_abandon(&s);
    assert(!s.active && s.exits == 0);
    assert(ticks_to_enter(&s, noise(), 200) == IDLE_RASTER_ENTER_TICKS && s.entries == 2);

    puts("PASS idle_raster: 2-s quiet entry, sync / 2-window carrier exit, doubling re-entry hold-off, hysteresis, blockers");
    return 0;
}
