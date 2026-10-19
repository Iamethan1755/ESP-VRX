/* C5VRX-5 by Twotoz and the C5VRX contributors: automatic receive-chain
 * calibration, from legacy/c5vrx4/main/video_calibration.c, video_dco.c,
 * video_drift.c and video_sampling.c. One low-priority task, never in the
 * 40 MS/s path.
 *
 * Everything that interrupts the picture runs only in the STARTUP phase of
 * a boot (or once more after a manual request from the menu / console),
 * never in flight: a receiver must not drop the picture because it decided
 * to calibrate. The final sampling-phase qualification and the deviation
 * step run on the live picture, but still inside this bounded phase (a
 * repair slip there briefly disturbs it). Startup, bounded to ~2 minutes:
 *  1. sampling phase: PARLIO must not sample MODEM_DIAG mid-transition;
 *  2. narrow analog filter, only when none is stored: one sweep on the own
 *     channel (VTX off) or a quiet channel >= 40 MHz away (VTX on);
 *  3. per-gain RX DC correction, only for the gain rows of the channel that
 *     have none yet: missing rows on receiver noise (VTX off), or the
 *     current row on the carrier's circle (VTX on);
 *  4. VTX deviation and receive profile (profile.c): a fresh measurement
 *     on this boot's carrier, never a stored value alone.
 * Afterwards: held DC codes, small DC drift nudges and the gain map, which
 * do not interrupt video, plus passive monitoring (sampling phase, VTX
 * deviation, diag.c). A step that cannot run (no quiet channel, menu open,
 * ...) keeps the safe default and is tried again on the next boot. */
#include "receiver.h"
#include "calibration_noise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/temperature_sensor.h"
#include "dsp.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "phy.h"
#include "profile_logic.h"
#include "sphase_logic.h"
#include "dco_slots.h"
#include "rf.h"
#include "soc/pcr_struct.h"

extern unsigned char phy_param[];

typedef struct {
    uint32_t windows;
    uint64_t hf4_d4, hf4_count, hf2_d2;
    int dc_i, dc_q;          /* milli-cells: circle centre or plain mean */
    bool dc_circle;
    unsigned envelope_x100;
    metrics_t m;             /* last window */
} window_stats_t;

static volatile bool s_startup = true;
bool calibration_in_startup(void) { return s_startup; }

static bool quiet_owner(void)
{
    return !s_menu_active && !s_scan_active && !receiver_paused() && !phy_busy();
}

/* Status screens of the startup phase own TX (menu raster), so the menu
 * check must not count them as a busy owner. */
static bool startup_owner(void)
{
    return (!s_menu_active || menu_status_active()) && !s_scan_active && !receiver_paused() && !phy_busy();
}

static bool collect(unsigned windows, window_stats_t *out)
{
    uint8_t sample[PROBE_BYTES];
    dsp_circle_t circle = {0};
    memset(out, 0, sizeof(*out));
    for (unsigned tries = 0; tries < windows * 3u && out->windows < windows; ++tries) {
        vTaskDelay(2);   /* two ticks: one tick starved IDLE with the observer */
        if (!copy_probe_window(sample, NULL)) continue;
        for (unsigned r = 0; r < PROBE_REGIONS; ++r) {
            dsp_hf4_sums(sample + r * PROBE_REGION_BYTES, PROBE_REGION_BYTES, &out->hf4_d4);
            dsp_hf2_sums(sample + r * PROBE_REGION_BYTES, PROBE_REGION_BYTES, &out->hf2_d2);
        }
        out->hf4_count += PROBE_REGIONS * (PROBE_REGION_BYTES - 4u);
        dsp_circle_sums(sample, sizeof(sample), &circle);
        out->m = measure_window(sample, sizeof(sample));
        ++out->windows;
    }
    if (!out->windows) return false;
    out->dc_circle = dsp_circle_dc(&circle, &out->dc_i, &out->dc_q, &out->envelope_x100);
    return out->windows * 2u >= windows;
}

/* ---- Sampling phase --------------------------------------------------------
 * PARLIO RX (PLL_F240M/6) and MODEM_DIAG (every 3 ticks of 240 MHz) have a
 * phase fixed at reset; about 3 boots in 10 sample mid-transition (zerowidth
 * PR #3). The fourth-difference excess separates them on noise and on a
 * carrier; a bad phase is slipped one RX tick at a time, only in the
 * startup phase of every boot. Live, it is measured every 30 s and only
 * reported: a slip would interrupt the picture.
 *
 * Verdict (sphase_logic.h, test/test_sphase.c): the absolute limit is the
 * fast path; above it only a scan whose positions differ is evidence of a
 * mid-transition read. Positions that all read alike are intrinsic noise
 * (NOISE_LIMITED): no repair, not a phase fault. The stored DC row is held
 * while the gain worker is paused for a scan, and the final qualification
 * runs after the status screen has handed TX back to live video, because
 * the TX context changes the noise the probe sees (board 2026-10-12). */
#define SPHASE_POSITIONS  9u
#define SPHASE_TRIES      12u
static int s_sphase_excess = -1, s_sphase_white = -1, s_sphase_dc[2];
static unsigned s_sphase_scans;
static sphase_scan_t s_sphase_last = {.best = 0x7fffffff, .worst = -1};
static const char *s_sphase_state = "unverified";
static void dco_hold_for_scan(void);
static bool video_recent(void);

static int sphase_excess(const window_stats_t *w)
{
    int e = dsp_hf4_excess_milli(w->hf4_d4, w->hf4_count);
    return e > 0 ? e : 0;
}

/* The gate metric: absolute excess (HF4/HF2 whiteness is logged only; a
 * normalized gate failed board repeat scans, phasefix6). */
static int sphase_metric(const window_stats_t *w) { return sphase_excess(w); }

static int sphase_record(const window_stats_t *w)
{
    s_sphase_excess = sphase_excess(w);
    s_sphase_white = sphase_white_permille(w->hf4_d4, w->hf2_d2);
    s_sphase_dc[0] = w->dc_i;
    s_sphase_dc[1] = w->dc_q;
    return sphase_metric(w);
}

/* positions/windows/tries: startup 9 / 48 / 12 on any signal; on the
 * carrier 3 / 16 / 6 (one cycle, ~0.4 s with the control loop paused),
 * settling within the tight carrier margin of the cleanest position. */
