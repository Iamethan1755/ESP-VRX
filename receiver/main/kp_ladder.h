/* C5VRX-5 by Twotoz and the C5VRX contributors: kp ladder target policy.
 *
 * The U85Q tracker loop gain kp sets the video bandwidth: a calmer tracker
 * (lower kp) extends the FM threshold at the range edge; on a strong carrier
 * kp 0.75-0.80 is slightly better than 0.85 too (research README section
 * 14). The ladder picks a level (kp_tables.h) from the C/N estimate and the
 * deviation class measured this session. Physical RUN/PAUSE LUT writes
 * failed verification; transport.c now applies a verified full target via
 * a brief pipeline restart. kp_switch_guard.h adds target confirmation and
 * a shared cooldown before actual kp/program changes.
 *
 * - The deep levels only for a VTX whose deviation was measured this
 *   session: a calm tracker on a wide VTX loses the picture. Unknown or wide
 *   deviation uses the wide row (kp 0.85 near the floor, 0.80 above).
 * - One level per step, after the target has held for KP_HOLD_US, and at
 *   most one step per KP_STEP_GAP_US, so estimate noise does not hop between
 *   levels. Toward a sharper kp (lower level) the hold and gap are shorter:
 *   near the floor a calm tracker loses lock first, so a collapsing carrier
 *   must get back up quickly.
 * - A noise-like window (CN_UNKNOWN_X10: envelope at or below plain noise)
 *   is the floor, not missing evidence: it targets the curve's 0 dB point.
 *   Only an unusable window (retune, gain write, menu) holds the level:
 *   kp_ladder_window_usable(); a short unusable gap keeps pending changes. The AFC's envelope-stationarity test is not
 *   part of it; receiver noise near the threshold fails it, and with it the
 *   ladder froze at the range edge (research README section 18).
 * LP follows the gain even without usable IQ; only retune/PHY ownership
 * holds that policy. Missing C/N holds kp. The C/N estimate comes from the
 * IQ envelope, not from the tracker,
 * so a kp change cannot feed back into it. Pure logic: host test
 * test/test_kp_ladder.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cn_estimate.h"

#define KP_LADDER_LEVELS   8           /* kp 0.85 .. 0.50 in 0.05 steps */
#define KP_HOLD_US         1000000LL   /* toward a calmer kp: the target must hold this long */
#define KP_STEP_GAP_US     500000LL    /* ... and at least this between steps */
#define KP_HOLD_UP_US      300000LL    /* toward a sharper kp (better lock near the floor) */
#define KP_STEP_GAP_UP_US  200000LL

/* LP is the rescue: into it fast. Out of it after a hold that adapts to
 * flapping: 2 s on a calm link (back to the sharper U85Q soon), doubled each
 * time LP is needed again within 30 s of leaving it, up to 16 s. Replay on
 * the board walks (research/ladder_replay.py, ADAPTIVE=1): 9 program
 * switches in 250 s against 13-19 for a fixed 3-5 s hold, and the most LP
 * time where sync was poor (board walk 2026-10-13: LP on 2-4 s and off
 * again, 12 switches). */
#define KP_LP_ENTER_HOLD_US   300000LL    /* C/N must ask for LP this long */
#define KP_LP_ENTER_GAP_US    500000LL    /* ... and this long after the last program switch */
#define KP_LP_RELEASE_BASE_US 2000000LL   /* back to U85Q after this long on a calm link */
#define KP_LP_RELEASE_MAX_US  16000000LL
#define KP_LP_FLAP_WINDOW_US  30000000LL  /* LP needed again within this: double the hold */
#define KP_LP_GAP_US          3000000LL   /* leaving: at least this after the last program switch */

/* The program follows the receiver's gain, not the coherence C/N model:
 * the gain index is the received level, and LP is for a link that has run
 * out of gain. The model read 9-13 dB next to the VTX (LP threshold 8.5 dB)
 * and 4-20 dB on the uncorrected DC with no VTX (board walks 2026-10-13).
 * On the board receiver noise becomes visible from ~G70 (first DCO row with
 * a noise-dithered search); sync was poor from ~G40-55 in the walks, but
 * those include the DC park. Operator choice: LP from G70, back below G65. */
#define KP_LP_GAIN_ON  70u
#define KP_LP_GAIN_OFF 65u

