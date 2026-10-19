/* C5VRX by Twotoz and the C5VRX contributors: kp/LP ladder under the
 * control loop's window gate, driven by the real firmware headers
 * (main/kp_ladder.h, main/kp_switch_guard.h). Host model for
 * research/ladder_gate.py; not a hardware measurement.
 *
 * Every 50 ms control window follows control.c: the ladder update, the
 * two-second target confirmation, the shared five-second restart guard and,
 * after a restart, a new context and 500 ms of settling (no usable window).
 * The C/N estimate is the true C/N plus uniform noise; below -3 dB it reads
 * noise-like (CN_UNKNOWN_X10). The envelope-stationarity gate of the AFC
 * (afc2_envelope_stationary) passes a window with probability p(C/N), the
 * table measured by ladder_gate.py on sim.py IQ.
 *
 *   ladder_gate_trace MODE DEV_X100 NARROW SEED "s:cn_x10,..." FADE_X10 FADE_S NOISE_X10 "cn:p,cn:p,..."
 *   The true C/N is piecewise linear through the (seconds, C/N x10) points,
 *   plus a sine of FADE_X10 amplitude and FADE_S period (0 = none).
 *   MODE 0: before: stationarity gate on the ladder, exact-target confirmation
 *   MODE 1: ladder gated only by settle / gain write / context, exact target
 *   MODE 2: MODE 1 plus the directional confirmation (control.c now)
 *   MODE 3: stationarity gate kept, directional confirmation
 *   MODE 9: oracle, the curve's program and level at the true C/N, no guards
 * Prints one line per window: t_ms cn_true_x10 applied_level applied_lp. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kp_ladder.h"
#include "kp_switch_guard.h"

#define TICK_US 50000LL
#define SETTLE_TICKS 10

static int s_npts;
static int s_cn[64];
static double s_p[64];

static double pass_rate(int cn_x10)
{
    if (cn_x10 <= s_cn[0]) return s_p[0];
    for (int k = 1; k < s_npts; ++k)
        if (cn_x10 <= s_cn[k])
            return s_p[k - 1] + (s_p[k] - s_p[k - 1]) * (cn_x10 - s_cn[k - 1]) / (double)(s_cn[k] - s_cn[k - 1]);
    return s_p[s_npts - 1];
}

static int s_nseg;
static double s_ts[64];
static int s_tc[64];

static int trajectory(double t)
{
    if (t <= s_ts[0]) return s_tc[0];
    for (int k = 1; k < s_nseg; ++k)
        if (t <= s_ts[k]) return s_tc[k - 1] + (int)((s_tc[k] - s_tc[k - 1]) * (t - s_ts[k - 1]) / (s_ts[k] - s_ts[k - 1]));
    return s_tc[s_nseg - 1];
}

static double uniform(void) { return rand() / (RAND_MAX + 1.0); }

int main(int argc, char **argv)
{
    if (argc != 10) { fprintf(stderr, "usage: see source\n"); return 2; }
    const int mode = atoi(argv[1]), dev = atoi(argv[2]), narrow = atoi(argv[3]);
    srand((unsigned)atoi(argv[4]));
    const int fade = atoi(argv[6]), noise = atoi(argv[8]);
    const double fade_s = atof(argv[7]);
    for (char *tok = strtok(argv[5], ","); tok && s_nseg < 64; tok = strtok(NULL, ",")) {
        if (sscanf(tok, "%lf:%d", &s_ts[s_nseg], &s_tc[s_nseg]) == 2) ++s_nseg;
    }
    if (s_nseg < 1) return 2;
    const double seconds = s_ts[s_nseg - 1];
    for (char *tok = strtok(argv[9], ","); tok && s_npts < 64; tok = strtok(NULL, ",")) {
        if (sscanf(tok, "%d:%lf", &s_cn[s_npts], &s_p[s_npts]) == 2) ++s_npts;
    }
    if (s_npts < 2) return 2;

    kp_ladder_t l;
    kp_switch_guard_t confirm = {0}, restart = {0};
    kp_ladder_reset(&l);
    l.narrow = narrow != 0;
    /* The profile's start level (MATCHED narrow 0.60 / 0.50, else 0.85). */
    int applied = narrow ? (dev <= 80 ? 7 : 5) : 0;
    bool applied_lp = false;
    l.level = (uint8_t)applied;
    uint32_t ctx = 1;
    int settle = 0;
    const long ticks = (long)(seconds * 1e6 / TICK_US);
    for (long k = 0; k < ticks; ++k) {
        const int64_t now = k * TICK_US;
        const double t_s = now / 1e6;
        const int cn_true = trajectory(t_s) + (fade_s > 0 ? (int)lround(fade * sin(6.283185307179586 * t_s / fade_s)) : 0);
        int est = cn_true + (noise ? (int)(uniform() * (2 * noise + 1)) - noise : 0);
        if (est < -30) est = CN_UNKNOWN_X10;
        const bool control_ok = settle == 0;
        const bool stationary = uniform() < pass_rate(cn_true);
        const bool ladder_ok = (mode == 1 || mode == 2) ? control_ok : control_ok && stationary;
        if (settle) --settle;
        const uint8_t level = kp_ladder_update_lp(&l, est, ladder_ok, dev, now, true);
        const bool confirmed = mode >= 2 ? kp_switch_confirm_toward(&confirm, level, l.lp, applied, applied_lp, ctx,
                                                                     ladder_ok, now)
                                         : kp_switch_confirm(&confirm, level, l.lp, ctx, ladder_ok, now);
        if (mode == 9) {
            applied_lp = kp_ladder_want_lp(kp_ladder_class(dev), cn_true, applied_lp);
            applied = kp_ladder_target_f(cn_true, dev, applied_lp, narrow != 0);
        } else if (confirmed &&
            (level != applied || l.lp != applied_lp) && kp_switch_ready(&restart, now)) {
            applied = level;
            applied_lp = l.lp;
            kp_switch_mark(&restart, now);
            ++ctx;                       /* transport.c bumps the control epoch */
            settle = SETTLE_TICKS;
        }
        printf("%ld %d %d %d\n", (long)(now / 1000), cn_true, applied, applied_lp);
    }
    return 0;
}