static sphase_verdict_t sphase_scan_n(const char *tag, unsigned positions, unsigned windows, unsigned tries,
                                      bool carrier)
{
    if (!receiver_pause(tag)) return SPHASE_UNSETTLED;
    dco_hold_for_scan();
    window_stats_t w;
    sphase_scan_reset(&s_sphase_last);
    for (unsigned pos = 0; pos < positions; ++pos) {
        if (pos) { rx_clock_slip(1); vTaskDelay(pdMS_TO_TICKS(20)); }
        if (collect(windows, &w)) sphase_scan_add(&s_sphase_last, sphase_metric(&w));
    }
    sphase_verdict_t v = SPHASE_UNSETTLED;
    const bool contrast = carrier ? sphase_carrier_contrast(&s_sphase_last) : sphase_contrast(&s_sphase_last);
    if (s_sphase_last.valid && !contrast) {
        /* Every position reads alike: intrinsic noise, slipping cannot help. */
        if (collect(windows, &w))
            v = carrier ? (sphase_record(&w) < SPHASE_LIVE_LIMIT ? SPHASE_SETTLED : SPHASE_NOISE_LIMITED)
                        : sphase_classify(sphase_record(&w), SPHASE_LIMIT, &s_sphase_last);
    } else {
        /* Positions repeat every three ticks: stop on one near the cleanest. */
        for (unsigned n = 0; s_sphase_last.valid && n < tries; ++n) {
            if (!collect(windows, &w)) break;
            int e = sphase_metric(&w);
            const int margin = carrier ? sphase_carrier_margin(s_sphase_last.best) : sphase_margin(s_sphase_last.best);
            if (e <= s_sphase_last.best + margin) {
                (void)sphase_record(&w);
                v = SPHASE_SETTLED;
                break;
            }
            rx_clock_slip(1);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    receiver_resume();
    printf("SPHASE scan tag=%s positions=%u best=%d worst=%d contrast=%u verdict=%s\n", tag,
           s_sphase_last.positions, s_sphase_last.best, s_sphase_last.worst, contrast, sphase_verdict_name(v));
    return v;
}

static sphase_verdict_t sphase_scan(void)
{
    return sphase_scan_n("SPHASE", SPHASE_POSITIONS, 48u, SPHASE_TRIES, false);
}

static sphase_live_t s_sphase_live = {.gap_us = SPHASE_LIVE_GAP_US};

static bool sphase_on_carrier(void)
{
    return sphase_live_carrier(video_recent(), s_current_gain, rf_gain_table()->max_index);
}

/* One carrier scan: choose the cleanest of the three positions. repair:
 * started by an excursion (not the first, proactive choice of the boot). */
static void sphase_carrier_scan(const char *stage, bool repair)
{
    ++s_sphase_scans;
    sphase_verdict_t v = sphase_scan_n(repair ? "SPHASE_REPAIR" : "SPHASE_CARRIER", 3u, 16u, 6u, true);
    const bool contrast = sphase_carrier_contrast(&s_sphase_last);
    sphase_live_done(&s_sphase_live, contrast, repair, esp_timer_get_time());
    s_sphase_state = v == SPHASE_SETTLED ? "settled" : v == SPHASE_NOISE_LIMITED ? "noise_limited" : "drifted";
    printf("SPHASE %s carrier_scan repair=%u verdict=%s contrast=%u excess_mc2=%d best=%d worst=%d scans=%u "
           "moved=%u false_alarms=%u next_gap_s=%lld\n", stage, repair, sphase_verdict_name(v), contrast,
           s_sphase_excess, s_sphase_last.best, s_sphase_last.worst, s_sphase_live.scans, s_sphase_live.moved,
           s_sphase_live.false_alarms, s_sphase_live.gap_us / 1000000LL);
}

/* Measure, classify and (startup only) repair with at most two scans. A
 * fresh context drops the previous scan: its positions were measured under
 * another TX/noise context and are no evidence here. */
static void sphase_qualify(const char *stage, bool fresh_context)
{
    window_stats_t w;
    if (fresh_context) sphase_scan_reset(&s_sphase_last);
    dco_hold_for_scan();
    if (!collect(48, &w)) {
        s_sphase_state = "probe_failed";
        printf("SPHASE %s state=probe_failed\n", stage);
        return;
    }
    sphase_verdict_t v = sphase_classify(sphase_record(&w), SPHASE_LIMIT, &s_sphase_last);
    for (unsigned n = 0; v == SPHASE_UNSETTLED && n < 2u; ++n) {
        ++s_sphase_scans;
        v = sphase_scan();
    }
    s_sphase_state = sphase_verdict_name(v);
    printf("SPHASE %s state=%s excess_mc2=%d white_pm=%d dc_mcell=%d/%d scans=%u best=%d worst=%d\n",
           stage, s_sphase_state, s_sphase_excess, s_sphase_white, s_sphase_dc[0], s_sphase_dc[1],
           s_sphase_scans, s_sphase_last.valid ? s_sphase_last.best : -1, s_sphase_last.valid ? s_sphase_last.worst : -1);
}

/* Live, passive every 10 s. On receiver noise it only reports; on the
 * carrier it chooses the phase once per boot and repairs an excursion
 * above SPHASE_LIVE_LIMIT (sphase_logic.h, research README section 19). */
static void sphase_monitor(void)
{
    static int64_t next_us;
    int64_t now = esp_timer_get_time();
    if (now < next_us || !quiet_owner()) return;
    next_us = now + SPHASE_LIVE_CHECK_US;
    window_stats_t w;
    if (!collect(16, &w)) return;
    const int excess = sphase_record(&w);
    const bool carrier = sphase_on_carrier();
    /* On the carrier the live limit decides; the startup scan was measured
     * in another context. */
    sphase_verdict_t v = carrier ? (excess < SPHASE_LIVE_LIMIT ? SPHASE_SETTLED : SPHASE_UNSETTLED)
                                 : sphase_classify(excess, SPHASE_LIMIT, &s_sphase_last);
    if (v == SPHASE_UNSETTLED && strcmp(s_sphase_state, "drifted") != 0) {
        s_sphase_state = "drifted";
        printf("SPHASE WARNING live excess_mc2=%d white_pm=%d carrier=%u gain=%u\n", s_sphase_excess,
               s_sphase_white, carrier, s_current_gain);
    } else if (v != SPHASE_UNSETTLED && strcmp(s_sphase_state, "drifted") == 0) {
        s_sphase_state = sphase_verdict_name(v);
    }
    const bool qualified = s_sphase_live.qualified;
    const bool scan = sphase_live_tick(&s_sphase_live, excess, carrier, now);
    next_us = now + sphase_live_next_us(&s_sphase_live);
    if (scan) sphase_carrier_scan("live", qualified);
}

const char *calibration_sphase_state(void) { return s_sphase_state; }
/* NOISE_LIMITED is not a phase fault: every position reads the same noise. */
bool calibration_sphase_settled(void)
{
    return !strcmp(s_sphase_state, "settled") || !strcmp(s_sphase_state, "noise_limited");
}

/* ---- Narrow analog filter --------------------------------------------------
 * The FM threshold follows the pre-detection noise bandwidth, and PARLIO
 * keeps every second sample of the ~80 MS/s bus unfiltered, so the noise
 * the demod sees is set by the analog filter, alias included. The
 * capacitor codes are per-chip calibrated, so the filter is measured, not
 * assumed: with no carrier at maximum gain every setting (digital BW40/
 * BW20 x RX0 code x second-stage skirt) gets a 64-point noise spectrum, and
 * the one with the lowest noise bandwidth whose -3 dB width still covers
 * 14 MHz (enough for every tested VTX deviation, docs/FINDINGS.md) and whose
 * noise stays incoherent (so Direct Gain still recognises "no carrier")
 * is kept. Built on ESPARGOS esp-sdr's C5 BANDWIDTH control (GPL-3.0). */
/* 3: wide class; 4: codes 52/62/63 (one new sweep after the update) */
#define FILTER_VERSION     4u
/* Two classes from one sweep (research: 12 MHz helps every tested VTX
 * deviation and keeps strong-signal sharpness; 10 MHz adds up to ~1.7 dB
 * for deviation <= ~1.1 but hurts wide VTXs, so it is used only when the
 * deviation was measured). */
#define FILTER_MIN_KHZ     12000u
#define FILTER_NARROW_KHZ  10000u
/* Wide VTXs (deviation >= 1.6): the 12 MHz class clips their spectrum
 * (research/data/wide.txt). */
#define FILTER_WIDE_KHZ    16000u
/* Quiet test: dsp_quiet_reason() (dsp.h). */
#define QUIET_NO_SYNC_US   2000000LL
/* s_last_video_us starts at 0: "no sync seen yet" is not recent sync. */
static bool video_recent(void)
{
    const int64_t t = s_last_video_us;
    return t != 0 && esp_timer_get_time() - t < QUIET_NO_SYNC_US;
}
static unsigned quiet_limit(int clip_pm) { return dsp_quiet_limit(clip_pm); }
#define CAL_WINDOWS        96u
typedef struct {
    uint8_t valid, bw20, skirt;
    int8_t code;
    uint16_t width_khz, nbw_khz;
} filter_entry_t;
typedef struct {
    uint8_t version;
    filter_entry_t normal, narrow, wide;
    uint16_t calibrated_nbw_khz;
} filter_blob_t;
static filter_blob_t s_filter_cal;
static uint16_t s_original_mhz;
static const char *s_filter_result = "never";

static bool noise_measure(unsigned *width, unsigned *nbw, unsigned *env_x100, int *clip_pm)
{
    cal_noise_t a = {0};
    uint8_t sample[PROBE_BYTES];
    for (unsigned tries = 0; tries < CAL_WINDOWS * 3u && a.got < CAL_WINDOWS; ++tries) {
        vTaskDelay(1);
        if (!copy_probe_window(sample, NULL)) continue;
        cal_noise_add(&a, sample, sizeof(sample), measure_window(sample, sizeof(sample)).clip_pm);
    }
    return cal_noise_finish(&a, width, nbw, env_x100, clip_pm);
}

/* On-screen progress: the measured time per setting gives the time left. */
static unsigned s_cal_done, s_cal_total;
/* Higher code = narrower. Board sweep (FINDINGS, 2026-10-12): the narrow
 * class landed on code 60, the old grid's last code, at 11.9 MHz noise
 * bandwidth instead of the modelled 10 MHz; the field takes 0..63, so 62
 * and 63 are tried too, and 52 fills the gap around the 12 MHz class. */
#define FILTER_CODES  13u
#define FILTER_SKIRTS 5u
static int64_t s_cal_t0;
static bool s_cal_own_channel;
static void cal_status(void)
{
    char line1[48], line2[48];
    int64_t spent = esp_timer_get_time() - s_cal_t0;
    unsigned left = s_cal_done ? (unsigned)(spent / s_cal_done * (s_cal_total - s_cal_done) / 1000000) + 1u : 30u;
    if (s_cal_own_channel) snprintf(line1, sizeof(line1), "KEEP THE VTX OFF");
    else snprintf(line1, sizeof(line1), "ON QUIET CHANNEL %s (VTX MAY STAY ON)", rf_channel()->name);
    snprintf(line2, sizeof(line2), "ABOUT %u S LEFT, THEN LIVE VIDEO", left);
    status_screen("CALIBRATING", line1, line2, (int)(s_cal_done * 100u / s_cal_total));
}

/* *quiet stays false for a setting the PHY did not read back, so it can
 * never be chosen; false only when the probe itself fails. */
static bool measure_setting(rf_filter_t f, unsigned *width, unsigned *nbw, bool *quiet, bool *carrier)
{
    if (s_cal_total) { ++s_cal_done; cal_status(); }
    *quiet = *carrier = false;
    if (!rf_set_filter(f)) {
        printf("FILTER_CAL bw=%s code=%d skirt=%u skip=not_applied\n", f.bw20 ? "BW20" : "BW40", f.code, f.skirt);
        *width = *nbw = 0;
        return true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    unsigned env = 0;
    int clip = 0;
    if (!noise_measure(width, nbw, &env, &clip)) return false;
    /* A setting with clipped noise is just not chosen; only a clearly
     * non-noise envelope means a carrier appeared. */
    *quiet = cal_setting_quiet(env, clip);
    *carrier = cal_setting_carrier(env, clip);
    printf("FILTER_CAL bw=%s code=%d skirt=%u width_khz=%u nbw_khz=%u env_x100=%u limit=%u clip_pm=%d\n",
           f.bw20 ? "BW20" : "BW40", f.code, f.skirt, *width, *nbw, env, quiet_limit(clip), clip);
    return true;
}

/* A channel is quiet when it shows receiver noise at maximum gain: no
 * burst-confirmed sync, noise-like envelope, not dead or railing IQ. */
static const char *noise_check(unsigned *env_out, int *clip_out);
static bool channel_quiet(void)
{
    unsigned env = 0;
    int clip = 0;
    const char *why = video_recent() ? "video_sync" : noise_check(&env, &clip);
    printf("FILTER_CAL channel=%s freq=%u gain=%u quiet=%u reason=%s env_x100=%u clip_pm=%d\n", rf_channel()->name,
           rf_frequency_mhz(), s_current_gain, why == NULL, why ? why : "-", env, clip);
    return why == NULL;
}

/* The filter is the chip's own baseband filter: its noise bandwidth does
 * not depend on the channel. With a VTX on the current channel the sweep
 * runs on the quietest other channel instead and returns afterwards. */
static bool find_quiet_channel(size_t original)
{
    if (channel_quiet()) return true;
    /* Channels within 40 MHz are skipped: a VTX leaks into its neighbours. */
    for (size_t k = 0; k < FPV_CHANNELS; ++k) {
        size_t ch = (original + FPV_CHANNELS / 2 + k) % FPV_CHANNELS;
        if (ch == original) continue;
        if (abs((int)rf_channel_freq(ch) - (int)s_original_mhz) < 40) continue;
        if (rf_set_channel(ch) != ESP_OK) continue;
        receiver_set_gain(rf_gain_table()->max_index);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (channel_quiet()) return true;
    }
    return false;
}

bool filter_calibrate(bool automatic)
{
    if (phy_filter_calibrated_code() < 0) { s_filter_result = "unsupported_phy"; return false; }
    if (!receiver_pause("FILTER_CAL")) { s_filter_result = "busy"; return false; }
    const uint8_t saved_gain = s_current_gain;
    const size_t original = rf_channel_index();
    const int original_offset = rf_offset_khz();
    s_original_mhz = rf_frequency_mhz();
    const rf_filter_t previous = rf_filter(), calibrated = {PHY_FILTER_CALIBRATED, 0, false};
    s_scan_active = true;              /* control loop and AFC stand aside */
    bool ok = rf_set_filter(calibrated);
    receiver_set_gain(rf_gain_table()->max_index);
    vTaskDelay(pdMS_TO_TICKS(50));
    status_screen("CALIBRATING", "LOOKING FOR A QUIET CHANNEL", "ONE TIME ONLY, ABOUT 30 S", -1);
    const char *fail = ok ? NULL : "calibrated_bytes_not_applied";
    if (ok && !(ok = find_quiet_channel(original))) fail = "no_quiet_channel";
    s_cal_own_channel = rf_channel_index() == original;
    s_cal_done = 0;
    s_cal_total = ok ? 2u + 2u * FILTER_CODES * FILTER_SKIRTS : 0u;   /* checks + BW40/BW20 x codes x skirts */
    s_cal_t0 = esp_timer_get_time();
    printf("FILTER_CAL begin mode=%s freq=%u (own %u) target_width_khz=%u\n", automatic ? "auto" : "manual",
           rf_frequency_mhz(), s_original_mhz, FILTER_MIN_KHZ);
    unsigned width = 0, nbw = 0, cal_nbw = 0;
    bool quiet = false, carrier = false;
    if (ok && !(ok = measure_setting(calibrated, &width, &cal_nbw, &quiet, &carrier) && quiet))
        fail = carrier ? "carrier_detected" : "not_quiet";
    rf_filter_t best = calibrated, narrow = calibrated;
    unsigned best_nbw = ok && width >= FILTER_MIN_KHZ ? cal_nbw : 0, best_width = width;
    unsigned narrow_nbw = ok && width >= FILTER_NARROW_KHZ ? cal_nbw : 0, narrow_width = width;
    rf_filter_t wide = calibrated;
    unsigned wide_nbw = ok && width >= FILTER_WIDE_KHZ ? cal_nbw : 0, wide_width = width;
    static const int8_t codes[FILTER_CODES] = {PHY_FILTER_CALIBRATED, 0, 8, 16, 24, 32, 40, 48, 52, 56, 60, 62, 63};
    static const uint8_t skirts[FILTER_SKIRTS] = {0, 8, 16, 24, 32};
    for (unsigned d = 0; ok && d < 2u; ++d)
        for (unsigned c = 0; ok && c < sizeof(codes); ++c)
            for (unsigned k = 0; ok && k < sizeof(skirts); ++k) {
                rf_filter_t f = {codes[c], skirts[k], d == 1u};
                if (!measure_setting(f, &width, &nbw, &quiet, &carrier)) { ok = false; fail = "probe_failed"; break; }
                if (carrier) { ok = false; fail = "carrier_detected"; break; }   /* appeared mid-sweep */
                if (quiet && nbw && width >= FILTER_MIN_KHZ && (!best_nbw || nbw < best_nbw)) {
                    best = f; best_nbw = nbw; best_width = width;
                }
                if (quiet && nbw && width >= FILTER_NARROW_KHZ && (!narrow_nbw || nbw < narrow_nbw)) {
                    narrow = f; narrow_nbw = nbw; narrow_width = width;
                }
                if (quiet && nbw && width >= FILTER_WIDE_KHZ && (!wide_nbw || nbw < wide_nbw)) {
                    wide = f; wide_nbw = nbw; wide_width = width;
                }
            }
    /* Re-check on the calibrated bytes before trusting the sweep. */
    if (ok && !(ok = measure_setting(calibrated, &width, &nbw, &quiet, &carrier) && quiet))
        fail = carrier ? "carrier_detected" : "recheck_not_quiet";
    /* The normal class must be a measured setting: a noise bandwidth of 0
     * means nothing reached the minimum width. */
    if (ok && !best_nbw) { ok = false; fail = "no_wide_setting"; }
    /* Back to the exact original tuning; a failed retune is retried and
     * reported, never assumed. */
    bool restored = rf_channel_index() == original;
    for (unsigned t = 0; !restored && t < 3u; ++t) {
        restored = rf_set_channel(original) == ESP_OK;
        if (!restored) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!restored) printf("FILTER_CAL error: retune to %u MHz failed\n", s_original_mhz);
    if (rf_offset_khz() != original_offset) rf_set_offset_khz(original_offset);
    if (rf_offset_khz() != original_offset) {
        printf("FILTER_CAL error: offset %d kHz not restored (now %d)\n", original_offset, rf_offset_khz());
        restored = false;
    }
    /* Nothing is stored unless the receiver is back on its own tuning: a
     * sweep that ends elsewhere is discarded and tried on the next boot. */
    if (ok && !restored) { ok = false; fail = "restore_failed"; }
    if (ok) {
#define KHZ16(v) ((uint16_t)((v) > 65535u ? 65535u : (v)))
        const filter_blob_t cal = {FILTER_VERSION,
            {1u, best.bw20, best.skirt, best.code, KHZ16(best_width), KHZ16(best_nbw)},
            {narrow_nbw ? 1u : 0u, narrow.bw20, narrow.skirt, narrow.code, KHZ16(narrow_width), KHZ16(narrow_nbw)},
            {wide_nbw ? 1u : 0u, wide.bw20, wide.skirt, wide.code, KHZ16(wide_width), KHZ16(wide_nbw)},
            KHZ16(cal_nbw)};
        if (!rf_set_filter(best)) { ok = false; fail = "best_not_applied"; }
        else if (!blob_store("filter", &cal, sizeof(cal))) { ok = false; fail = "nvs_failed"; }
        else s_filter_cal = cal;
    }
    if (ok) {
        s_filter_result = "stored";
        printf("FILTER_CAL normal bw=%s code=%d skirt=%u width_khz=%u nbw_khz=%u | narrow bw=%s code=%d "
               "skirt=%u width_khz=%u nbw_khz=%u | wide bw=%s code=%d skirt=%u width_khz=%u nbw_khz=%u | "
               "calibrated_nbw_khz=%u\n", best.bw20 ? "BW20" : "BW40",
               best.code, best.skirt, best_width, best_nbw, narrow.bw20 ? "BW20" : "BW40", narrow.code,
               narrow.skirt, narrow_width, narrow_nbw, wide.bw20 ? "BW20" : "BW40", wide.code, wide.skirt,
               wide_width, wide_nbw, cal_nbw);
    } else {
        if (!rf_set_filter(previous)) printf("FILTER_CAL error: previous filter not applied\n");
        s_filter_result = fail ? fail : "sweep_failed";
        printf("FILTER_CAL aborted=%s\n", s_filter_result);
    }
    s_cal_total = 0;
    status_screen(ok ? "FILTER CALIBRATED" : "FILTER NOT CALIBRATED", ok ? "NARROW FILTER STORED" : s_filter_result,
                  ok ? "" : "SAFE DEFAULT, RETRY NEXT BOOT", ok ? 100 : -1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    receiver_set_gain(saved_gain);
    s_scan_active = false;
    receiver_resume();
    return ok;
}

const char *calibration_filter_result(void) { return s_filter_result; }

/* Width class of the active analog filter, for model tables keyed on it
 * (cn_estimate.h): 10 narrow, 12 normal, 16 wide, 20 the chip's own ~20 MHz. */
int calibration_filter_class_mhz(void)
{
    rf_filter_t f, now = rf_filter();
    /* The sweep can select the same setting for normal and narrow (board:
     * both 13.125 MHz). Prefer normal, and do not call a measured >11 MHz
     * narrow entry 10 MHz; the kp ladder uses the same width boundary. */
    if (filter_setting(FILTER_NORMAL, &f) && rf_filter_equal(f, now)) return 12;
    if (filter_setting(FILTER_NARROW, &f) && rf_filter_equal(f, now))
        return s_filter_cal.narrow.width_khz > 11000u ? 12 : 10;
    if (filter_setting(FILTER_WIDE, &f) && rf_filter_equal(f, now)) return 16;
    return 20;
}

unsigned calibration_narrow_width_khz(void)
{
    return s_filter_cal.narrow.valid ? s_filter_cal.narrow.width_khz : 0u;
}

bool filter_setting(int filter_class, rf_filter_t *out)
{
    const filter_entry_t *e = filter_class == FILTER_NARROW ? &s_filter_cal.narrow :
                              filter_class == FILTER_WIDE ? &s_filter_cal.wide : &s_filter_cal.normal;
    if (s_filter_cal.version != FILTER_VERSION || !e->valid) return false;
    if (!e->nbw_khz) return false;      /* never measured: not a usable class */
    *out = (rf_filter_t){e->code, e->skirt, e->bw20 != 0};
    return true;
}

/* Manual request (menu or console): runs the startup phase once more, in
 * this task (the console keeps draining USB). */
static volatile bool s_manual_requested;
void filter_request(void) { s_manual_requested = true; }

/* ---- Per-gain RX DC correction --------------------------------------------
 * Near the range edge the carrier is a few cells; a receiver DC of ~1 cell
 * biases every origin-adjacent phase. The DC DAC pair is searched per gain
 * row in confirmed quiet (no burst-confirmed sync for 3 s, a noise-like
 * envelope for 5 s, never on a channel where a carrier was seen this boot)
 * and held right after every gain write by the post-gain hook (board,
 * 2026-10-07: DC median ~134 LSB without the hook, ~11 with it). */
#define DCO_VERSION      7u   /* 7: FLOOR rows searched flat (phy_dco_search_flat) */
typedef struct {
    int16_t code[2];
    int16_t residual_mcells[2];
    uint8_t valid;           /* DCO_ROW_* */
} dco_entry_t;
/* A row is OK (codes measured) or FLOOR (measured: receiver noise below
 * one quantizer step there, nothing to correct); 0 = still missing. */
#define DCO_ROW_OK    1u
#define DCO_ROW_FLOOR 2u
typedef struct {
    uint16_t freq;
    uint8_t version, lo, hi, band5, iq_scale_sel, filter_bw20;
    int8_t filter_code, filter_skirt;
    dco_entry_t e[ARC_VENDOR_GAIN_MAX + 1u];
} dco_table_t;
/* CPU-only state in LP RAM: internal SRAM is the menu's DMA heap. */
static RTC_FAST_ATTR dco_table_t s_dco;
static RTC_FAST_ATTR int64_t s_dco_found_us[ARC_VENDOR_GAIN_MAX + 1u];
static RTC_FAST_ATTR uint8_t s_env_buf[24u * PROBE_BYTES];
static uint8_t s_dco_banned[ARC_VENDOR_GAIN_MAX + 1u];
static uint8_t s_dco_floor;
static bool s_dco_dirty;
static uint32_t s_dco_searches, s_dco_hook_holds, s_dco_aborts, s_dco_carrier_searches;

static void dco_context(dco_table_t *t, uint16_t freq)
{
    const arc_gain_table_t *table = rf_gain_table();
    rf_filter_t f = rf_filter();
    t->version = DCO_VERSION;
    t->freq = freq;
    t->lo = arc_gain_highest_rf_stage_start(table);
    t->hi = table->max_index;
    t->band5 = table->band5;
    t->iq_scale_sel = phy_param[650];   /* phy_rxiq_scale_set() selector */
    t->filter_code = f.code;
    t->filter_skirt = (int8_t)f.skirt;
    t->filter_bw20 = f.bw20;
}

static bool dco_same_context(const dco_table_t *a, const dco_table_t *b)
{
    return a->version == b->version && a->freq == b->freq && a->lo == b->lo && a->hi == b->hi &&
           a->band5 == b->band5 && a->iq_scale_sel == b->iq_scale_sel;
}

/* The wanted context lives off the task stack: two full tables on it took
 * 1840 bytes (-fstack-usage) before NVS added its own frames. */
static RTC_FAST_ATTR dco_table_t s_dco_want;

/* Per-channel tables (dco_slots.h): slot blobs "dco0".."dco5" plus the
 * index "dco_idx". The single legacy "dco" table is adopted once. */
static dco_index_t s_dco_index;
static bool s_dco_index_loaded;
static uint16_t s_dco_seed_mhz;     /* the current table was seeded from this channel, 0 = own */
static uint32_t s_dco_seeds;
static dco_mask_t s_dco_templ;      /* rows of the current table that come from a template */

static unsigned dco_template_rows(void) { return dco_mask_count(s_dco_templ, s_dco.lo, s_dco.hi); }

/* A row measured on the own channel is no longer a template row. */
static void dco_row_measured(unsigned g) { dco_mask_put(s_dco_templ, g, false); }

static void dco_slot_key(unsigned slot, char key[8]) { snprintf(key, 8, "dco%u", slot); }

static bool dco_load_slot(unsigned slot, dco_table_t *out)
{
    char key[8];
    dco_slot_key(slot, key);
    return blob_load(key, out, sizeof(*out)) && out->version == DCO_VERSION && out->freq == s_dco_index.freq[slot];
}

static bool dco_save(void)
{
    char key[8];
    const unsigned slot = dco_slot_for_store(&s_dco_index, s_dco.freq);
    dco_slot_key(slot, key);
    if (!blob_store(key, &s_dco, sizeof(s_dco))) return false;
    dco_index_touch(&s_dco_index, slot, s_dco.freq);
    memcpy(s_dco_index.templ[slot], s_dco_templ, sizeof(s_dco_templ));
    if (!blob_store("dco_idx", &s_dco_index, sizeof(s_dco_index))) return false;
    s_dco_seed_mhz = 0;
    return true;
}

static void dco_index_load(void)
{
    if (s_dco_index_loaded) return;
    s_dco_index_loaded = true;
    if (blob_load("dco_idx", &s_dco_index, sizeof(s_dco_index)) && s_dco_index.version == DCO_INDEX_VERSION) return;
    dco_index_init(&s_dco_index);
    /* Adopt the single table of earlier builds as the first slot. */
    memset(s_dco_templ, 0, sizeof(s_dco_templ));   /* measured on its own channel */
    if (blob_load("dco", &s_dco, sizeof(s_dco)) && s_dco.version == DCO_VERSION && s_dco.freq) {
        if (dco_save()) printf("DCO adopted the legacy table freq=%u as slot 0\n", s_dco.freq);
    }
}

static void dco_select(void)
{
    memset(&s_dco_want, 0, sizeof(s_dco_want));
    dco_context(&s_dco_want, rf_frequency_mhz());
    if (dco_same_context(&s_dco_want, &s_dco)) return;
    (void)phy_dco_release();
    memset(s_dco_found_us, 0, sizeof(s_dco_found_us));
    memset(s_dco_banned, 0, sizeof(s_dco_banned));
    s_dco_floor = 0;
    s_dco_seed_mhz = 0;
    dco_index_load();
    memset(s_dco_templ, 0, sizeof(s_dco_templ));
    const uint16_t freq = s_dco_want.freq;
    const int own = dco_slot_of(&s_dco_index, freq);
    if (own >= 0 && dco_load_slot((unsigned)own, &s_dco) && dco_same_context(&s_dco, &s_dco_want)) {
        memcpy(s_dco_templ, s_dco_index.templ[own], sizeof(s_dco_templ));
        return;
    }
    /* No own table: start from the nearest measured channel. */
    const int near = dco_nearest_slot(&s_dco_index, freq);
    if (near >= 0 && dco_load_slot((unsigned)near, &s_dco)) {
        const uint16_t from = s_dco.freq;
        s_dco.freq = freq;
        if (dco_same_context(&s_dco, &s_dco_want)) {
            s_dco_seed_mhz = from;
            ++s_dco_seeds;
            unsigned ok = 0;
            for (unsigned g = s_dco.lo; g <= s_dco.hi && g <= ARC_VENDOR_GAIN_MAX; ++g) {
                dco_mask_put(s_dco_templ, g, s_dco.e[g].valid != 0);
                ok += s_dco.e[g].valid != 0;
            }
            printf("DCO template freq=%u from=%u rows=%u (nearest measured channel; replaced by own measurements)\n",
                   freq, from, ok);
            return;
        }
    }
    s_dco = s_dco_want;
}

void dco_post_gain(uint8_t g)
{
    if (receiver_paused() || g < s_dco.lo || g > s_dco.hi || s_dco.e[g].valid != DCO_ROW_OK ||
        s_dco_banned[g] || s_dco.freq != rf_frequency_mhz()) return;
    if (phy_dco_hold(s_dco.e[g].code[0], s_dco.e[g].code[1]) == ESP_OK) ++s_dco_hook_holds;
}

/* Sampling-phase probes run with the gain worker paused, where the post-gain
 * hook stays silent: hold the current gain's stored row explicitly so the
 * scan sees the same DC as live video. */
static void dco_hold_for_scan(void)
{
    dco_select();
    const uint8_t g = s_current_gain;
    if (g < s_dco.lo || g > s_dco.hi || s_dco.e[g].valid != DCO_ROW_OK || s_dco_banned[g] ||
        s_dco.freq != rf_frequency_mhz()) return;
    (void)phy_dco_hold(s_dco.e[g].code[0], s_dco.e[g].code[1]);
}

static bool dco_measure(int dc[2])
{
    window_stats_t w;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!collect(64, &w)) return false;
    dc[0] = w.dc_i;
    dc[1] = w.dc_q;
    return true;
}

/* Receiver noise: flat envelope statistic, and not dead or railing IQ
 * (which also has one; board 2026-10-07). NULL when it is noise, else the
 * reason. */
static const char *noise_check(unsigned *env_out, int *clip_out)
{
    unsigned got = 0;
    for (unsigned tries = 0; tries < 72u && got < 24u; ++tries) {
        vTaskDelay(1);
        if (copy_probe_window(s_env_buf + got * PROBE_BYTES, NULL)) ++got;
    }
    return cal_channel_quiet(s_env_buf, got, false, env_out, clip_out);
}

static bool noise_like(void) { return noise_check(NULL, NULL) == NULL; }

/* A search needs noise wide enough to dither the quantizer: >= 3 cells
 * per axis each with >= 5 % of the samples. A failed probe is not a floor:
 * it used to mark every lower row FLOOR in NVS and stop the DC bar half way. */
enum { NOISE_MEASURABLE, NOISE_BELOW_STEP, NOISE_PROBE_FAILED };
static int noise_measurable(void)
{
    uint8_t buf[PROBE_BYTES];
    unsigned hi[16] = {0}, hq[16] = {0}, n = 0;
    for (unsigned tries = 0; tries < 24u && n < 8u * sizeof(buf); ++tries) {
        vTaskDelay(1);
        if (!copy_probe_window(buf, NULL)) continue;
        for (unsigned k = 0; k < sizeof(buf); ++k) { ++hi[dsp_i(buf[k]) & 15]; ++hq[dsp_q(buf[k]) & 15]; }
        n += sizeof(buf);
    }
    if (n < 4u * sizeof(buf)) return NOISE_PROBE_FAILED;
    unsigned ci = 0, cq = 0;
    for (unsigned c = 0; c < 16u; ++c) { ci += hi[c] * 20u >= n; cq += hq[c] * 20u >= n; }
    return ci >= 3u && cq >= 3u ? NOISE_MEASURABLE : NOISE_BELOW_STEP;
}

/* With a VTX on: a strong FM carrier lies on a circle around the true DC,
 * and the circle centre does not depend on the picture (dsp_circle_dc).
 * The current gain row is searched on such a carrier, but only on the
 * bench: a clean, strong circle and the same gain held for 10 s, never in
 * flight (a search steps the DC for about two seconds). */
#define DCO_CARRIER_RATIO_X100 300u
static bool dco_measure_carrier(int dc[2])
{
    window_stats_t w;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!collect(64, &w) || !w.dc_circle || w.envelope_x100 < DCO_CARRIER_RATIO_X100) return false;
    dc[0] = w.dc_i;
    dc[1] = w.dc_q;
    return true;
}

static bool dco_carrier_search(bool manual)
{
    const uint8_t g = s_current_gain;
    if (!manual && g <= ARC_VENDOR_GAIN_MAX && s_dco.e[g].valid == DCO_ROW_OK) return false;
    if (s_gain.state != DG_HOLD || g < s_dco.lo || g > s_dco.hi || s_obs_coherence < 80 || s_obs_clip_pm) return false;
    window_stats_t w;
    if (!collect(16, &w) || !w.dc_circle || w.envelope_x100 < DCO_CARRIER_RATIO_X100) return false;
    if (!receiver_pause("DCO_CARRIER")) return false;
    (void)phy_dco_release();
    phy_dco_result_t res;
    (void)phy_dco_search(dco_measure_carrier, &res);
    if (res.measured) {
        s_dco.e[g] = (dco_entry_t){{(int16_t)res.codes[0], (int16_t)res.codes[1]},
            {(int16_t)res.residual_mcells[0], (int16_t)res.residual_mcells[1]}, DCO_ROW_OK};
        dco_row_measured(g);
        if (dco_save()) s_dco_dirty = false;
    }
    s_dco_found_us[g] = esp_timer_get_time();
    ++s_dco_searches;
    ++s_dco_carrier_searches;
    receiver_resume();
    return res.measured;
}

/* Noise below one quantizer step: the IQ is a fixed code, so a short
 * window is an exact DC measurement (no averaging needed). */
static bool dco_measure_flat(int dc[2])
{
    window_stats_t w;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!collect(8, &w)) return false;
    dc[0] = w.dc_i;
    dc[1] = w.dc_q;
    return true;
}

/* Row g with the receiver noise below one step: codes from the flat search.
 * Before, such a row and all below it were FLOOR (left at the chip's DC). */
static bool dco_flat_row(uint8_t g)
{
    phy_dco_result_t res;
    (void)phy_dco_search_flat(dco_measure_flat, &res);
    if (!res.measured) return false;
    s_dco.e[g] = (dco_entry_t){{(int16_t)res.codes[0], (int16_t)res.codes[1]},
        {(int16_t)res.residual_mcells[0], (int16_t)res.residual_mcells[1]}, DCO_ROW_OK};
    dco_row_measured(g);
    s_dco_found_us[g] = esp_timer_get_time();
    ++s_dco_searches;
    return true;
}

/* One quiet search of gain row g (receiver noise). NOISE_BELOW_STEP when
 * the noise is below one quantizer step there (no codes at or below g),
 * NOISE_PROBE_FAILED when nothing could be measured (rows untouched). */
static int dco_quiet_search(uint8_t g)
{
    if (!receiver_pause("DCO")) return NOISE_PROBE_FAILED;
    const uint8_t restore = s_current_gain;
    (void)phy_dco_release();
    receiver_set_gain(g);
    vTaskDelay(pdMS_TO_TICKS(20));
    int measurable = noise_measurable();
    if (measurable == NOISE_PROBE_FAILED) {
        printf("DCO row=%u probe_failed (rows kept for the next boot)\n", g);
    } else if (measurable == NOISE_BELOW_STEP && dco_flat_row(g)) {
        measurable = NOISE_MEASURABLE;          /* this row has codes now; go on downward */
    } else if (measurable == NOISE_BELOW_STEP) {
        /* Less gain, even less noise, and no flip found: the rows below
         * cannot be searched either. Codes measured on a carrier stay. */
        for (int k = g; k >= (int)s_dco.lo; --k) {
            if (s_dco.e[k].valid != DCO_ROW_OK) s_dco.e[k].valid = DCO_ROW_FLOOR;
            dco_row_measured((unsigned)k);   /* measured: nothing to correct here */
        }
        s_dco_floor = (uint8_t)(g + 1);
    } else {
        phy_dco_result_t res;
        (void)phy_dco_search(dco_measure, &res);
        if (res.measured) {
            s_dco.e[g] = (dco_entry_t){{(int16_t)res.codes[0], (int16_t)res.codes[1]},
                {(int16_t)res.residual_mcells[0], (int16_t)res.residual_mcells[1]}, DCO_ROW_OK};
            dco_row_measured(g);
        }
        s_dco_found_us[g] = esp_timer_get_time();
        ++s_dco_searches;
    }
    if (measurable != NOISE_PROBE_FAILED) s_dco_dirty = true;
    receiver_set_gain(restore);
    receiver_resume();
    return measurable;
}

static unsigned dco_rows(uint8_t state)
{
    unsigned n = 0;
    for (unsigned g = s_dco.lo; g <= s_dco.hi && g <= ARC_VENDOR_GAIN_MAX; ++g) n += s_dco.e[g].valid == state;
    return n;
}
static unsigned dco_valid_rows(void) { return dco_rows(DCO_ROW_OK); }

bool calibration_dc_ok(uint8_t g)
{
    if (phy_filter_calibrated_code() < 0) return true;     /* unpinned PHY: nothing to hold */
    if (s_dco.freq != rf_frequency_mhz() || g > ARC_VENDOR_GAIN_MAX) return false;
    if (g < s_dco.lo || g > s_dco.hi) return true;
    if (s_dco.e[g].valid == DCO_ROW_OK) return !s_dco_banned[g] && phy_dco_held();
    return s_dco.e[g].valid == DCO_ROW_FLOOR || g < s_dco_floor;
}

/* The quiet-noise row search, from the top row down; shared by the own
 * channel (VTX off) and a quiet channel (VTX on). */
/* refine: also measure rows that still come from a template (own channel
 * quiet at boot), replacing them by this channel's own codes. */
static const char *dco_quiet_rows(bool manual, bool refine, int64_t deadline, const char *line1, unsigned *done_out)
{
    const int64_t t0 = esp_timer_get_time();
    const unsigned rows = manual ? (unsigned)(s_dco.hi - s_dco.lo + 1u) :
                          dco_rows(0) + (refine ? dco_template_rows() : 0u);
    unsigned done = 0;
    const char *why = "done";
    for (int g = s_dco.hi; g >= (int)s_dco.lo; --g) {
        if (!manual && s_dco.e[g].valid && !(refine && dco_mask_get(s_dco_templ, (unsigned)g)))
            continue;                                             /* own codes or floor */
        char line2[48];
        unsigned left = done ? (unsigned)((esp_timer_get_time() - t0) / done * (rows - done) / 1000000) + 1u : rows * 2u;
        snprintf(line2, sizeof(line2), "ABOUT %u S LEFT, THEN LIVE VIDEO", left);
        status_screen("CALIBRATING DC", line1, line2, rows ? (int)(done * 100u / rows) : 0);
        if (esp_timer_get_time() > deadline) { why = "timeout"; break; }
        if (!noise_like()) { why = "carrier_detected"; break; }   /* VTX switched on / leaking */
        const int r = dco_quiet_search((uint8_t)g);
        if (r == NOISE_PROBE_FAILED) { why = "probe_failed"; break; }
        if (r == NOISE_BELOW_STEP) { why = "floor"; break; }     /* the rest needs no correction */
        ++done;
    }
    *done_out = done;
    return why;
}

/* With the VTX on, the own channel never shows plain receiver noise, so the
 * high gain rows (the ones used far out) stayed without DC codes: board
 * DC without correction is ~1.1 cell (C5VRX-4 video_dco.c), and in the
 * stress matrix a 1-cell DC at 4 dB C/N broke the goggle line lock
 * (picture 0.25-0.97: lines shifting in bands). Like the filter sweep, the
 * rows are measured on a quiet channel >= 40 MHz away and the exact tuning
 * is restored. The DC DAC offsets are baseband offsets per gain row, so the
 * codes are stored for the own channel; the live drift nudges trim the
 * remaining difference. Startup only, behind the status screen. */
static const char *dco_remote_search(bool manual, int64_t deadline, unsigned *done)
{
    *done = 0;
    const size_t original = rf_channel_index();
    const int original_offset = rf_offset_khz();
    const uint8_t saved_gain = s_current_gain;
    s_original_mhz = rf_frequency_mhz();
    if (!receiver_pause("DCO_REMOTE")) return "busy";
    s_scan_active = true;                       /* control loop and AFC stand aside */
    receiver_set_gain(rf_gain_table()->max_index);
    vTaskDelay(pdMS_TO_TICKS(50));
    status_screen("CALIBRATING DC", "LOOKING FOR A QUIET CHANNEL", "VTX MAY STAY ON", -1);
    const bool quiet = find_quiet_channel(original) && rf_channel_index() != original;
    receiver_resume();
    char line1[48];
    snprintf(line1, sizeof(line1), "ON QUIET CHANNEL %s (VTX MAY STAY ON)", rf_channel()->name);
    const char *why = quiet ? dco_quiet_rows(manual, false, deadline, line1, done) : "no_quiet_channel";
    /* Back to the exact original tuning, checked like the filter sweep. */
    (void)receiver_pause("DCO_REMOTE");
    bool restored = rf_channel_index() == original;
    for (unsigned t = 0; !restored && t < 3u; ++t) {
        restored = rf_set_channel(original) == ESP_OK;
        if (!restored) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (rf_offset_khz() != original_offset) rf_set_offset_khz(original_offset);
    if (!restored || rf_offset_khz() != original_offset) {
        printf("DCO error: own channel %u MHz / offset %d kHz not restored\n", s_original_mhz, original_offset);
        why = "restore_failed";
    }
    receiver_set_gain(saved_gain);
    s_scan_active = false;
    receiver_resume();
    return why;
}

/* Startup: only the rows of the channel that are still missing (all rows
 * on a manual request). Rows that have codes or a measured noise floor are
 * kept, so a boot after a complete calibration does nothing here. */
static void dco_startup(bool manual, int64_t deadline)
{
    dco_select();
    const unsigned missing = dco_rows(0), templ = dco_template_rows();
    const bool carrier = video_recent();
    /* Template rows (from another channel) are used as they are; they are
     * replaced only when the own channel can be measured without a detour:
     * VTX off at boot. With the VTX on there is no quiet-channel trip. */
    const bool refine = !manual && !missing && templ && !carrier && noise_like();
    if (!manual && !missing && !refine) {
        printf("DCO startup skip=complete rows=%u floor_rows=%u template_rows=%u\n", dco_valid_rows(),
               dco_rows(DCO_ROW_FLOOR), templ);
        return;
    }
    unsigned done = 0;
    const char *why;
    const char *where = refine ? "own_refine" : "own";
    if (!carrier && noise_like()) {
        why = dco_quiet_rows(manual, refine, deadline, refine ? "OWN CHANNEL, VTX OFF" : "KEEP THE VTX OFF", &done);
    } else {
        /* A carrier on the own channel: measure on a quiet channel; only if
         * there is none, the current row on the carrier's circle. */
        where = "quiet_channel";
        why = dco_remote_search(manual, deadline, &done);
        if (!strcmp(why, "no_quiet_channel") || !strcmp(why, "busy")) {
            bool ok = carrier && dco_carrier_search(manual);
            printf("DCO startup carrier=%u result=%s gain=%u missing_rows=%u\n", carrier, ok ? "stored" : "skip",
                   s_current_gain, missing);
            where = "carrier";
        }
    }
    /* The noise floor ends the search before the last row: that is the
     * normal end, not a stall, so the bar completes and says so. */
    if (!strcmp(why, "floor") || !strcmp(why, "done"))
        status_screen("DC CALIBRATED", "LOWER GAINS NEED NO DC CORRECTION", "", 100);
    else
        status_screen("DC NOT COMPLETE", why, "SAFE DEFAULT, RETRY NEXT BOOT", -1);
    vTaskDelay(pdMS_TO_TICKS(800));
    if (s_dco_dirty && dco_save()) s_dco_dirty = false;
    printf("DCO startup where=%s result=%s searched=%u rows=%u floor_rows=%u missing=%u floor_from_gain=%u\n", where,
           why, done, dco_valid_rows(), dco_rows(DCO_ROW_FLOOR), dco_rows(0), s_dco_floor);
}

/* Live: hold the stored codes after gain writes and guard them; never a
 * search (a search steps the DC for seconds). */
static void dco_service(void)
{
    if (!quiet_owner()) return;
    dco_select();
    if (s_dco_dirty && s_gain.state == DG_HOLD && dco_save()) s_dco_dirty = false;
    /* Hold guard: held codes must never coincide with dead or railing IQ. */
    static unsigned bad_ticks;
    const uint8_t g = s_current_gain;
    if (phy_dco_held()) {
        bad_ticks = (s_obs_origin_pm >= 900 || s_obs_clip_pm >= 500) ? bad_ticks + 1u : 0u;
        if (bad_ticks >= 3u) {
            (void)phy_dco_release();
            s_dco_banned[g] = 1u;
            bad_ticks = 0;
            ++s_dco_aborts;
            printf("DCO hold abort gain=%u\n", g);
        }
    } else if (s_gain.state != DG_SETTLE) {
        dco_post_gain(g);
    }
}

/* Thermal drift of the held DC during flight: nudge the held codes one
 * step when the measured centre stays > 0.3 cell off for 5 s (at most four
 * steps per gain per boot). Only a carrier's circle centre or receiver
 * noise is a DC measurement. */
static int s_drift_offset[ARC_VENDOR_GAIN_MAX + 1u][2];
static int s_drift_avg[2];
static uint32_t s_drift_nudges;
static float s_temp_c = -100.0f;

static void drift_service(void)
{
    static temperature_sensor_handle_t tsens;
    static int32_t sum[2];
    static unsigned n, over_s, ticks;
    static int64_t last_nudge;
    if (!tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &tsens) == ESP_OK) (void)temperature_sensor_enable(tsens);
        else tsens = (temperature_sensor_handle_t)(uintptr_t)1u;
    }
    if ((uintptr_t)tsens > 1u && (ticks++ & 3u) == 0u) (void)temperature_sensor_get_celsius(tsens, &s_temp_c);
    const uint8_t g = s_current_gain;
    if (!quiet_owner() || s_gain.state != DG_HOLD || !phy_dco_held() || g > ARC_VENDOR_GAIN_MAX ||
        s_dco.e[g].valid != DCO_ROW_OK) { n = 0; sum[0] = sum[1] = 0; over_s = 0; return; }
    window_stats_t w;
    if (!collect(8, &w) || s_current_gain != g || s_gain.state != DG_HOLD ||
        (!w.dc_circle && w.envelope_x100 > 130u)) { n = 0; sum[0] = sum[1] = 0; return; }
    sum[0] += w.dc_i;
    sum[1] += w.dc_q;
    if (++n < 4u) return;
    s_drift_avg[0] = sum[0] / (int32_t)n;
    s_drift_avg[1] = sum[1] / (int32_t)n;
    n = 0; sum[0] = sum[1] = 0;
    bool over = abs(s_drift_avg[0]) > 300 || abs(s_drift_avg[1]) > 300;
    over_s = over ? over_s + 1u : 0u;
    int64_t now = esp_timer_get_time();
    if (over_s < 5u || now - last_nudge < 2000000LL) return;
    int di = s_drift_avg[0] > 300 ? -1 : s_drift_avg[0] < -300 ? 1 : 0;
    int dq = s_drift_avg[1] > 300 ? -1 : s_drift_avg[1] < -300 ? 1 : 0;
    if (abs(s_drift_offset[g][0] + di) > 4) di = 0;
    if (abs(s_drift_offset[g][1] + dq) > 4) dq = 0;
    int codes[2];
    if ((!di && !dq) || !phy_dco_nudge(di, dq, codes)) return;
    s_drift_offset[g][0] += di;
    s_drift_offset[g][1] += dq;
    s_dco.e[g].code[0] = (int16_t)codes[0];
    s_dco.e[g].code[1] = (int16_t)codes[1];
    s_dco_dirty = true;
    last_nudge = now;
    over_s = 0;
    ++s_drift_nudges;
}

