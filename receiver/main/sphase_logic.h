/* C5VRX-5 by Twotoz and the C5VRX contributors: sampling-phase verdict
 * (pure logic, host test test/test_sphase.c).
 *
 * PARLIO RX and MODEM_DIAG run from the same PLL with a phase fixed at
 * reset; a read that lands on a MODEM_DIAG transition mixes old and new bits
 * (zerowidth/C5VRX PR #3). The fourth-difference excess (dsp.h) rises for
 * such reads, but also for intrinsic broadband noise: at the table-maximum
 * gain without a carrier, or with DAC/TX self-noise in another TX context,
 * the cleanest position itself can read above the absolute limit.
 * Board 2026-10-12: boots settled while repeated explicit scans reported
 * `unsettled` (PR #196 CVT_STATUS.md).
 *
 * The positions repeat every three RX ticks, so a scan over nine slips
 * sees every position three times. A mid-transition read is only evidence
 * when the positions differ: the scan's worst-best contrast must exceed
 * twice the margin. Without contrast the excess is intrinsic noise, every
 * position is equivalent and slipping cannot help: NOISE_LIMITED, no repair.
 * With contrast, the current position must lie within the margin of the
 * best one; the absolute limit remains the fast path that needs no scan. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPHASE_LIMIT       300   /* milli-cell^2, absolute excess (dsp.h) */
typedef enum { SPHASE_SETTLED, SPHASE_NOISE_LIMITED, SPHASE_UNSETTLED } sphase_verdict_t;

typedef struct {
    bool valid;          /* a scan measured at least two positions */
    unsigned positions;
    int best, worst;     /* excess over the scanned positions */
} sphase_scan_t;

static inline int sphase_margin(int best)
{
    int m = best / 4;
    return m > 100 ? m : 100;
}

/* The scanned positions differ by more than measurement scatter. */
static inline bool sphase_contrast(const sphase_scan_t *s)
{
    return s->valid && s->worst - s->best > 2 * sphase_margin(s->best);
}

static inline void sphase_scan_reset(sphase_scan_t *s)
{
    s->valid = false;
    s->positions = 0;
    s->best = 0x7fffffff;
    s->worst = -1;
}

static inline void sphase_scan_add(sphase_scan_t *s, int excess)
{
    if (excess < s->best) s->best = excess;
    if (excess > s->worst) s->worst = excess;
    s->valid = ++s->positions >= 2u;
}

static inline sphase_verdict_t sphase_classify(int excess, int limit, const sphase_scan_t *s)
{
    if (excess < limit) return SPHASE_SETTLED;
    if (!s || !s->valid) return SPHASE_UNSETTLED;
    if (!sphase_contrast(s)) return SPHASE_NOISE_LIMITED;
    return excess <= s->best + sphase_margin(s->best) ? SPHASE_SETTLED : SPHASE_UNSETTLED;
}

/* Sampling phase on the VTX carrier (C5VRX by Twotoz and the C5VRX
 * contributors, 2026-10-13). Host model (research README section 19,
 * sphase_study.py): a mid-transition read costs 0.3-0.6 dB of range when
 * one LSB lane mixes and 1.4-5 dB when several bits do, plus a grainy strong
 * picture. It must never stay unnoticed, so on the carrier the phase is
 * chosen, watched and repaired, not only checked at boot:
 * - Carrier context: burst-confirmed sync and Direct Gain at least
 *   SPHASE_LIVE_GAIN_MARGIN steps below the table maximum. The board read
 *   clean positions at -70..+42 there (dsp.h); receiver noise at the table
 *   maximum reads ~725 on a clean position, so such windows neither count
 *   nor clear anything.
 * - Choose: the first carrier window of a boot runs one scan over the three
 *   positions and settles on the cleanest one (carrier margin, not the
 *   noise margin), so a boot qualified on noise (VTX off) still ends on the
 *   position farthest from the transition.
 * - Watch: every SPHASE_LIVE_CHECK_US, against SPHASE_LIVE_LIMIT, half the
 *   absolute limit: an LSB lane that mixes in a third of the reads already
 *   crosses it, before the multi-bit range cliff.
 * - Repair: SPHASE_LIVE_CONFIRM consecutive carrier checks above it run the
 *   same short scan. No per-boot cap: a scan that finds no contrast (a false
 *   alarm) doubles the gap up to SPHASE_LIVE_GAP_MAX_US; one that moved the
 *   phase resets it. A slip moves the RX write point by a few samples once:
 *   far less than the verified kp/program restart. */
