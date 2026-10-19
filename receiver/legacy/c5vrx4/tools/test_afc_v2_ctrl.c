#include "afc_v2_ctrl.h"
#include <assert.h>
#include <stdio.h>

static afc2_result_t est(int porch, int sync, int8_t pol, uint8_t std)
{
    afc2_result_t r = {0};
    r.sync_pairs = 100; r.porch_pairs = 40;
    r.lines = 1; r.polarity = pol; r.standard = std;
    r.porch_khz = porch; r.sync_khz = sync; r.burst_x10 = 40;
    return r;
}

int main(void)
{
    uint8_t lost = 0;
    assert(!afc2_native_lock(false, true, true, 15, &lost));
    assert(afc2_native_lock(false, true, true, 16, &lost));
    assert(!afc2_native_lock(false, true, false, 16, &lost));
    /* CFO drift alone must never turn tracking into frequency writes. */
    assert(afc2_native_lock(true, true, false, 16, &lost));
    for (unsigned k = 1; k < AFC2_NATIVE_LOST_WINDOWS; ++k)
        assert(afc2_native_lock(true, false, false, 0, &lost));
    assert(!afc2_native_lock(true, false, false, 0, &lost));

    afc2_ctrl_t c = {0};
    int32_t step = 0;
    afc2_ctrl_reset(&c, 1u, true);

    /* Needs a full set of samples. */
    for (unsigned k = 0; k + 1 < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t r = est(400, -2000, -1, 2);
        afc2_ctrl_observe(&c, &r);
        assert(!afc2_ctrl_decide(&c, true, &step));
    }
    afc2_result_t r = est(400, -2000, -1, 2);
    afc2_ctrl_observe(&c, &r);
    assert(!afc2_ctrl_decide(&c, false, &step));   /* ineligible (TRACK) */
    assert(afc2_ctrl_decide(&c, true, &step));
    assert(step == AFC2_CTRL_MAX_STEP_KHZ);          /* clamped 400 -> 250 */
    assert(c.n == 0 && c.corrections == 1);

    /* Own write changes context but keeps the correction count. */
    assert(afc2_ctrl_sync(&c, 2u, true) && c.corrections == 1);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(150 + (int)(k % 3) * 10 - 10, -2250, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    assert(afc2_ctrl_decide(&c, true, &step) && step == 150);

    /* Inside deadband: no write. */
    afc2_ctrl_sync(&c, 3u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(30, -2370, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    assert(!afc2_ctrl_decide(&c, true, &step));

    /* Unstable estimates (scene leakage / noise): no write. */
    afc2_ctrl_sync(&c, 4u, false);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est((k & 1u) ? 600 : -300, -2000, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    assert(!afc2_ctrl_decide(&c, true, &step));

    /* Polarity or standard change discards the collected samples. */
    afc2_ctrl_sync(&c, 5u, false);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES - 1; ++k) {
        afc2_result_t s = est(300, -2000, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    afc2_result_t flip = est(300, 2600, 1, 2);
    afc2_ctrl_observe(&c, &flip);
    assert(c.n == 1);
    afc2_result_t ntsc = est(300, 2600, 1, 1);
    afc2_ctrl_observe(&c, &ntsc);
    assert(c.n == 1);

    /* Loss of burst evidence invalidates all retained estimates. */
    afc2_result_t none = {0};
    afc2_ctrl_observe(&c, &none);
    assert(c.n == 0);
    afc2_ctrl_reset(&c, 8u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(400, -1900, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    for (unsigned k = 0; k < 100; ++k) afc2_ctrl_observe(&c, &none);
    assert(!afc2_ctrl_decide(&c, true, &step));
    afc2_result_t missing_pairs = est(400, -1900, -1, 2);
    missing_pairs.porch_pairs = 0;
    afc2_ctrl_observe(&c, &missing_pairs);
    assert(c.n == 0);

    /* Correction budget per acquisition. */
    afc2_ctrl_reset(&c, 6u, true);
    unsigned writes = 0;
    for (unsigned round = 0; round < 10u; ++round) {
        for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
            afc2_result_t s = est(500, -1800, -1, 2);
            afc2_ctrl_observe(&c, &s);
        }
        if (afc2_ctrl_decide(&c, true, &step)) {
            ++writes;
            afc2_ctrl_sync(&c, 100u + round, true);
        }
    }
    assert(writes == AFC2_CTRL_MAX_CORRECTIONS);
    /* A new acquisition (e.g. channel change) restores the budget. */
    assert(afc2_ctrl_sync(&c, 999u, false) && c.corrections == 0);

    /* Lock shares the correction stability test. A centred last sample does
     * not turn a noisy acquisition into a permanent TRACK freeze. */
    afc2_ctrl_reset(&c, 42u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(k + 1 == AFC2_CTRL_SAMPLES ? 0 :
                             (k & 1u) ? 600 : -300, -2000, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    assert(!afc2_ctrl_can_lock(&c, true));
    assert(!afc2_ctrl_can_lock(&c, false));
    afc2_ctrl_reset(&c, 43u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(k + 1 == AFC2_CTRL_SAMPLES ? 0 : 200, -2000, -1, 2);
        afc2_ctrl_observe(&c, &s);
    }
    assert(!afc2_ctrl_can_lock(&c, true));
    /* OFF/HOLD freezes a stable receive state without demanding zero CFO. */
    assert(afc2_ctrl_can_lock(&c, false));
    afc2_ctrl_reset(&c, 44u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(30, -2000, -1, 2);
        afc2_ctrl_observe(&c, &s);
        assert(afc2_ctrl_can_lock(&c, true) == (k + 1 == AFC2_CTRL_SAMPLES));
    }

    /* Sync/porch midpoint reference. */
    afc2_ctrl_t m = {0};
    m.ref = AFC2_REF_SYNC_MID;
    afc2_ctrl_reset(&m, 1u, true);
    for (unsigned k = 0; k < AFC2_CTRL_SAMPLES; ++k) {
        afc2_result_t s = est(1100, -900, -1, 2);
        afc2_ctrl_observe(&m, &s);
    }
    assert(afc2_ctrl_decide(&m, true, &step) && step == 100);

    puts("AFC V2 acquisition decision (#115): passed");
    return 0;
}