/* Persist the measured gain map: at most every 30 s, while holding, only
 * when it changed. Short flights end with the battery pulled, so 2 minutes
 * lost most of a flight's learning. Flash wear: a ~540-byte blob in the
 * 24 KiB NVS partition, even written every 30 s, is > 30,000 flight hours
 * of page erase cycles; unchanged maps are never written. */
static void gain_map_service(void)
{
    static int64_t last;
    static uint32_t last_learned;
    int64_t now = esp_timer_get_time();
    if (now - last < 30000000LL || s_gain.state != DG_HOLD || s_gain.learned == last_learned) return;
    last = now;
    last_learned = s_gain.learned;
    dg_map_blob_t blob;
    if (!dg_export_map(&s_gain, &blob)) return;
    blob.freq_mhz = rf_frequency_mhz();
    blob.lane_mode = 2u;
    if (s_gain_map_saved_valid && !memcmp(&blob, &s_gain_map_saved, sizeof(blob))) return;
    if (blob_store("gain_map", &blob, sizeof(blob))) { s_gain_map_saved = blob; s_gain_map_saved_valid = true; }
}

/* VTX deviation for the receive profile (profile.c): measured fresh on
 * this boot's carrier while the DEFAULT picture keeps running (a stored
 * deviation is not a VTX identity); only the switch to a matched profile
 * happens behind a status screen. With no VTX yet (calibrated with the VTX
 * off) it waits up to 20 s for one to appear; later the measurement goes
 * on passively and a result is kept as pending for the next boot. */
