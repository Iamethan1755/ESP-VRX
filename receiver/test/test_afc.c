/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the AFC
 * acquisition decision (main/afc_ctrl.h), driven the way control.c drives
 * it: one burst-confirmed estimate per 50 ms window, a correction moves the
 * tuning offset, the context change after our own write keeps the budget.
 *
 * Why it matters: the C5VRX-5 stress matrix (research/situations.py)
 * shows that a residual carrier error of -0.75 / -1.0 MHz already costs the
 * picture at 4 dB C/N, so the AFC must actually reach the centre for VTXs
 * whose frequency is more than 1 MHz off, and must not hunt on noise.
 * Weak-signal gating (cn_estimate.h) sits in control.c and is tested in
 * test_diag.c; here every window is a strong one.
 *
 *   cc -O2 -std=c11 -I../main test_afc.c -lm -o test_afc && ./test_afc
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afc_ctrl.h"

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static double uniform(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2.0 * log(uniform())) * cos(6.283185307179586 * uniform()); }

static unsigned s_fail, s_run;
static void check(const char *name, int ok, const char *detail)
{
    ++s_run;
    printf("%-4s %-52s %s\n", ok ? "ok" : "FAIL", name, detail);
    s_fail += !ok;
}

/* Simulates `seconds` of reception of a VTX whose porch sits `vtx_khz` away
 * from the target; estimate noise `sigma_khz`. Returns the final residual
 * error, sets *locked and *corrections. Offset clamp as rf_set_offset_khz. */
/* plant_sign = +1: raising the offset lowers the estimate (the sign the
 * firmware assumes); -1: the opposite (never confirmed on hardware, C5VRX-4
 * INTEGRATION.md "VTX sign/reference calibration pending"). */
static int s_plant_sign = 1;
static int run(int vtx_khz, double sigma_khz, double seconds, int *locked, unsigned *corrections)
{
    afc2_ctrl_t c;
    memset(&c, 0, sizeof(c));
    uint32_t ctx = 1;
    afc2_ctrl_reset(&c, ctx, true);
    int offset = 0;
    bool lock = false, own = false;
    uint8_t lost = 0;
    *corrections = 0;
    for (unsigned w = 0; w < (unsigned)(seconds * 20.0); ++w) {
        afc2_ctrl_sync(&c, ctx, own);
        own = false;
        int err = vtx_khz - s_plant_sign * offset;
        afc2_result_t r;
        memset(&r, 0, sizeof(r));
        r.lines = 8; r.standard = 2; r.polarity = -1; r.burst_x10 = 40; r.sync_pairs = 200; r.porch_pairs = 300;
        r.porch_khz = (int32_t)lround(err + sigma_khz * gauss()) + c.target_khz;
        r.sync_khz = r.porch_khz - 3000;
        afc2_ctrl_observe(&c, &r);
        bool was = lock;
        lock = afc2_native_lock(lock, true, afc2_ctrl_can_lock(&c, true), c.n, &lost);
        if (was && !lock) afc2_ctrl_reset(&c, ctx, true);
        int32_t step;
        if (!lock && afc2_ctrl_decide(&c, true, &step)) {
            offset += step;
            if (offset > 2500) offset = 2500;     /* rf_set_offset_khz clamp */
            if (offset < -2500) offset = -2500;
            ++ctx;              /* the retune changes the context ... */
            own = true;         /* ... but it was our own write */
            ++*corrections;
        }
    }
    *locked = lock;
    return vtx_khz - s_plant_sign * offset;
}

int main(void)
{
    char name[96], detail[96];
    /* VTX frequency errors the offset clamp (+-2.5 MHz) can cover. */
    const int vtx[] = {0, 300, -300, 900, -900, 1200, -1200, 1450, -1450, 2000, -2000, 2400, -2400};
    for (unsigned k = 0; k < sizeof(vtx) / sizeof(vtx[0]); ++k) {
        int locked; unsigned n;
        int res = run(vtx[k], 40.0, 20.0, &locked, &n);
        snprintf(name, sizeof(name), "VTX %+5d kHz off: centred and locked within 20 s", vtx[k]);
        snprintf(detail, sizeof(detail), "residual=%d kHz locked=%d corrections=%u", res, locked, n);
        check(name, abs(res) <= AFC2_CTRL_DEADBAND_KHZ + 20 && locked, detail);
    }
    /* No hunting: a centred carrier with noisy estimates never retunes. */
    for (unsigned k = 0; k < 4u; ++k) {
        const double sigma[] = {20.0, 40.0, 60.0, 80.0};
        int locked; unsigned n;
        int res = run(0, sigma[k], 60.0, &locked, &n);
        snprintf(name, sizeof(name), "centred, estimate noise %2.0f kHz: no hunting", sigma[k]);
        snprintf(detail, sizeof(detail), "residual=%d corrections=%u", res, n);
        check(name, n <= 1u, detail);
    }
    /* The offset sign was never confirmed on hardware: with the plant
     * inverted, the AFC must still centre (and not walk to the clamp). */
    s_plant_sign = -1;
    const int inv[] = {300, -300, 900, -900, 1450, -1450, 2000, -2000};
    for (unsigned k = 0; k < sizeof(inv) / sizeof(inv[0]); ++k) {
        int locked; unsigned n;
        int res = run(inv[k], 40.0, 30.0, &locked, &n);
        snprintf(name, sizeof(name), "inverted offset sign, VTX %+5d kHz: centred", inv[k]);
        snprintf(detail, sizeof(detail), "residual=%d kHz locked=%d corrections=%u", res, locked, n);
        check(name, abs(res) <= AFC2_CTRL_DEADBAND_KHZ + 20 && locked, detail);
    }
    s_plant_sign = 1;
    {   /* Missing windows (failed copy, no burst) keep the estimate window up
         * to AFC2_CTRL_MAX_MISSES in a row; more clear it. */
        afc2_ctrl_t c;
        memset(&c, 0, sizeof(c));
        afc2_ctrl_reset(&c, 1, true);
        afc2_result_t good, none;
        memset(&good, 0, sizeof(good));
        memset(&none, 0, sizeof(none));
        good.lines = 8; good.standard = 2; good.polarity = -1; good.burst_x10 = 40; good.sync_pairs = 200;
        good.porch_pairs = 300; good.porch_khz = 120; good.sync_khz = -2880;
        for (unsigned w = 0; w < 40u; ++w) {
            if (w % 3u == 2u) afc2_ctrl_miss(&c);                  /* a third of the copies fail */
            else if (w % 3u == 1u) afc2_ctrl_observe(&c, &none);   /* no burst */
            else afc2_ctrl_observe(&c, &good);
        }
        snprintf(detail, sizeof(detail), "n=%u", c.n);
        check("2/3 missing windows: estimates kept", c.n >= 13u, detail);
        for (unsigned w = 0; w <= AFC2_CTRL_MAX_MISSES; ++w) afc2_ctrl_miss(&c);
        snprintf(detail, sizeof(detail), "n=%u", c.n);
        check("more than 1 s without evidence: cleared", c.n == 0u, detail);
    }
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
