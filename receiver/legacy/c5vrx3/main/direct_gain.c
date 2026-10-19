#include "direct_gain.h"
#include <string.h>

#define DIRECT_GAIN_TARGET_P      22
#define DIRECT_GAIN_DEADBAND_LO   19
#define DIRECT_GAIN_DEADBAND_HI   25
#define DIRECT_GAIN_SETTLE_TICKS  1u
#define DIRECT_GAIN_MAX_SLEW_UP   4u   /* The sole slew limit for Direct Gain */
#define DIRECT_GAIN_MAX_SLEW_DOWN 6u

/* Inverse-Q4 Gain Transfer LUT:
 * Precomputed delta-gain steps required to steer P_median from current P to sweet spot P_target (~22).
 * P is I^2+Q^2: the required correction is 10*log10(22/P)/0.82,
 * NOT 20*log10(22/P)/0.82. Index spacing is only a local heuristic;
 * vendor RF tuples are not globally monotonic. Values rounded to nearest.
 * Deadband [19..25] -> 0 steps (perfect envelope, zero hunting, instant HOLD).
 */
static const int8_t s_p_to_delta_gain[46] = {
    /*  0 */   0, /* cannot infer an amplitude from zero power */
    /*  1 */ +16,
    /*  2 */ +13,
    /*  3 */ +11,
    /*  4 */  +9,
    /*  5 */  +8,
    /*  6 */  +7,
    /*  7 */  +6,
    /*  8 */  +5,
    /*  9 */  +5,
    /* 10 */  +4,
    /* 11 */  +4,
    /* 12 */  +3,
    /* 13 */  +3,
    /* 14 */  +2,
    /* 15 */  +2,
    /* 16 */  +2,
    /* 17 */  +1,
    /* 18 */  +1,
    /* 19 */   0, /* DEADBAND SWEET SPOT START */
    /* 20 */   0,
    /* 21 */   0,
    /* 22 */   0, /* IDEAL TARGET */
    /* 23 */   0,
    /* 24 */   0,
    /* 25 */   0, /* DEADBAND SWEET SPOT END */
    /* 26 */  -1,
    /* 27 */  -1,
    /* 28 */  -1,
    /* 29 */  -1,
    /* 30 */  -2,
    /* 31 */  -2,
    /* 32 */  -2,
    /* 33 */  -2,
    /* 34 */  -2,
    /* 35 */  -2,
    /* 36 */  -3,
    /* 37 */  -3,
    /* 38 */  -3,
    /* 39 */  -3,
    /* 40 */  -3,
    /* 41 */  -3,
    /* 42 */  -3,
    /* 43 */  -4,
    /* 44 */  -4,
    /* 45 */  -4,
};

static uint8_t clamp_gain(const direct_gain_controller_t *dg, int gain)
{
    uint8_t min_gain = 2u;
    uint8_t max_gain = dg->table.max_index ? dg->table.max_index : 81u;
    if (gain < (int)min_gain) return min_gain;
    if (gain > (int)max_gain) return max_gain;
    return (uint8_t)gain;
}

void direct_gain_reset(direct_gain_controller_t *dg,
                       const arc_gain_table_t *table,
                       uint8_t initial_gain)
{
    if (!dg) return;
    memset(dg, 0, sizeof(*dg));
    if (table) dg->table = *table;
    dg->current_gain = clamp_gain(dg, initial_gain);
    dg->target_gain = dg->current_gain;
    dg->state = DIRECT_GAIN_SEEK;
    dg->settle_ticks = 0;
    dg->last_p = 0;
    dg->last_delta_gain = 0;
    dg->last_estimated_input_dbm = -127;
    dg->last_rssi_used = false;
    dg->no_carrier_ticks = 0;
    dg->pending_direction = 0;
    dg->pending_votes = 0;
    dg->total_writes = 0;
    dg->hold_cycles = 0;
}