#define DEVIATION_WAIT_US    20000000LL
#define DEVIATION_MEASURE_US 15000000LL
static void deviation_startup(int64_t deadline)
{
    rf_filter_t matched;
    if (!filter_setting(FILTER_NARROW, &matched) && !filter_setting(FILTER_WIDE, &matched)) {
        profile_set_reason("no_matched_filter");
        printf("PROFILE startup skip=no_matched_filter\n");
        return;
    }
    const int64_t wait_until = esp_timer_get_time() + DEVIATION_WAIT_US;
    while (!video_recent() && esp_timer_get_time() < wait_until && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(200));
    if (!video_recent()) {
        profile_set_reason("no_video_at_startup");
        printf("PROFILE startup skip=no_video (measured passively later, pending for the next boot)\n");
        return;
    }
    profile_measure_begin();
    const int64_t until = esp_timer_get_time() + DEVIATION_MEASURE_US;
    while (profile_measure_result() < 0 && esp_timer_get_time() < until && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(100));
    profile_measure_end();
    int dev = profile_measure_result();
    if (dev < 0) {
        profile_set_reason("inconclusive_at_startup");
        printf("PROFILE startup result=inconclusive profile=%s\n", profile_name(profile_active()));
        return;
    }
    profile_commit(dev);
}