#define SPHASE_LIVE_GAIN_MARGIN 6
#define SPHASE_LIVE_LIMIT       150
#define SPHASE_LIVE_CONFIRM     2u
#define SPHASE_LIVE_GAP_US      60000000LL    /* after a repair that moved the phase */
#define SPHASE_LIVE_GAP_MAX_US  960000000LL   /* false alarms back off to 16 min */
#define SPHASE_LIVE_CHECK_US    10000000LL    /* routine carrier check (passive) */
#define SPHASE_LIVE_RECHECK_US  2000000LL     /* confirmation after a strike */

/* Settle tolerance on the carrier: the probe scatter there is a few
 * milli-cell^2, so stop within 30 of the cleanest position (or 25 %). */
static inline int sphase_carrier_margin(int best)
{
    int m = best / 4;
    return m > 30 ? m : 30;
}

/* Contrast on the carrier: the positions differ by more than twice the
 * carrier margin (the noise rule's 100 floor would hide one LSB lane). */
static inline bool sphase_carrier_contrast(const sphase_scan_t *s)
{
    return s->valid && s->worst - s->best > 2 * sphase_carrier_margin(s->best);
}

typedef struct {
    unsigned strikes, scans, moved, false_alarms, carrier_checks;
    int64_t last_scan_us, gap_us;
    bool scanned_once, qualified;   /* qualified: a carrier scan ran this boot */
} sphase_live_t;

static inline void sphase_live_reset(sphase_live_t *s)
{
    s->strikes = s->scans = s->moved = s->false_alarms = s->carrier_checks = 0;
    s->last_scan_us = 0;
    s->gap_us = SPHASE_LIVE_GAP_US;
    s->scanned_once = s->qualified = false;
}

static inline bool sphase_live_carrier(bool sync_recent, int gain, int max_gain)
{
    return sync_recent && gain + SPHASE_LIVE_GAIN_MARGIN <= max_gain;
}

/* One live check. Returns true when a carrier scan should run now: the
 * first carrier window of the boot, or a confirmed excursion after the gap. */
static inline bool sphase_live_tick(sphase_live_t *s, int excess, bool carrier, int64_t now_us)
{
    if (!carrier) return false;
    ++s->carrier_checks;
    if (!s->qualified) return true;
    if (excess < SPHASE_LIVE_LIMIT) { s->strikes = 0; return false; }
    if (++s->strikes < SPHASE_LIVE_CONFIRM) return false;
    return !s->scanned_once || now_us - s->last_scan_us >= s->gap_us;
}

/* A carrier scan ran. moved: it found contrast (the positions differ), so
 * the phase was chosen on evidence; otherwise every position read alike. */
static inline void sphase_live_done(sphase_live_t *s, bool contrast, bool repair, int64_t now_us)
{
    ++s->scans;
    s->strikes = 0;
    s->last_scan_us = now_us;
    s->scanned_once = s->qualified = true;
    if (contrast) {
        if (repair) ++s->moved;
        s->gap_us = SPHASE_LIVE_GAP_US;
    } else if (repair) {
        ++s->false_alarms;
        s->gap_us = s->gap_us * 2 > SPHASE_LIVE_GAP_MAX_US ? SPHASE_LIVE_GAP_MAX_US : s->gap_us * 2;
    }
}

/* Delay to the next live check: sooner while a strike waits for its
 * confirmation. */
static inline int64_t sphase_live_next_us(const sphase_live_t *s)
{
    return s->strikes ? SPHASE_LIVE_RECHECK_US : SPHASE_LIVE_CHECK_US;
}

static inline const char *sphase_verdict_name(sphase_verdict_t v)
{
    return v == SPHASE_SETTLED ? "settled" : v == SPHASE_NOISE_LIMITED ? "noise_limited" : "unsettled";
}

/* Fourth-difference energy as per mille of white noise: a white sequence
 * has HF4/HF2 = 70/6 (sums of squared [1,-4,6,-4,1] and [1,-2,1] weights);
 * band-limited IQ reads far lower, mid-transition reads move toward white.
 * Independent of the absolute noise level. */
static inline int sphase_white_permille(uint64_t d4, uint64_t d2)
{
    if (!d2) return 0;
    double r = ((double)d4 / 70.0) / ((double)d2 / 6.0);
    return (int)(1000.0 * r + 0.5);
}