typedef struct {
    uint8_t level;          /* current target level, 0 = kp 0.85 */
    bool lp;                /* U85Q-LP program (post-detection low-pass) */
    int8_t lp_pending;      /* program wanted since lp_since: -1 none, 0 U85Q, 1 LP */
    int64_t lp_since;
    int64_t lp_switch_us;   /* last program switch */
    uint32_t lp_switches;
    int32_t cn_q4;          /* smoothed C/N (x10, Q4), window jitter filtered */
    bool cn_init;
    int8_t pending_dir;     /* direction of the target seen since pending_since */
    int64_t pending_since;
    int64_t last_step_us;
    int64_t last_usable_us; /* last usable C/N window */
    int64_t last_program_us; /* last usable gain/program observation */
    int64_t release_hold_us; /* current LP release hold (adaptive, see KP_LP_RELEASE_*) */
    int64_t released_us;     /* last LP -> U85Q */
    uint32_t steps;
    bool narrow;            /* narrow (MATCHED, 10 MHz class) filter: own curves; set by the caller */
    bool wide;              /* WIDE profile (16 MHz class) filter: own curves; set by the caller */
    uint8_t gain;           /* receiver gain index; set by the caller, 0 = unknown (C/N rule) */
} kp_ladder_t;

/* A control window the ladder may use: settled after a retune or restart,
 * unchanged context, no gain write in the last KP_GAIN_WRITE_GUARD_US. The
 * C/N comes from the gain observer's envelope, so a noisy, non-stationary
 * window is evidence (the floor), never a reason to hold.
 *
 * The envelope is one ~6 us observer probe taken right before the control
 * window reads it, and Direct Gain settles a write in 0.3-1 ms. A 100 ms
 * guard plus "no write during the 50 ms window" made every window unusable
 * while the carrier faded: 50-120 writes a second at the range edge (board
 * walk 2026-10-13, docs/FINDINGS.md). 5 ms covers the settle. */
#define KP_GAIN_WRITE_GUARD_US 5000LL
static inline bool kp_ladder_window_usable(bool settled, bool context_unchanged, int64_t since_gain_write_us)
{
    return settled && context_unchanged && since_gain_write_us >= KP_GAIN_WRITE_GUARD_US;
}

/* An unusable window holds the ladder; it does not erase a pending step or
 * program change. A single unusable window used to restart the 1 s LP hold:
 * with 10-20 % unusable windows the board changed LP internally 6 times and
 * never held long enough to apply (research/ladder_replay.py). Evidence
 * older than this gap is dropped. */
#define KP_STALE_US 500000LL

static inline void kp_ladder_reset(kp_ladder_t *l)
{
    l->level = 0;
    l->lp = false;
    l->narrow = false;
    l->gain = 0;
    l->wide = false;
    l->lp_pending = -1;
    l->lp_since = 0;
    l->lp_switch_us = -(1LL << 40);
    l->cn_init = false;
    l->pending_dir = 0;
    l->pending_since = 0;
    l->last_step_us = 0;
    l->last_usable_us = 0;
    l->last_program_us = 0;
    l->release_hold_us = KP_LP_RELEASE_BASE_US;
    l->released_us = -(1LL << 40);
}

/* Best kp (x100) per VTX deviation class at C/N 0, 2, 4, 8, 12, 20 dB:
 * research/best_combo.py, 8 seeds, 12 MHz (data/best_combo.json). The
 * best SINAD whose goggle lock stays within 0.02 of the best, kept off the
 * deepest kp near 0-2 dB where a calm tracker loses lock first. kp 0.80
 * beats 0.85 on a strong carrier for every deviation (+0.25..0.9 dB SINAD).
 * Unknown deviation uses the wide row. */
enum { KP_CLASS_LOW, KP_CLASS_NORMAL, KP_CLASS_MID, KP_CLASS_WIDE, KP_CLASSES };
static const int16_t kp_cn_points_x10[6] = {0, 20, 40, 80, 120, 200};
static const uint8_t kp_curve_x100[KP_CLASSES][6] = {
    {70, 55, 50, 50, 50, 75},   /* deviation <= 0.80 */
    {75, 60, 55, 55, 65, 75},   /* <= 1.05 */
    {80, 70, 65, 65, 70, 80},   /* <= 1.15 (interpolated, no own run) */
    {85, 85, 80, 80, 80, 80},   /* wider or unknown */
};

