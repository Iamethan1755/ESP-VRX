/* C5VRX-5 by Twotoz and the C5VRX contributors: receive profile per channel.
 *
 * The VTX deviation sets the best filter width and tracker loop gain
 * (research, simulation, usable-picture threshold):
 *   deviation 0.69: 10 MHz + kp 0.5, 3.2 dB better than 14 MHz + kp 0.85;
 *   deviation 1.0:  10 MHz + kp 0.6, 1.7 dB better;
 *   deviation 1.4:  12 MHz + kp 0.85 (the default) is already best;
 *   deviation 1.7:  16 MHz + kp 0.85, 0.4 dB better and +2 dB SINAD at 8 dB;
 *   deviation 2.0:  16 MHz + kp 0.85, 2.8 dB better (data/wide.txt).
 * The narrow profiles hurt a wide VTX badly, so a profile is only taken
 * from a clear measurement, kept per channel in NVS. At startup it is
 * applied behind a status screen; in flight only by the optional live
 * switch below (asymmetric, guarded restart). Anything uncertain keeps
 * DEFAULT.
 *
 * A stored deviation is not a VTX identity: another VTX on the same
 * channel can be much wider. So every boot and every channel change starts
 * in DEFAULT, and a matched profile needs a fresh measurement in the same
 * channel session, taken in the bounded startup phase (or a manual
 * CALIBRATE) while the DEFAULT picture keeps running; only then the switch
 * happens behind a status screen. After startup the measurement continues
 * passively on live video (no retune, no filter or tracker change): a new
 * cycle every 30 s (continuous VTX matching, profile_logic.h). Each result
 * updates the session deviation (the kp ladder's class) and is kept as
 * PENDING for the next boot or CALIBRATE; agreement and mismatch streaks are
 * reported. With the live switch on (NVS prof_live, default, console A) a
 * reliable result changes the active profile in flight: widening after one
 * decision, narrowing after two agreeing decisions (profile_logic.h), via
 * a filter relock plus the guarded, verified kp restart (a few ms). A
 * profile sets the filter and the kp level it starts from; with AUTO the
 * kp/LP ladder takes over from there on every profile (control.c).
 * In report-only mode a VTX that reads wide while a matched profile is
 * active is reported loudly and DEFAULT returns at the next boot, channel
 * change or CALIBRATE.
 *
 * Deviation: the AFC measures the sync tip and the burst-free porch of
 * confirmed lines (burst or line timing); their distance is deviation x 1.914 MHz (40 of 140
 * IRE over 6.7 MHz). A decision needs three batches of 32 estimates, each
 * with a spread <= 8 % and all medians within 5 % of each other, on a
 * centred (AFC locked), coherent carrier in the healthy gain band, at a
 * C/N estimate >= 8 dB (a weak carrier reads the deviation narrow). The
 * estimator and thresholds are pure (profile_logic.h) and host-tested. */
#include "receiver.h"
#include "profile_logic.h"
#include "phy.h"
#include "cn_estimate.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const s_names[PROFILES] = {"DEFAULT 12MHZ KP0.85", "MATCHED 10MHZ KP0.6", "MATCHED 10MHZ KP0.5",
                                              "WIDE 16MHZ KP0.85"};
static uint16_t s_dev_table[FPV_CHANNELS];    /* last deviation x100 per channel, provisional */
static volatile int s_profile = PROFILE_DEFAULT;
static const char *volatile s_reason = "boot_default";

/* Measurement state, written by the control task through profile_observe().
 * STARTUP may switch behind a status screen; LIVE only records. */
enum { MEASURE_OFF, MEASURE_STARTUP, MEASURE_LIVE };
static volatile int s_measuring = MEASURE_OFF;
static dev_estimator_t s_dev;
static volatile int s_live_dev = -1;          /* this channel session, -1 = none yet */
static volatile size_t s_live_channel = (size_t)-1;
static volatile uint32_t s_observed, s_gated, s_gated_cn;
static dev_monitor_t s_mon;                   /* continuous matching (live only) */
static bool s_live_switch = true;             /* NVS prof_live: in-flight switch allowed */
static int s_switch_pending = -1;             /* profile a live switch waits for, -1 none */
static uint32_t s_live_switches, s_live_switch_fail;

const char *profile_name(int p) { return p >= 0 && p < PROFILES ? s_names[p] : "?"; }
int profile_active(void) { return s_profile; }
const char *profile_reason(void) { return s_reason; }
int profile_deviation_x100(size_t channel) { return channel < FPV_CHANNELS ? s_dev_table[channel] : 0; }
int profile_live_deviation_x100(void) { return s_live_channel == rf_channel_index() ? s_live_dev : -1; }

