/* C5VRX-5 by Twotoz and the C5VRX contributors: pure part of the passive
 * field diagnostics (diag.c; host test test/test_diag.c).
 *
 * Reports AFC sync evidence and model-derived C/N, never recognisable
 * moving picture quality, luma SINAD, dBm or a proven physical cause.
 * FAST_EVIDENCE_LOSS can include fades, shadowing, VTX power changes or
 * sampling/measurement failures. DEMOD_CLIFF is only a candidate for replay.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    DIAG_UNKNOWN,
    DIAG_CALIBRATION_OR_PHY,
    DIAG_PROPAGATION_FADE,
    DIAG_AT_RF_FLOOR,
    DIAG_DEMOD_CLIFF,
    DIAG_CATEGORIES
};

static inline const char *diag_category_name(int c)
{
    static const char *const names[DIAG_CATEGORIES] = {"UNKNOWN", "CALIBRATION_OR_PHY",
        "FAST_EVIDENCE_LOSS", "AT_RF_FLOOR", "DEMOD_CLIFF"};
    return c >= 0 && c < DIAG_CATEGORIES ? names[c] : "?";
}

/* Chain-state flags of a record; any of them set at a dropout means
 * CALIBRATION_OR_PHY. */
#define DIAG_F_FILTER_MISMATCH 0x01u   /* requested filter not read back */
#define DIAG_F_SPHASE_BAD      0x02u   /* sampling phase not settled */
#define DIAG_F_NO_DC           0x04u   /* no DC correction held for the gain */
#define DIAG_F_FAULT           0x08u   /* new DMA/BitScrambler fault */
#define DIAG_F_AFC_STEP        0x40u   /* the AFC retuned the offset */
#define DIAG_F_CHAIN           0x4fu
/* Informational, not a chain fault. */
#define DIAG_F_AFC_LOCKED      0x10u
#define DIAG_F_OUT_OF_SPEC     0x20u   /* channel above the documented C5 range */

typedef struct {
    uint32_t t_ms;          /* observation time, ms; wraps after ~49 days */
    uint32_t t_s;           /* uptime, s */
    uint8_t video_pct;      /* sync-valid / ALL observation windows, % */
    uint8_t coverage_pct;   /* measurable / ALL observation windows, % */
    uint8_t sync_measured_pct; /* sync-valid / measurable, % */
    uint8_t measured;       /* event observation had usable AFC measurement */
    uint16_t frequency_mhz;
    uint8_t profile, filter_skirt, filter_bw20;
    int8_t filter_code;
    uint8_t rf_code, bb_code, fine_code;
    int16_t p95, cfo_khz;
    uint32_t gain_writes, fade_holds, overloads, faults;
    uint8_t gain, gain_max; /* mean gain index of the second, table maximum */
    uint8_t rf_stage;       /* vendor RF stage of the gain */
    int16_t p50;            /* observer envelope median (ultrafine cells^2 x16) */
    int16_t coherence;      /* observer phase coherence, % */
    int16_t cn_x10;         /* C/N estimate x10 dB (cn_estimate.h), -990 noise-like */
    int16_t origin_pm, clip_pm;
    int16_t offset_khz;     /* AFC tuning offset */
    int8_t kp_level;        /* verified kp ladder level (0 = kp 0.85), -1 unknown */
    uint8_t lp;             /* U85Q-LP program running */
    uint8_t flags;
} diag_record_t;

/* Thresholds on the C/N estimate (per 20 MHz; simulated usable-picture
 * threshold ~1-5 dB, research/README.md). */
#define DIAG_VIDEO_OK_PCT     50u   /* majority of windows have sync evidence */
#define DIAG_VIDEO_LOST_PCT   5u    /* very little confirmed sync evidence */
#define DIAG_FLOOR_CN_X10     20    /* at or below: at the receiver noise */
#define DIAG_WEAK_CN_X10      80    /* below: a weak carrier */
#define DIAG_CLIFF_CN_X10     60    /* at or above: RF still clearly there */
#define DIAG_FADE_HEADROOM    6u    /* gain steps below the maximum */

static inline bool diag_video_ok(const diag_record_t *r) { return r->video_pct >= DIAG_VIDEO_OK_PCT; }
static inline bool diag_video_lost(const diag_record_t *r) { return r->video_pct <= DIAG_VIDEO_LOST_PCT; }

static inline uint8_t diag_pct(unsigned part, unsigned total)
{
    return total ? (uint8_t)(part * 100u / total) : 0u;
}
static inline void diag_coverage(diag_record_t *r, unsigned windows, unsigned measured, unsigned sync)
{
    r->coverage_pct = diag_pct(measured, windows);
    r->video_pct = diag_pct(sync, windows);
    r->sync_measured_pct = diag_pct(sync, measured);
}

/* rec[0..n-1] oldest first; rec[n-1] is the first second without a
 * picture. Needs at least one second with a picture before it. */
static inline int diag_classify(const diag_record_t *rec, unsigned n)
{
    if (n < 2u) return DIAG_UNKNOWN;
    const diag_record_t *drop = &rec[n - 1u];
    int last_ok = -1;
    for (int k = (int)n - 2; k >= 0; --k)
        if (diag_video_ok(&rec[k])) { last_ok = k; break; }
    if (last_ok < 0) return DIAG_UNKNOWN;
    const diag_record_t *ok = &rec[last_ok];
    /* Chain state in the seconds from the last picture to the dropout. */
    for (unsigned k = (unsigned)last_ok; k < n; ++k)
        if (rec[k].flags & DIAG_F_CHAIN) return DIAG_CALIBRATION_OR_PHY;
    /* Missing/unusable AFC observations cannot diagnose a DSP or RF floor. */
    if (drop->coverage_pct < DIAG_VIDEO_OK_PCT) return DIAG_UNKNOWN;
    if (drop->cn_x10 == -990) return DIAG_UNKNOWN;
    /* RF-like structure remains: check decoding/output before propagation. */
    if (drop->cn_x10 >= DIAG_CLIFF_CN_X10) return DIAG_DEMOD_CLIFF;
    /* Rapid evidence loss with gain headroom; physical cause unknown. */
    bool fast = (int)n - 1 - last_ok <= 2;
    if (fast && ok->gain + DIAG_FADE_HEADROOM <= ok->gain_max) return DIAG_PROPAGATION_FADE;
    bool at_max_before = ok->gain + 1u >= ok->gain_max;
    if (at_max_before && ok->cn_x10 < DIAG_WEAK_CN_X10 && drop->cn_x10 <= DIAG_FLOOR_CN_X10)
        return DIAG_AT_RF_FLOOR;
    return DIAG_UNKNOWN;
}
