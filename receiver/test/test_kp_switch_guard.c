/* C5VRX-5 by Twotoz and the C5VRX contributors: anti-flicker regressions. */
#include <assert.h>
#include <stdio.h>
#include "kp_switch_guard.h"

int main(void)
{
    kp_switch_guard_t physical = {0}, candidate = {0};
    assert(kp_switch_ready(&physical, 0));
    kp_switch_mark(&physical, 100);
    assert(!kp_switch_ready(&physical, 100));
    assert(!kp_switch_ready(&physical, 100 + KP_SWITCH_GAP_US - 1));
    assert(kp_switch_ready(&physical, 100 + KP_SWITCH_GAP_US));
    /* Every actual operation shares this clock, including failed restarts
     * and manual requests: changing target or resetting the policy cannot
     * bypass the backend's five-second gap. */
    kp_switch_mark(&physical, 100 + KP_SWITCH_GAP_US);
    candidate = (kp_switch_guard_t){0};
    assert(!kp_switch_ready(&physical, 100 + KP_SWITCH_GAP_US + 1));

    assert(!kp_switch_confirm(&candidate, 1, false, 7, true, 0));
    assert(!kp_switch_confirm(&candidate, 1, false, 7, true, KP_SWITCH_CONFIRM_US - 1));
    assert(kp_switch_confirm(&candidate, 1, false, 7, true, KP_SWITCH_CONFIRM_US));
    assert(!kp_switch_confirm(&candidate, 1, true, 7, true, KP_SWITCH_CONFIRM_US));
    assert(!kp_switch_confirm(&candidate, 1, false, 7, true, KP_SWITCH_CONFIRM_US + 50000));
    /* Oscillation at a threshold never reaches confirmation. */
    for (int64_t t = 3000000; t < 20000000; t += 50000)
        assert(!kp_switch_confirm(&candidate, (int)((t / 50000) & 1), false, 7, true, t));
    assert(!kp_switch_confirm(&candidate, 2, true, 7, true, 20000000));
    assert(!kp_switch_confirm(&candidate, 2, true, 7, false, 21999999));
    assert(!kp_switch_confirm(&candidate, 2, true, 7, true, 22000000));
    assert(kp_switch_confirm(&candidate, 2, true, 7, true, 24000000));
    assert(!kp_switch_confirm(&candidate, 2, true, 8, true, 24000000));
    assert(!kp_switch_confirm(&candidate, 2, true, 8, true, 25999999));
    assert(kp_switch_confirm(&candidate, 2, true, 8, true, 26000000));

    /* Directional confirmation (control.c AUTO): running level 2, U85Q. */
    kp_switch_guard_t dir = {0};
    assert(!kp_switch_confirm_toward(&dir, 2, false, 2, false, 7, true, 0));        /* nothing to do */
    assert(!dir.candidate);
    /* A ladder stepping down 3, 4, 5 every 0.5 s confirms after 2 s and
     * carries the latest level; the exact-level guard restarted each step. */
    kp_switch_guard_t exact = {0};
    bool exact_ok = false, dir_ok = false;
    for (int64_t t = 0; t <= 2000000; t += 50000) {
        int level = 3 + (int)(t / 500000);
        if (level > 5) level = 5;
        exact_ok |= kp_switch_confirm(&exact, level, false, 7, true, t);
        dir_ok = kp_switch_confirm_toward(&dir, level, false, 2, false, 7, true, t);
    }
    assert(!exact_ok && dir_ok && dir.level == 5);
    /* Oscillation around the running level never confirms. */
    dir = (kp_switch_guard_t){0};
    for (int64_t t = 0; t < 20000000; t += 50000)
        assert(!kp_switch_confirm_toward(&dir, (t / 50000) & 1 ? 3 : 1, false, 2, false, 7, true, t));
    for (int64_t t = 0; t < 20000000; t += 50000)
        assert(!kp_switch_confirm_toward(&dir, (t / 50000) & 1 ? 3 : 2, false, 2, false, 7, true, t));
    /* A program change is keyed on the program: the level may move (leaving
     * LP, which keeps the 2 s confirmation). */
    dir = (kp_switch_guard_t){0};
    for (int64_t t = 0; t < 2000000; t += 50000)
        assert(!kp_switch_confirm_toward(&dir, t < 1000000 ? 1 : 0, false, 2, true, 7, true, t));
    assert(kp_switch_confirm_toward(&dir, 2, false, 2, true, 7, true, 2000000) && dir.level == 2 && !dir.lp);
    /* An unusable window holds the candidate (board walk 2026-10-13: one
     * unusable window cleared 2 s of agreement); a gap longer than
     * KP_SWITCH_STALE_US and a new context restart it. */
    dir = (kp_switch_guard_t){0};
    bool held_ok = false;
    for (int64_t t = 0; t <= 2000000; t += 50000)
        held_ok = kp_switch_confirm_toward(&dir, 2, false, 2, true, 7, (t / 50000) % 4 != 3, t);
    assert(held_ok && dir.resets == 0);
    dir = (kp_switch_guard_t){0};
    assert(!kp_switch_confirm_toward(&dir, 2, false, 2, true, 7, true, 0));
    for (int64_t t = 50000; t <= 1000000; t += 50000)                               /* 1 s unusable */
        assert(!kp_switch_confirm_toward(&dir, 2, false, 2, true, 7, false, t));
    assert(!dir.candidate && dir.resets == 1);
    bool again = false;
    for (int64_t t = 1050000; t <= 3050000; t += 50000) {
        again = kp_switch_confirm_toward(&dir, 2, false, 2, true, 7, true, t);
        assert(again == (t >= 3050000));
    }
    assert(!kp_switch_confirm_toward(&dir, 2, false, 2, true, 8, true, 3100000) && dir.resets == 2);
    for (int64_t t = 3150000; t <= 5100000; t += 50000)
        assert(kp_switch_confirm_toward(&dir, 2, false, 2, true, 8, true, t) == (t >= 5100000));
    /* No window offered at all (failed copies) is a gap too. */
    assert(!kp_switch_confirm_toward(&dir, 2, false, 2, true, 8, true, 6000000) && dir.resets == 3);
    /* Into LP confirms in 0.2 s; out of LP still needs 2 s. */
    dir = (kp_switch_guard_t){0};
    for (int64_t t = 0; t < KP_SWITCH_RESCUE_CONFIRM_US; t += 50000)
        assert(!kp_switch_confirm_toward(&dir, 0, true, 0, false, 9, true, t));
    assert(kp_switch_confirm_toward(&dir, 0, true, 0, false, 9, true, KP_SWITCH_RESCUE_CONFIRM_US));
    dir = (kp_switch_guard_t){0};
    for (int64_t t = 0; t < KP_SWITCH_CONFIRM_US; t += 50000)
        assert(!kp_switch_confirm_toward(&dir, 0, false, 0, true, 9, true, t));
    assert(kp_switch_confirm_toward(&dir, 0, false, 0, true, 9, true, KP_SWITCH_CONFIRM_US));
    kp_switch_guard_t gap = {0};
    kp_switch_mark(&gap, 0);
    assert(!kp_switch_ready_rescue(&gap, KP_SWITCH_RESCUE_GAP_US - 1) && kp_switch_ready_rescue(&gap, KP_SWITCH_RESCUE_GAP_US));
    assert(!kp_switch_ready(&gap, KP_SWITCH_RESCUE_GAP_US));
    puts("PASS: rescue confirm and gap,  shared five-second cooldown, two-second confirmation, jitter, held unusable windows, stale gaps, contexts, direction");
    return 0;
}