/* U85Q-LP (u85q_lp.bsasm: same LUT, IIR pole 1/2 after the tracker) beats
 * U85Q below ~6.5 dB C/N for every deviation and loses ~2-3.5 dB SINAD on
 * a strong carrier; at the same kp its goggle lock equals U85Q's
 * (best_combo.json). Its best kp per class at the same C/N points, the C/N
 * below which LP is used, and for wide VTXs the floor below which U85Q
 * kp 0.85 keeps lock better (README section 16). */
static const uint8_t kp_lp_curve_x100[KP_CLASSES][6] = {
    {75, 55, 50, 50, 50, 50},
    {80, 70, 65, 75, 75, 75},
    {85, 75, 70, 80, 80, 80},
    {85, 85, 85, 85, 85, 85},
};
/* The same choice on the narrow 10 MHz class of the MATCHED profiles
 * (research/best_combo.py, FILTER_MHZ=10, data/best_combo_10mhz.json). Only
 * the low and normal classes reach that filter; the others keep the 12 MHz
 * rows. Range limit against the old fixed MATCHED tracker: deviation 0.69
 * 0.59 vs 1.62 dB (kp 0.50), deviation 1.0 0.59 vs 1.10 dB (kp 0.60); close
 * in kp 0.80 / 0.85 are 0.5 dB SINAD sharper. The LP switch points stay
 * (crossover ~6.9 / ~6.5 dB). Simulation, not measured range. */
static const uint8_t kp_curve_narrow_x100[2][6] = {
    {50, 50, 50, 50, 50, 80},   /* deviation <= 0.80 */
    {80, 55, 50, 50, 60, 85},   /* <= 1.05 */
};
static const uint8_t kp_lp_curve_narrow_x100[2][6] = {
    {60, 50, 50, 50, 50, 50},
    {80, 70, 70, 70, 70, 70},
};
/* The narrow rows come from a 10 MHz filter model. The board's narrow
 * sweep entry measured 11.9 MHz (docs/FINDINGS.md): there the 12 MHz rows
 * fit better (data/best_combo.json, deviation 0.69: range limit 1.15 vs
 * 1.28 dB, SINAD at 30 dB 11.4 vs 11.1; deviation 1.0: equal limit, 13.7
 * vs 13.2 dB). So a MATCHED profile uses the narrow rows only when its
 * filter measured at most halfway between the two models; an unknown width
 * keeps them. Simulation, research README section 19. */
#define KP_NARROW_MAX_WIDTH_KHZ 11000u
static inline bool kp_ladder_narrow_rows(bool matched_profile, unsigned narrow_width_khz)
{
    return matched_profile && (narrow_width_khz == 0u || narrow_width_khz <= KP_NARROW_MAX_WIDTH_KHZ);
}

/* The WIDE profile's own rows (research/best_combo.py, FILTER_MHZ=16,
 * deviation 1.4 / 1.7 / 2.0, data/best_combo_16mhz.json; README section
 * 19). The 12 MHz wide row was made for an unknown or deviation-1.4 VTX on
 * the 12 MHz class; on the 16 MHz filter of a measured wide VTX:
 * - kp 0.85 everywhere: kp 0.80 costs 0.5 dB SINAD at 12 dB C/N for
 *   deviation 1.7 and 2.2-2.8 dB at 12-30 dB for deviation 2.0;
 * - LP up to 9.5 dB (crossovers ~9.5-13 dB), LP kp 0.85.
 * Range limit unchanged (2.75 / 2.86 / 3.96 dB): the existing LP rule
 * already reaches the best in that data; the gain is a sharper picture
 * between 8.5 dB and close in. Simulation, not measured range. */
static const uint8_t kp_curve_wide16_x100[6] = {85, 85, 85, 85, 85, 85};
static const uint8_t kp_lp_curve_wide16_x100[6] = {85, 85, 85, 85, 85, 85};
#define KP_LP_ON_WIDE16_X10 95

static const int16_t kp_lp_on_x10[KP_CLASSES] = {65, 65, 70, 85};
static const int16_t kp_lp_floor_x10[KP_CLASSES] = {-999, -999, -999, 15};
#define KP_LP_HYST_X10   15            /* leave LP 1.5 dB above where it is entered */