#define STARTUP_BUDGET_US 120000000LL
static void startup_phase(bool manual)
{
    s_startup = true;
    const int64_t t0 = esp_timer_get_time(), deadline = t0 + STARTUP_BUDGET_US;
    /* Direct Gain first finds its state (table maximum without a carrier). */
    for (unsigned k = 0; k < 40u && s_gain.state == DG_SETTLE; ++k) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(manual ? 200 : 1500));
    for (unsigned k = 0; k < 100u && !startup_owner(); ++k) vTaskDelay(pdMS_TO_TICKS(100));
    if (!startup_owner()) {
        printf("STARTUP skip=owner_busy menu=%u scan=%u paused=%u\n", s_menu_active, s_scan_active, receiver_paused());
    } else {
        printf("STARTUP begin manual=%u filter=%s dco_rows=%u deviation_x100=%d\n", manual,
               s_filter_cal.normal.valid ? "stored" : "missing", dco_valid_rows(),
               profile_deviation_x100(rf_channel_index()));
        /* A manual run re-measures the filter and the VTX: leave a matched
         * profile first, so filter and tracker never disagree. */
        if (manual && profile_active() != PROFILE_DEFAULT) {
            status_screen("CALIBRATING", "BACK TO THE DEFAULT PROFILE", "", -1);
            if (!profile_set(PROFILE_DEFAULT)) printf("STARTUP error: DEFAULT profile not applied\n");
        }
        printf("STARTUP step=sphase\n");
        sphase_qualify("startup", true);
        printf("STARTUP step=filter\n");
        if (phy_filter_calibrated_code() < 0) { s_filter_result = "unsupported_phy"; }
        else if (manual || s_filter_cal.version != FILTER_VERSION || !s_filter_cal.normal.valid) {
            (void)filter_calibrate(!manual);
        }
        printf("STARTUP step=dco\n");
        if (esp_timer_get_time() < deadline) dco_startup(manual, deadline);
        printf("STARTUP step=deviation\n");
        /* Video needs a moment back on the own channel before measuring. */
        status_screen_end();
        vTaskDelay(pdMS_TO_TICKS(1500));
        /* Final qualification in the live TX/RX context, before the VTX
         * deviation measurement that needs the picture. */
        printf("STARTUP step=sphase_final\n");
        if (sphase_on_carrier()) sphase_carrier_scan("final", false);   /* choose on the carrier */
        else sphase_qualify("final", true);
        if (esp_timer_get_time() < deadline) deviation_startup(deadline);
        else profile_set_reason("startup_budget");
    }
    status_screen_end();
    s_startup = false;
    profile_live_begin();   /* passive from here on: never switches */
    printf("STARTUP done ms=%lld filter=%s dco_rows=%u dco_missing=%u sphase=%s profile=%s reason=%s\n",
           (esp_timer_get_time() - t0) / 1000, s_filter_result, dco_valid_rows(), dco_rows(0), s_sphase_state,
           profile_name(profile_active()), profile_reason());
}