uint8_t direct_gain_tick(direct_gain_controller_t *dg,
                         const direct_gain_observation_t *obs)
{
    if (!dg || !obs) return 52u;

    int p = obs->p_median;
    if (p < 0) p = 0;
    dg->last_p = p;

    /* Classify loss of carrier before interpreting noise/outer-bin occupancy
     * as an overload or a request for more gain. Never learn on these windows. */
    if (obs->q_phase < 18 && obs->origin_permille >= 650) {
        if (dg->no_carrier_ticks < 255u) ++dg->no_carrier_ticks;
        dg->settle_ticks = 0;
        dg->pending_direction = 0;
        dg->pending_votes = 0;
        dg->state = DIRECT_GAIN_SEEK;
        if (obs->survival_gain) {
            uint8_t survival = clamp_gain(dg, obs->survival_gain);
            /* Recover quickly if below survival. When a weak carrier drops
             * out above it, preserve the extra sensitivity for ~1 second. */
            if ((dg->current_gain < survival && dg->no_carrier_ticks >= 3u) ||
                (dg->current_gain > survival && dg->no_carrier_ticks >= 20u)) {
                dg->target_gain = survival;
                dg->current_gain = survival;
            }
        }
        return dg->current_gain;
    }
    dg->no_carrier_ticks = 0;

    /* 1. Wait one ~50 ms control tick after a write. This is decision
     * settling only: it does not blank the live DAC waveform. */
    if (dg->settle_ticks > 0) {
        --dg->settle_ticks;
        if (dg->settle_ticks == 0) {
            /* Settle complete: evaluate accuracy of the single-write hop (Slow Path verification) */
            if (p >= DIRECT_GAIN_DEADBAND_LO && p <= DIRECT_GAIN_DEADBAND_HI &&
                obs->clip_permille < 20) {
                /* Target envelope achieved in 1 write! Enter HOLD. */
                dg->state = DIRECT_GAIN_HOLD;
                dg->hold_ticks = 1;
                ++dg->hold_cycles;
            } else {
                /* One 102 us window cannot train a persistent offset in an
                 * uncalibrated vendor index. Wait for directional agreement. */
                dg->state = DIRECT_GAIN_SEEK;
            }
        }
        return dg->current_gain;
    }

    /* 2. QUALITY SEPARATION (Golden Rule)
     * Q_phase, origin_permille, and winding metrics represent signal quality/multipath,
     * NOT RF amplitude. If P is in the sweet spot, DO NOT TOUCH GAIN, even if Q is degraded! */
    if (p >= DIRECT_GAIN_DEADBAND_LO && p <= DIRECT_GAIN_DEADBAND_HI &&
        obs->clip_permille < 20) {
        dg->pending_direction = 0;
        dg->pending_votes = 0;
        dg->state = DIRECT_GAIN_HOLD;
        if (dg->hold_ticks < 65535u) ++dg->hold_ticks;
        return dg->current_gain;
    }

    /* 3. EMERGENCY OVERLOAD PROTECTION
     * Outer Q4-bin occupancy alone is not evidence of analog saturation. */
    if (p > 44) {
        dg->pending_direction = 0;
        dg->pending_votes = 0;
        int cut = -14;
        uint8_t next = clamp_gain(dg, (int)dg->current_gain + cut);
        if (next != dg->current_gain) {
            dg->current_gain = next;
            dg->target_gain = next;
            dg->settle_ticks = DIRECT_GAIN_SETTLE_TICKS;
            dg->state = DIRECT_GAIN_SETTLE;
            dg->last_delta_gain = cut;
            ++dg->total_writes;
            return dg->current_gain;
        }
    } else if (obs->clip_permille >= 80 && p > 35) {
        dg->pending_direction = 0;
        dg->pending_votes = 0;
        int cut = -8;
        uint8_t next = clamp_gain(dg, (int)dg->current_gain + cut);
        if (next != dg->current_gain) {
            dg->current_gain = next;
            dg->target_gain = next;
            dg->settle_ticks = DIRECT_GAIN_SETTLE_TICKS;
            dg->state = DIRECT_GAIN_SETTLE;
            dg->last_delta_gain = cut;
            ++dg->total_writes;
            return dg->current_gain;
        }
    }

    /* 4. POWER-DOMAIN FEED-FORWARD
     * Approximate local index correction from the power ratio. */
    int lut_idx = p > 45 ? 45 : p;
    int delta = (int)s_p_to_delta_gain[lut_idx];

    /* RSSI is telemetry only until forced-gain sweeps establish whether it
     * is pre-gain, monotonic and calibrated for this PHY/table. */
    bool carrier_authentic = obs->q_phase >= 50 && obs->origin_permille <= 350;
    if (obs->rssi_valid && obs->rssi_dbm >= -100 && obs->rssi_dbm <= -10 && carrier_authentic) {
        dg->last_rssi_used = false;
        dg->last_estimated_input_dbm = obs->rssi_dbm;
    } else {
        dg->last_rssi_used = false;
        /* Estimate input power: known gain + current P */
        dg->last_estimated_input_dbm = -90 + (int)dg->current_gain - (DIRECT_GAIN_TARGET_P - p);
    }

    dg->last_delta_gain = delta;
    dg->target_gain = clamp_gain(dg, (int)dg->current_gain + delta);

    /* A single 102 us descriptor is only 0.2% of the preceding 50 ms.
     * Confirm ordinary tracking direction in a second control window;
     * overload above remains immediate. This also prevents alternating
     * weak/strong outliers from causing opposing PHY writes. */
    int8_t direction = delta > 0 ? 1 : delta < 0 ? -1 : 0;
    if (direction != dg->pending_direction) {
        dg->pending_direction = direction;
        dg->pending_votes = direction ? 1u : 0u;
        dg->state = direction ? DIRECT_GAIN_SEEK : DIRECT_GAIN_HOLD;
        return dg->current_gain;
    }
    if (direction && dg->pending_votes < 2u) {
        ++dg->pending_votes;
        dg->state = DIRECT_GAIN_SEEK;
        if (dg->pending_votes < 2u) return dg->current_gain;
    }
    if (!direction) dg->pending_votes = 0;

    /* 5. TARGET ESTIMATE & ONE SLEW AUTHORITY
     * Bound active tracking to +4/-6 indices per tick. The physical writer
     * must not apply an additional cap; PHY transitions may still affect video. */
    if (delta != 0) {
        uint8_t desired_target = dg->target_gain;

        int step = (int)desired_target - (int)dg->current_gain;
        bool is_tracking = (p >= 8) || carrier_authentic || (dg->hold_ticks > 0);

        if (is_tracking) {
            if (step > (int)DIRECT_GAIN_MAX_SLEW_UP)   step = (int)DIRECT_GAIN_MAX_SLEW_UP;
            if (step < -(int)DIRECT_GAIN_MAX_SLEW_DOWN) step = -(int)DIRECT_GAIN_MAX_SLEW_DOWN;
        }

        uint8_t next_gain = clamp_gain(dg, (int)dg->current_gain + step);
        if (next_gain != dg->current_gain) {
            dg->current_gain = next_gain;
            dg->pending_direction = 0;
            dg->pending_votes = 0;
            dg->settle_ticks = DIRECT_GAIN_SETTLE_TICKS;
            dg->state = DIRECT_GAIN_SETTLE;
            dg->hold_ticks = 0;
            ++dg->total_writes;
        } else {
            dg->state = DIRECT_GAIN_HOLD;
            if (dg->hold_ticks < 65535u) ++dg->hold_ticks;
        }
    } else {
        dg->state = DIRECT_GAIN_HOLD;
        if (dg->hold_ticks < 65535u) ++dg->hold_ticks;
    }

    return dg->current_gain;
}

void direct_gain_sync_applied(direct_gain_controller_t *dg, uint8_t applied_gain)
{
    if (!dg) return;
    applied_gain = clamp_gain(dg, applied_gain);
    if (dg->current_gain != applied_gain) {
        dg->current_gain = applied_gain;
        dg->target_gain = applied_gain;
        dg->settle_ticks = DIRECT_GAIN_SETTLE_TICKS;
        dg->state = DIRECT_GAIN_SETTLE;
        dg->pending_direction = 0;
        dg->pending_votes = 0;
    }
}

const char *direct_gain_state_name(direct_gain_state_t state)
{
    switch (state) {
    case DIRECT_GAIN_HOLD:   return "HOLD";
    case DIRECT_GAIN_SETTLE: return "SETTLE";
    case DIRECT_GAIN_SEEK:   return "SEEK";
    default:                 return "UNKNOWN";
    }
}