static inline int kp_ladder_class(int dev_x100)
{
    if (dev_x100 <= 0) return KP_CLASS_WIDE;
    if (dev_x100 <= 80) return KP_CLASS_LOW;
    if (dev_x100 <= 105) return KP_CLASS_NORMAL;
    if (dev_x100 <= 115) return KP_CLASS_MID;
    return KP_CLASS_WIDE;
}

/* kp (x100) the curve asks for at a C/N, linear between the points. wide
 * selects the WIDE profile's rows for the wide class only. */
static inline int kp_ladder_curve_g(int cls, int cn_x10, bool lp, bool narrow, bool wide)
{
    const uint8_t *k = narrow && cls <= KP_CLASS_NORMAL ? (lp ? kp_lp_curve_narrow_x100[cls] : kp_curve_narrow_x100[cls])
                     : wide && cls == KP_CLASS_WIDE ? (lp ? kp_lp_curve_wide16_x100 : kp_curve_wide16_x100)
                                                    : (lp ? kp_lp_curve_x100[cls] : kp_curve_x100[cls]);
    if (cn_x10 <= kp_cn_points_x10[0]) return k[0];
    for (int i = 1; i < 6; ++i)
        if (cn_x10 <= kp_cn_points_x10[i])
            return k[i - 1] + (k[i] - k[i - 1]) * (cn_x10 - kp_cn_points_x10[i - 1]) /
                              (kp_cn_points_x10[i] - kp_cn_points_x10[i - 1]);
    return k[5];
}

static inline int kp_ladder_curve_x100(int cls, int cn_x10, bool lp, bool narrow)
{
    return kp_ladder_curve_g(cls, cn_x10, lp, narrow, false);
}

/* Ladder level (0 = kp 0.85, 0.05 per level) nearest to a kp. */
static inline uint8_t kp_ladder_level_for(int kp_x100)
{
    int lv = (85 - kp_x100 + 2) / 5;
    return (uint8_t)(lv < 0 ? 0 : lv >= KP_LADDER_LEVELS ? KP_LADDER_LEVELS - 1 : lv);
}

static inline uint8_t kp_ladder_target_g(int cn_x10, int dev_x100, bool lp, bool narrow, bool wide)
{
    return kp_ladder_level_for(kp_ladder_curve_g(kp_ladder_class(dev_x100), cn_x10, lp, narrow, wide));
}

static inline uint8_t kp_ladder_target_f(int cn_x10, int dev_x100, bool lp, bool narrow)
{
    return kp_ladder_target_g(cn_x10, dev_x100, lp, narrow, false);
}

static inline uint8_t kp_ladder_target_lp(int cn_x10, int dev_x100, bool lp)
{
    return kp_ladder_target_f(cn_x10, dev_x100, lp, false);
}

/* Program the C/N asks for, with hysteresis around the current one. */
static inline bool kp_ladder_want_lp_g(int cls, int cn_x10, bool lp, bool wide)
{
    const int on = wide && cls == KP_CLASS_WIDE ? KP_LP_ON_WIDE16_X10 : kp_lp_on_x10[cls], floor = kp_lp_floor_x10[cls];
    if (lp) return cn_x10 < on + KP_LP_HYST_X10 && cn_x10 >= floor;
    return cn_x10 < on && cn_x10 >= floor + KP_LP_HYST_X10 / 3;
}

static inline bool kp_ladder_want_lp(int cls, int cn_x10, bool lp)
{
    return kp_ladder_want_lp_g(cls, cn_x10, lp, false);
}

/* Separate evidence: window_ok controls C/N/kp, program_ok controls LP.
 * A usable noise-like window passes CN_UNKNOWN_X10 and means the floor.
 * Returns the level the LUT should move to. */