/* ---- Bench-only IQ lane comparison (console L) ---------------------------
 * Production keeps the ultrafine lane {9,6,5,4}. This compares it with fine
 * {9,7,6,5} and coarse {9,8,7,6} on the same RF input, gain, filter and
 * tuning, in ABBA order (U F C C F U) so a slow fade does not favour one
 * lane. Raw-IQ statistics only: U85Q's LUTs are made for ultrafine, so
 * this is no demodulation, sensitivity or range result. Only on an
 * explicit request; the status screen owns TX for the ~3 s it takes and
 * the ultrafine lane is always routed back. */
#define LANE_WINDOWS 24u
static volatile bool s_lane_requested;
void lane_compare_request(void) { s_lane_requested = true; }

typedef struct { unsigned windows; long p_median, q_phase, origin_pm, clip_pm, env_x100; } lane_stats_t;

static void lane_measure(lane_stats_t *st)
{
    uint8_t sample[PROBE_BYTES];
    vTaskDelay(pdMS_TO_TICKS(20));      /* the ring refills within ~1 ms */
    for (unsigned tries = 0, got = 0; tries < LANE_WINDOWS * 3u && got < LANE_WINDOWS; ++tries) {
        vTaskDelay(1);
        if (!copy_probe_window(sample, NULL)) continue;
        metrics_t m = measure_window(sample, sizeof(sample));
        st->p_median += m.p_median;
        st->q_phase += m.q_phase;
        st->origin_pm += m.origin_pm;
        st->clip_pm += m.clip_pm;
        st->env_x100 += dsp_envelope_ratio_x100(sample, sizeof(sample));
        ++st->windows;
        ++got;
    }
}