int profile_for_deviation(int dev_x100)
{
    rf_filter_t f;
    return profile_class(dev_x100, filter_setting(FILTER_NARROW, &f), filter_setting(FILTER_WIDE, &f));
}

/* kp ladder level (kp_tables.h) a profile starts from. */
int profile_base_level(int p)
{
    return profile_base_level_for(p);
}

/* Filter class and start kp level of a profile. Only while TX is not live: before
 * transport_start or with the menu / a status screen owning TX. A filter
 * that does not read back leaves DEFAULT, never a half-applied profile. */
bool profile_set(int p)
{
    if (p < 0 || p >= PROFILES) return false;
    rf_filter_t f;
    const int cls = p == PROFILE_DEFAULT ? FILTER_NORMAL : p == PROFILE_WIDE ? FILTER_WIDE : FILTER_NARROW;
    if (!filter_setting(cls, &f)) {
        if (p != PROFILE_DEFAULT) return false;
        f = rf_filter();            /* uncalibrated: keep what is set */
    }
    if (!transport_set_base_level(profile_base_level(p))) return false;   /* refuses while TX is live */
    if ((!rf_filter_equal(rf_filter(), f) || !rf_filter_applied()) && !rf_set_filter(f)) {
        printf("PROFILE error: filter code=%d skirt=%u not applied (read back code=%d skirt=%d)\n", f.code, f.skirt,
               phy_filter_code(), phy_filter_skirt());
        if (p != PROFILE_DEFAULT) {
            rf_filter_t d;
            if (filter_setting(FILTER_NORMAL, &d)) (void)rf_set_filter(d);
            (void)transport_set_base_level(profile_base_level(PROFILE_DEFAULT));
            s_profile = PROFILE_DEFAULT;
            return false;
        }
    }
    s_profile = p;
    return true;
}

bool profile_live_mode(void) { return s_live_switch; }
void profile_set_live_mode(bool on)
{
    uint8_t v = on;
    s_live_switch = on;
    if (!on) s_switch_pending = -1;
    (void)blob_store("prof_live", &v, 1);
}

void profile_load(void)
{
    uint8_t live;
    if (blob_load("prof_live", &live, 1) && live <= 1u) s_live_switch = live != 0;
    if (!blob_load("vtxdev", s_dev_table, sizeof(s_dev_table))) memset(s_dev_table, 0, sizeof(s_dev_table));
}

/* Boot, before live TX: always DEFAULT; the stored deviation is only a hint
 * until this boot measures the VTX again. */
void profile_boot(void)
{
    if (!profile_set(PROFILE_DEFAULT)) printf("PROFILE boot error: DEFAULT not applied\n");
    s_reason = "boot_default";
    printf("PROFILE boot channel=%s stored_deviation_x100=%u (provisional) profile=%s\n", rf_channel()->name,
           s_dev_table[rf_channel_index()], s_names[s_profile]);
}

/* After a channel change: back to DEFAULT through a TX-owned transition
 * (the retune already interrupts the picture). Another channel may carry
 * another VTX; a matched profile needs a new measurement. */
void profile_channel_changed(void)
{
    s_measuring = MEASURE_OFF;
    s_live_dev = -1;
    s_live_channel = (size_t)-1;
    s_switch_pending = -1;
    s_reason = "channel_changed";
    if (s_profile == PROFILE_DEFAULT) { profile_live_begin(); return; }
    if (s_menu_active) { (void)profile_set(PROFILE_DEFAULT); }
    else {
        status_screen("CHANNEL", rf_channel()->name, profile_name(PROFILE_DEFAULT), -1);
        (void)profile_set(PROFILE_DEFAULT);
        status_screen_end();
    }
    profile_live_begin();
}

void profile_measure_begin(void)
{
    s_measuring = MEASURE_OFF;
    dev_reset(&s_dev);
    s_measuring = MEASURE_STARTUP;
}

void profile_measure_end(void) { s_measuring = MEASURE_OFF; }
int profile_measure_result(void) { return s_dev.result_x100; }

/* Continuous passive measurement on live video for the channel session. */
void profile_live_begin(void)
{
    if (calibration_in_startup()) return;   /* the startup step owns the estimator */
    const size_t ch = rf_channel_index();
    if (s_measuring == MEASURE_LIVE && s_live_channel == ch) return;   /* already running */
    s_measuring = MEASURE_OFF;
    dev_reset(&s_dev);
    dev_monitor_reset(&s_mon, esp_timer_get_time());
    if (s_live_channel != ch) s_live_dev = -1;
    s_live_channel = ch;
    s_measuring = MEASURE_LIVE;
}