static inline uint8_t kp_ladder_update_evidence(kp_ladder_t *l, int cn_x10, bool window_ok, bool program_ok,
                                                int dev_x100, int64_t now_us, bool allow_lp)
{
    if (!allow_lp && l->lp) {                   /* LP not allowed (mode change): back to U85Q at once */
        l->lp = false;
        l->lp_pending = -1;
        l->lp_switch_us = now_us;
        ++l->lp_switches;
    }
    /* Deeper than the wide row ever goes (deviation lost on a channel
     * change, or measured wide): back to that row's deepest level at once. */
    const uint8_t safe = kp_ladder_class(dev_x100) == KP_CLASS_WIDE ? kp_ladder_level_for(80) : KP_LADDER_LEVELS - 1;
    if (l->level > safe) {
        l->level = safe;
        l->pending_dir = 0;
        l->last_step_us = now_us;
        ++l->steps;
        return l->level;
    }
    if (now_us - l->last_usable_us > KP_STALE_US) l->pending_dir = 0;
    if (now_us - l->last_program_us > KP_STALE_US) l->lp_pending = -1;
    /* Per-window estimates jitter by a few dB: decide on a smoothed C/N
     * (first-order, 1/8 per 50 ms window, ~0.4 s). A noise-like window
     * counts as 0 dB. */
    if (window_ok) {
        l->last_usable_us = now_us;
        const int raw_cn = cn_x10 <= CN_UNKNOWN_X10 ? 0 : cn_x10;
        if (!l->cn_init) { l->cn_q4 = raw_cn * 16; l->cn_init = true; }
        else l->cn_q4 += (raw_cn * 16 - l->cn_q4) / 8;
    }
    const int cn = l->cn_init ? l->cn_q4 / 16 : 0;
    /* A known gain is evidence for LP without any IQ measurement. Retunes
     * and PHY ownership still hold it. The C/N fallback needs a window. */
    const bool program_evidence = program_ok && (l->gain || window_ok);
    if (program_evidence) l->last_program_us = now_us;
    if (allow_lp && program_evidence) {
        /* Program first: a switch lands on the new program's curve level at
         * this C/N, a quality-matched point (the curves cross near the
         * switch C/N), then kp steps on as usual. */
        const int8_t want = l->gain ? (l->gain >= (l->lp ? KP_LP_GAIN_OFF : KP_LP_GAIN_ON) ? 1 : 0)
                                    : (kp_ladder_want_lp_g(kp_ladder_class(dev_x100), cn, l->lp, l->wide) ? 1 : 0);
        if (want == (int8_t)l->lp) l->lp_pending = -1;
        else if (want != l->lp_pending) { l->lp_pending = want; l->lp_since = now_us; }
        else if (now_us - l->lp_since >= (want ? KP_LP_ENTER_HOLD_US : l->release_hold_us) &&
                 now_us - l->lp_switch_us >= (want ? KP_LP_ENTER_GAP_US : KP_LP_GAP_US)) {
            if (want) {
                const int64_t doubled = 2 * l->release_hold_us;
                l->release_hold_us = now_us - l->released_us < KP_LP_FLAP_WINDOW_US
                                     ? (doubled > KP_LP_RELEASE_MAX_US ? KP_LP_RELEASE_MAX_US : doubled)
                                     : KP_LP_RELEASE_BASE_US;
            } else l->released_us = now_us;
            l->lp = want != 0;
            l->lp_pending = -1;
            l->lp_switch_us = now_us;
            ++l->lp_switches;
            if (window_ok) l->level = kp_ladder_target_g(cn, dev_x100, l->lp, l->narrow, l->wide);
            l->pending_dir = 0;
            l->last_step_us = now_us;
            ++l->steps;
            return l->level;
        }
    }
    if (!window_ok) return l->level;
    const uint8_t target = kp_ladder_target_g(cn, dev_x100, l->lp, l->narrow, l->wide);
    const int8_t dir = target > l->level ? 1 : target < l->level ? -1 : 0;
    if (dir != l->pending_dir) {
        l->pending_dir = dir;
        l->pending_since = now_us;
        return l->level;
    }
    const int64_t hold = dir < 0 ? KP_HOLD_UP_US : KP_HOLD_US, gap = dir < 0 ? KP_STEP_GAP_UP_US : KP_STEP_GAP_US;
    if (dir && now_us - l->pending_since >= hold && now_us - l->last_step_us >= gap) {
        l->level = (uint8_t)(l->level + dir);
        l->last_step_us = now_us;
        ++l->steps;
    }
    return l->level;
}

/* Existing model callers offer a single validity flag for both policies. */
static inline uint8_t kp_ladder_update_lp(kp_ladder_t *l, int cn_x10, bool window_ok, int dev_x100, int64_t now_us,
                                          bool allow_lp)
{
    return kp_ladder_update_evidence(l, cn_x10, window_ok, window_ok, dev_x100, now_us, allow_lp);
}
