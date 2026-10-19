#include <assert.h>
#include <stdio.h>

#include "arc_v3_controller.h"

static arc_gain_table_t table(void)
{
    arc_gain_table_t t = {0};
    t.max_index = 81;
    return t;
}

static arc_v3_observation_t obs(int p, int q, int clip, int origin)
{
    return (arc_v3_observation_t) {
        .p_median = p,
        .q_phase = q,
        .clip_permille = clip,
        .origin_permille = origin,
        .winding_permille = 0,
    };
}

static void feed(arc_v3_controller_t *a,
                 arc_v3_observation_t o,
                 unsigned ticks)
{
    for (unsigned i = 0; i < ticks; ++i)
        (void)arc_v3_controller_tick(a, &o);
}

static uint8_t tick_values(arc_v3_controller_t *a,
                           int p, int q, int clip, int origin)
{
    arc_v3_observation_t o = obs(p, q, clip, origin);
    return arc_v3_controller_tick(a, &o);
}

static void drive_until_gain(arc_v3_controller_t *a,
                             arc_v3_observation_t o,
                             uint8_t wanted,
                             unsigned max_ticks)
{
    for (unsigned i = 0; i < max_ticks && a->gain != wanted; ++i)
        (void)arc_v3_controller_tick(a, &o);
    assert(a->gain == wanted);
}

static void drive_until_state(arc_v3_controller_t *a,
                              arc_v3_observation_t o,
                              arc_v3_state_t wanted,
                              unsigned max_ticks)
{
    for (unsigned i = 0; i < max_ticks && a->state != wanted; ++i)
        (void)arc_v3_controller_tick(a, &o);
    assert(a->state == wanted);
}

int main(void)
{
    arc_gain_table_t t = table();
    arc_v3_controller_t a;

    /* FAR: the controller must escape the old G62 no-sync trap using raw Q4
     * starvation alone. Five-window filtering may delay the decision, but it
     * must still traverse the table toward the measured usable region. */
    arc_v3_controller_reset(&a, &t, 62);
    drive_until_gain(&a, obs(1, 0, 0, 1000), 66, 40);
    drive_until_gain(&a, obs(1, 0, 0, 1000), 70, 40);
    drive_until_gain(&a, obs(1, 0, 0, 980), 74, 40);
    drive_until_gain(&a, obs(5, 13, 0, 435), 75, 40);
    drive_until_gain(&a, obs(5, 41, 0, 213), 76, 40);
    drive_until_state(&a, obs(9, 74, 0, 62), ARC_V3_LOCK, 40);

    /* A rolling majority of good Q4 must ignore short deep fades. This models
     * the live G66/G69 traces where individual 50 ms windows varied strongly
     * while the underlying carrier remained useful. */
    uint8_t locked = a.gain;
    const arc_v3_observation_t jitter[] = {
        {10, 81, 0, 80, 0},
        { 2,  8, 0, 900, 0}, /* short fade */
        {13, 97, 0, 30, 0},
        { 3, 12, 0, 850, 0}, /* short fade */
        { 9, 68, 0, 100, 0},
    };
    for (unsigned round = 0; round < 30; ++round) {
        for (unsigned i = 0; i < sizeof(jitter) / sizeof(jitter[0]); ++i) {
            (void)arc_v3_controller_tick(&a, &jitter[i]);
            assert(a.gain == locked);
        }
    }
    assert(a.state == ARC_V3_LOCK);

    /* MEDIUM: overload descends, a mild-high state refines by one, then the
     * clean lower state locks. */
    arc_v3_controller_reset(&a, &t, 62);
    drive_until_gain(&a, obs(65, 99, 450, 0), 58, 20);
    drive_until_gain(&a, obs(36, 100, 0, 0), 57, 40);
    drive_until_state(&a, obs(17, 99, 0, 0), ARC_V3_LOCK, 40);
    assert(a.gain == 57);

    /* CLOSE / ULTRA-CLOSE: sustained severe overload keeps moving downward.
     * Each -4 is still protected from stale post-write observations. */
    arc_v3_controller_reset(&a, &t, 62);
    drive_until_gain(&a, obs(65, 99, 400, 0), 58, 20);
    unsigned settle_after_cut = a.settle;
    assert(settle_after_cut > 0u);
    (void)tick_values(&a, 65, 99, 400, 0);
    assert(a.gain == 58); /* one stale-looking raw window cannot chain a cut */
    drive_until_gain(&a, obs(65, 99, 400, 0), 54, 20);
    drive_until_gain(&a, obs(65, 99, 400, 0), 50, 20);

    /* WALK-BACK: after a legitimate downward move, brief starvation cannot
     * reverse the trajectory. This specifically guards the measured
     * G79 -> G69 -> G66 -> G81 bounce while walking toward the VTX. */
    arc_v3_controller_reset(&a, &t, 69);
    drive_until_gain(&a, obs(40, 99, 0, 0), 68, 40);
    assert(a.up_guard_ticks > 0u);
    uint8_t down_gain = a.gain;
    for (unsigned i = 0; i < 10; ++i) {
        (void)tick_values(&a, 1, 0, 0, 1000);
        assert(a.gain == down_gain);
    }
    feed(&a, obs(17, 99, 0, 0), 20);
    assert(a.gain == down_gain);

    /* Persistent real starvation is still allowed to reverse after the guard
     * expires; the controller is stable, not one-way. */
    drive_until_gain(&a, obs(1, 0, 0, 1000), 72, 60);

    /* At the table ceiling, persistent median starvation becomes RF_LIMIT. */
    arc_v3_controller_reset(&a, &t, 81);
    drive_until_state(&a, obs(1, 0, 0, 1000), ARC_V3_RF_LIMIT, 40);
    assert(a.gain == 81);

    /* A sustained stronger signal exits RF_LIMIT and overload protection can
     * move down again. */
    drive_until_gain(&a, obs(65, 99, 300, 0), 77, 20);
    assert(a.state == ARC_V3_ACQUIRE);

    puts("arc_v3_controller temporal tests passed");
    return 0;
}