/* In-flight profile switch, control task only (the only restart caller).
 * Nothing changes unless the shared restart guard is ready, so the filter
 * relock and the tracker restart happen together or not at all. */
static void live_apply(void)
{
    const int p = s_switch_pending;
    if (p < 0 || p == s_profile) { s_switch_pending = -1; return; }
    if (s_menu_active || calibration_in_startup() || transport_faulted()) return;
    if (transport_switch_wait_ms()) return;                  /* retry after the cooldown */
    rf_filter_t f;
    const int cls = p == PROFILE_DEFAULT ? FILTER_NORMAL : p == PROFILE_WIDE ? FILTER_WIDE : FILTER_NARROW;
    if (!filter_setting(cls, &f)) { s_switch_pending = -1; ++s_live_switch_fail; return; }
    const rf_filter_t before = rf_filter();
    s_switch_pending = -1;
    if (!rf_filter_equal(before, f) && !rf_set_filter(f)) {
        (void)rf_set_filter(before);
        ++s_live_switch_fail;
        printf("PROFILE live switch to %s refused: filter code=%d skirt=%u did not read back\n", s_names[p],
               f.code, f.skirt);
        return;
    }
    /* Start level, LP off, in one guarded restart (none if both already hold). */
    if (!transport_apply_auto(profile_base_level(p), false)) {
        (void)rf_set_filter(before);
        ++s_live_switch_fail;
        printf("PROFILE live switch to %s failed: kp restart (see T recovery); filter restored\n", s_names[p]);
        return;
    }
    const int old = s_profile;
    s_profile = p;
    s_reason = "live_switch";
    ++s_live_switches;
    printf("PROFILE live switch %s -> %s deviation_x100=%d (filter relock + verified restart)\n", s_names[old],
           s_names[p], s_live_dev);
}

static void live_decided(int dev_x100, int64_t now)
{
    size_t ch = rf_channel_index();
    if (ch != s_live_channel) return;
    s_live_dev = dev_x100;
    const int want = profile_for_deviation(dev_x100);
    const bool class_changed = s_dev_table[ch] == 0 || profile_for_deviation(s_dev_table[ch]) != want;
    if (dev_monitor_should_store(s_dev_table[ch], dev_x100, class_changed, s_mon.stored_us, now)) {
        s_dev_table[ch] = (uint16_t)dev_x100;
        if (blob_store("vtxdev", s_dev_table, sizeof(s_dev_table))) s_mon.stored_us = now;
    }
    dev_monitor_decided(&s_mon, dev_x100, want, s_profile, now);
    const bool switch_now = profile_live_switch_wanted(s_live_switch, want, s_profile, s_mon.agree_streak);
    const char *what = want == s_profile ? "confirms" : switch_now ? "switching from" :
                       s_live_switch ? "awaits a second agreeing result; active stays" :
                       profile_width_rank(want) > profile_width_rank(s_profile) ? "WARNING too narrow:" :
                       "pending for the next boot/CALIBRATE; active stays";
    printf("PROFILE live n=%u channel=%s deviation_x100=%d want=%s %s %s agree=%u mismatch=%u mode=%s\n",
           s_mon.decisions, rf_channel()->name, dev_x100, s_names[want], what, s_names[s_profile],
           s_mon.agree_streak, s_mon.mismatch_streak, s_live_switch ? "live_switch" : "report_only");
    s_switch_pending = switch_now ? want : -1;
    live_apply();
}

/* One burst-confirmed AFC window; gates on a usable, centred carrier. */
void profile_observe(const afc2_result_t *r)
{
    const int mode = s_measuring;
    if (mode == MEASURE_OFF) return;
    const int64_t now = esp_timer_get_time();
    if (mode == MEASURE_LIVE) {
        if (s_live_channel != rf_channel_index()) return;
        if (s_switch_pending >= 0) live_apply();                 /* waiting for the restart cooldown */
        if (dev_monitor_tick(&s_mon, now)) dev_reset(&s_dev);   /* new cycle, or a timed-out one */
        if (!s_mon.collecting) return;
    }
    if (s_dev.result_x100 >= 0 || !r->sync_pairs || !r->porch_pairs || !s_afc_locked) return;
    ++s_observed;
    if (s_obs_p50 < 13 || s_obs_p50 > 32 || s_obs_coherence < 80 || s_obs_clip_pm || s_gain.state != DG_HOLD) {
        ++s_gated;
        return;
    }
    if (receiver_obs_cn_x10() < DEV_MONITOR_MIN_CN_X10) {
        ++s_gated_cn;
        return;
    }
    int32_t span = r->polarity < 0 ? r->porch_khz - r->sync_khz : r->sync_khz - r->porch_khz;
    unsigned rejected = s_dev.rejected;
    int dev = dev_add(&s_dev, span);
    if (s_dev.rejected != rejected) printf("PROFILE batch rejected (spread or disagreement), count=%u\n", s_dev.rejected);
    if (dev >= 0 && mode == MEASURE_LIVE) live_decided(dev, now);
}

