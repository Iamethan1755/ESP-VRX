/* C5VRX-5 by Twotoz and the C5VRX contributors: shared anti-flicker gate. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KP_SWITCH_GAP_US 5000000LL
#define KP_SWITCH_CONFIRM_US 2000000LL
/* Entering LP (the rescue) is confirmed and applied fast: the ladder already
 * held the wish for KP_LP_ENTER_HOLD_US, and a restart costs ~4.4 ms
 * (prog_restart_us on the board). Leaving LP and kp steps keep 2 s / 5 s. */
#define KP_SWITCH_RESCUE_CONFIRM_US 200000LL
#define KP_SWITCH_RESCUE_GAP_US     500000LL
/* kp_switch_confirm_toward(): an unusable window holds the candidate; only
 * a gap without usable windows longer than this drops it. One unusable
 * window used to clear 2 s of agreement, so 10-20 % unusable windows at the
 * range edge never let a switch through (board walk 2026-10-13,
 * research/ladder_replay.py). */
#define KP_SWITCH_STALE_US 500000LL

typedef struct {
    bool switched, candidate;
    int64_t last_switch_us, candidate_since_us;
    int level;
    bool lp;
    int8_t side;            /* kp_switch_confirm_toward: target side of the running level */
    uint32_t context;
    int64_t last_valid_us;  /* kp_switch_confirm_toward: last usable window */
    uint32_t resets;        /* candidates dropped before confirming (diagnostics) */
} kp_switch_guard_t;

static inline bool kp_switch_ready(const kp_switch_guard_t *g, int64_t now)
{
    return !g->switched || now - g->last_switch_us >= KP_SWITCH_GAP_US;
}

/* Into LP: only the short rescue gap. */
static inline bool kp_switch_ready_rescue(const kp_switch_guard_t *g, int64_t now)
{
    return !g->switched || now - g->last_switch_us >= KP_SWITCH_RESCUE_GAP_US;
}

static inline void kp_switch_mark(kp_switch_guard_t *g, int64_t now)
{
    g->switched = true;
    g->last_switch_us = now;
}

/* Require an unchanged kp/program pair and context with usable evidence.
 * Invalid windows break confirmation; they never count as stable evidence. */
static inline bool kp_switch_confirm(kp_switch_guard_t *g, int level, bool lp,
                                     uint32_t context, bool valid, int64_t now)
{
    if (!valid) { g->candidate = false; return false; }
    if (!g->candidate || g->level != level || g->lp != lp || g->context != context) {
        g->candidate = true;
        g->level = level;
        g->lp = lp;
        g->context = context;
        g->candidate_since_us = now;
        return false;
    }
    return now - g->candidate_since_us >= KP_SWITCH_CONFIRM_US;
}

/* Directional confirmation for the automatic ladder (control.c). The
 * candidate is the wanted program plus the side of the running kp level the
 * target lies on, not the exact level: the ladder moves one level every
 * 0.2-1 s while the C/N drifts, and an exact-level candidate restarted on
 * every step, so a fading range-edge carrier rarely got the program and kp
 * it asked for (research/ladder_gate.py, README section 18). A target back
 * on the running pair, or on the other side of it, still restarts the
 * confirmation, so oscillation at a threshold never switches. An unusable
 * window neither agrees nor disagrees: it holds the candidate up to
 * KP_SWITCH_STALE_US. The caller applies the latest target once this
 * returns true. */
static inline bool kp_switch_confirm_toward(kp_switch_guard_t *g, int level, bool lp, int applied_level,
                                            bool applied_lp, uint32_t context, bool valid, int64_t now)
{
    const int side = lp != applied_lp ? 2 : (level > applied_level) - (level < applied_level);
    /* Also when no window was offered at all (failed copies, scan). */
    if (g->candidate && now - g->last_valid_us > KP_SWITCH_STALE_US) { g->candidate = false; ++g->resets; }
    if (!valid) return false;
    g->last_valid_us = now;
    if (side == 0) { g->candidate = false; return false; }   /* reached or back: not a reset */
    if (!g->candidate || g->side != side || g->lp != lp || g->context != context) {
        if (g->candidate) ++g->resets;
        g->candidate = true;
        g->side = (int8_t)side;
        g->level = level;
        g->lp = lp;
        g->context = context;
        g->candidate_since_us = now;
        return false;
    }
    g->level = level;
    const bool rescue = side == 2 && lp;
    return now - g->candidate_since_us >= (rescue ? KP_SWITCH_RESCUE_CONFIRM_US : KP_SWITCH_CONFIRM_US);
}