static void lane_compare(void)
{
    if (!quiet_owner() || !receiver_pause("LANE_CMP")) { printf("LANE_CMP refused=busy\n"); return; }
    s_scan_active = true;
    status_screen("BENCH LANE TEST", "VIDEO STOPS FOR ABOUT 3 S", "SAME GAIN, FILTER AND CHANNEL", -1);
    static const uint8_t order[] = {RF_LANE_ULTRAFINE, RF_LANE_FINE, RF_LANE_COARSE,
                                    RF_LANE_COARSE, RF_LANE_FINE, RF_LANE_ULTRAFINE};
    lane_stats_t st[RF_LANES];
    memset(st, 0, sizeof(st));
    bool routed = true;
    for (unsigned k = 0; k < sizeof(order) && routed; ++k) {
        routed = rf_set_lane(order[k]) == ESP_OK;
        if (routed) lane_measure(&st[order[k]]);
    }
    esp_err_t back = rf_set_lane(RF_LANE_ULTRAFINE);
    for (unsigned t = 0; back != ESP_OK && t < 3u; ++t) { vTaskDelay(pdMS_TO_TICKS(10)); back = rf_set_lane(RF_LANE_ULTRAFINE); }
    printf("LANE_CMP freq=%u gain=%u filter_code=%d skirt=%u routed=%u restored=%s (raw IQ only; U85Q is made for "
           "ultrafine, not a range result)\n", rf_frequency_mhz(), s_current_gain, rf_filter().code, rf_filter().skirt,
           routed, back == ESP_OK && rf_lane() == RF_LANE_ULTRAFINE ? "ultrafine" : "FAILED");
    for (unsigned l = 0; l < RF_LANES; ++l) {
        const long n = st[l].windows ? (long)st[l].windows : 1;
        printf("LANE_CMP lane=%s windows=%u p_median=%ld q_phase=%ld origin_pm=%ld rail_pm=%ld env_x100=%ld\n",
               rf_lane_name(l), st[l].windows, st[l].p_median / n, st[l].q_phase / n, st[l].origin_pm / n,
               st[l].clip_pm / n, st[l].env_x100 / n);
    }
    printf("LANE_CMP note: origin_pm high = signal below the lane's step; rail_pm high = folding risk; compare "
           "q_phase at equal RF input over repeated runs before changing the production lane\n");
    status_screen_end();
    s_scan_active = false;
    receiver_resume();
}