/* Store a deviation measured in this channel session (startup or manual
 * CALIBRATE) and select its profile through a TX-owned transition. */
void profile_commit(int dev_x100)
{
    size_t ch = rf_channel_index();
    if (dev_x100 <= 0 || dev_x100 > 400) return;
    if (s_dev_table[ch] != dev_x100) {
        s_dev_table[ch] = (uint16_t)dev_x100;
        (void)blob_store("vtxdev", s_dev_table, sizeof(s_dev_table));
    }
    s_live_dev = dev_x100;
    s_live_channel = ch;
    int p = profile_for_deviation(dev_x100);
    if (p == PROFILE_DEFAULT) {
        s_reason = dev_x100 >= DEV_WIDE_MIN ? "no_wide_filter" : dev_x100 > DEV_KP060_MAX ? "measured_default_vtx" :
                   "no_narrow_filter";
        if (s_profile != PROFILE_DEFAULT) {
            status_screen("VTX MEASURED", "DEFAULT PROFILE", profile_name(PROFILE_DEFAULT), 100);
            (void)profile_set(PROFILE_DEFAULT);
            vTaskDelay(pdMS_TO_TICKS(800));
            status_screen_end();
        }
    } else {
        char line[48];
        snprintf(line, sizeof(line), "VTX DEVIATION %d.%02d", dev_x100 / 100, dev_x100 % 100);
        status_screen("VTX MATCHED", line, profile_name(p), 100);
        s_reason = profile_set(p) ? "measured_this_boot" : "narrow_filter_not_applied";
        vTaskDelay(pdMS_TO_TICKS(1200));
        status_screen_end();
    }
    printf("PROFILE channel=%s deviation_x100=%d profile=%s reason=%s\n", rf_channel()->name, dev_x100,
           s_names[s_profile], s_reason);
}

void profile_set_reason(const char *reason) { s_reason = reason; }

void profile_print(void)
{
    size_t ch = rf_channel_index();
    int want = s_live_dev > 0 ? profile_for_deviation(s_live_dev) : -1;
    printf("PROFILE active=%s reason=%s measured_this_session_x100=%d pending=%s stored_x100=%u (provisional) "
           "measuring=%s windows=%u batches=%u rejected_batches=%u observed=%lu gated=%lu gated_cn=%lu\n",
           s_names[s_profile], s_reason, s_live_channel == ch ? s_live_dev : -1,
           want < 0 || want == s_profile ? "none" : s_names[want], s_dev_table[ch],
           s_measuring == MEASURE_STARTUP ? "startup" : s_measuring == MEASURE_LIVE ? "live" : "off",
           s_dev.n, s_dev.batches, s_dev.rejected, (unsigned long)s_observed, (unsigned long)s_gated,
           (unsigned long)s_gated_cn);
    if (s_measuring == MEASURE_LIVE) {
        const int64_t now = esp_timer_get_time();
        printf("PROFILE monitor period_s=30 collecting=%u next_in_s=%lld decisions=%u timeouts=%u last_x100=%d "
               "last_age_s=%lld agree=%u mismatch=%u min_cn_db=%d mode=%s pending=%s switches=%lu switch_fail=%lu\n",
               s_mon.collecting,
               s_mon.collecting || s_mon.next_start_us <= now ? 0LL : (s_mon.next_start_us - now) / 1000000,
               s_mon.decisions, s_mon.timeouts, s_mon.last_x100,
               s_mon.decisions ? (now - s_mon.last_us) / 1000000 : -1LL, s_mon.agree_streak, s_mon.mismatch_streak,
               DEV_MONITOR_MIN_CN_X10 / 10, s_live_switch ? "live_switch" : "report_only",
               s_switch_pending >= 0 ? s_names[s_switch_pending] : "none", (unsigned long)s_live_switches,
               (unsigned long)s_live_switch_fail);
    }
}