static void calibration_task(void *arg)
{
    (void)arg;
    rf_set_post_gain_hook(dco_post_gain);
    printf("STARTUP task up\n");
    startup_phase(false);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_manual_requested && !s_menu_active) { s_manual_requested = false; startup_phase(true); continue; }
        if (s_lane_requested && !s_menu_active) { s_lane_requested = false; lane_compare(); continue; }
        dco_service();
        drift_service();
        gain_map_service();
        sphase_monitor();
    }
}

void calibration_start(void)
{
    rf_filter_t loaded;
    if (blob_load("filter", &s_filter_cal, sizeof(s_filter_cal)) && filter_setting(FILTER_NORMAL, &loaded)) {
        rf_set_filter(loaded);
        s_filter_result = "loaded";
    } else memset(&s_filter_cal, 0, sizeof(s_filter_cal));
    ESP_ERROR_CHECK(xTaskCreate(calibration_task, "calibration", 8192, NULL, 1, NULL) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
}

void calibration_print(void)
{
    rf_filter_t f = rf_filter();
    const unsigned valid = dco_valid_rows();
    printf("FILTER %s active bw=%s code=%d skirt=%u | normal width_khz=%u nbw_khz=%u | narrow valid=%u "
           "width_khz=%u nbw_khz=%u | wide valid=%u width_khz=%u nbw_khz=%u | calibrated_nbw_khz=%u\n",
           s_filter_result, f.bw20 ? "BW20" : "BW40", f.code, f.skirt, s_filter_cal.normal.width_khz,
           s_filter_cal.normal.nbw_khz, s_filter_cal.narrow.valid, s_filter_cal.narrow.width_khz,
           s_filter_cal.narrow.nbw_khz, s_filter_cal.wide.valid, s_filter_cal.wide.width_khz,
           s_filter_cal.wide.nbw_khz, s_filter_cal.calibrated_nbw_khz);
    printf("DCO held=%u gains_valid=%u floor_rows=%u missing_rows=%u floor=%u searches=%lu carrier_searches=%lu hook_holds=%lu aborts=%lu "
           "drift_avg_mcells=%d/%d nudges=%lu temp_c=%.1f\n", phy_dco_held(), valid, dco_rows(DCO_ROW_FLOOR), dco_rows(0),
           s_dco_floor,
           (unsigned long)s_dco_searches, (unsigned long)s_dco_carrier_searches, (unsigned long)s_dco_hook_holds,
           (unsigned long)s_dco_aborts, s_drift_avg[0], s_drift_avg[1], (unsigned long)s_drift_nudges,
           (double)s_temp_c);
    unsigned stored = 0;
    for (unsigned k = 0; k < DCO_SLOTS; ++k) stored += s_dco_index.freq[k] != 0;
    printf("DCO tables freq=%u template_rows=%u seeded_from=%u seeds=%lu stored_channels=%u/%u:", s_dco.freq,
           dco_template_rows(), s_dco_seed_mhz, (unsigned long)s_dco_seeds, stored, DCO_SLOTS);
    for (unsigned k = 0; k < DCO_SLOTS; ++k) if (s_dco_index.freq[k]) printf(" %u", s_dco_index.freq[k]);
    printf("\n");
    printf("SPHASE state=%s excess_mc2=%d limit=%d white_pm=%d dc_mcell=%d/%d scans=%u "
           "best=%d worst=%d contrast=%u rx_div=%lu carrier_qualified=%u live_limit=%d carrier_checks=%u "
           "carrier_scans=%u moved=%u false_alarms=%u strikes=%u\n", s_sphase_state,
           s_sphase_excess, SPHASE_LIMIT, s_sphase_white, s_sphase_dc[0], s_sphase_dc[1], s_sphase_scans,
           s_sphase_last.valid ? s_sphase_last.best : -1, s_sphase_last.valid ? s_sphase_last.worst : -1,
           sphase_contrast(&s_sphase_last), (unsigned long)PCR.parl_clk_rx_conf.parl_clk_rx_div_num + 1ul,
           s_sphase_live.qualified, SPHASE_LIVE_LIMIT, s_sphase_live.carrier_checks, s_sphase_live.scans,
           s_sphase_live.moved, s_sphase_live.false_alarms, s_sphase_live.strikes);
}
