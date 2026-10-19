/* C5VRX-5 by Twotoz and the C5VRX contributors: receiver-wide state.
 *
 * Data plane (hardware only): MODEM_DIAG Q4/I4 (fixed ultrafine lane) ->
 * PARLIO RX 40 MS/s -> 32 KiB cyclic raw ring -> TX BitScrambler U85Q
 * (50 ns span, two bundles, LUT16) -> PARLIO TX 40 MHz [D,D] -> 6-bit DAC.
 * The CPU only observes completed RX descriptors and owns gain, AFC, DC,
 * filter, menu and console. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/dma_types.h"
#include "gain.h"
#include "menu_raster.h"
#include "rf.h"

#define IQ_RATE_HZ          40000000u
#define RAW_RING_BYTES      32768u
#define DAC_IDLE_CODE       20u          /* blanking level; sync is 0 */
#define CONTROL_BYTES       4092u        /* one completed GDMA descriptor */
#define PROBE_REGIONS       4u
#define PROBE_REGION_BYTES  64u
#define PROBE_BYTES         (PROBE_REGIONS * PROBE_REGION_BYTES)
#define MAX_RING_DESCRIPTORS 16
/* AFC centres the VTX blanking level here. The U85Q window (-6..+9 MHz)
 * holds more of the sync-to-white swing with the porch at -1.0 MHz than at
 * the -436 kHz that suited the trained trackers: held-out simulation,
 * 400 cases, picture 0.969 vs 0.908, +0.85 dB SINAD, deviation 1.8 VTXs
 * 0.90 vs 0.64 (docs/FINDINGS.md). */
#define AFC_PORCH_TARGET_KHZ (-1000)

typedef struct {
    dma_descriptor_t *dscr;
    uint8_t *buffer;
    uint32_t length;
} ring_node_t;

/* Cheap envelope/phase statistics of a raw Q4/I4 window. */
typedef struct {
    int p_median;      /* median i^2+q^2 of the signed nibbles */
    int q_phase;       /* % of adjacent pairs with a coherent phase step */
    int clip_pm;       /* rail codes, per mille */
    int origin_pm;     /* origin cells, per mille */
} metrics_t;
metrics_t measure_window(const uint8_t *s, size_t n);

typedef enum { STD_MODE_AUTO = 0, STD_MODE_NTSC = 1, STD_MODE_PAL = 2 } std_mode_t;

/* ---- transport.c ---- */
extern uint8_t s_raw_ring[RAW_RING_BYTES];
extern int s_rx_dma_ch, s_tx_dma_ch;
extern ring_node_t s_rx_nodes[MAX_RING_DESCRIPTORS];
extern int s_rx_node_count;
esp_err_t transport_start(void);
void transport_stop_tx(void);
void transport_resume_live(void);
void transport_start_chain(dma_descriptor_t *first);
/* One tracker topology (U85Q / U85Q-LP); a profile only picks the filter
 * and the kp level the pipeline starts from (kp_tables.h). */
bool transport_set_base_level(int level);     /* not live: start level, LP off */
uint32_t transport_switch_wait_ms(void);
bool transport_kp_set(int level);          /* verified kp ladder level (kp_tables.h) */
int transport_kp_level(void);
int transport_kp_target(void);
unsigned transport_kp_tick(void);
bool transport_set_lp(bool lp);            /* U85Q <-> LP via a brief pipeline restart */
bool transport_lp(void);
const char *transport_program_name(void);
bool transport_faulted(void);
uint32_t transport_restart_errors(void);
unsigned transport_output(void);
const char *transport_output_name(void);
bool transport_set_output(unsigned level);
bool transport_apply_auto(int level, bool lp);
void transport_stop_chain(void);
void transport_poll_faults(void);
uint32_t transport_fault_count(void);
void transport_print(void);
void sync_dma_c2m(const void *addr, size_t size);
void sync_dma_m2c(const void *addr, size_t size);
int ring_node_index(uint32_t addr);
/* Copies that RX cannot have overwritten (false = stale, retry). */
bool copy_control_window(uint8_t *dst, size_t bytes);
bool copy_probe_window(uint8_t dst[PROBE_BYTES], int *node);
bool copy_snapshot(uint8_t *dst);
bool copy_recent_endpoints(uint8_t *dst, size_t n);
void rx_clock_slip(uint32_t us);

/* ---- receiver.c (Direct Gain V5 owner) ---- */
extern direct_gain_t s_gain;
extern volatile uint8_t s_current_gain;
extern volatile uint32_t s_gain_epoch;      /* bumps on every gain/offset write */
extern volatile uint32_t s_control_epoch;   /* bumps on retune/scan/calibration */
extern volatile int64_t s_last_gain_write_us;
extern volatile int s_obs_p50, s_obs_p95, s_obs_origin_pm, s_obs_clip_pm, s_obs_coherence;
extern volatile bool s_obs_stuck;   /* fixed IQ point, not a carrier (gain.h dg_stuck) */
extern volatile int s_obs_env_ratio_x100;   /* gain.h env_ratio_x100 of the latest window */
extern volatile uint32_t s_obs_us_max, s_obs_windows;
extern dg_map_blob_t s_gain_map_saved;
extern bool s_gain_map_saved_valid;
void receiver_start(void);
bool receiver_active(void);
bool receiver_paused(void);
/* Pause gain control for a calibration/scan that owns the gain itself. */
bool receiver_pause(const char *tag);
void receiver_resume(void);
void receiver_set_gain(uint8_t gain);
void receiver_print(void);
void receiver_print_map(void);   /* console M: GAINMAP line */

/* ---- control.c ---- */
extern volatile bool s_menu_active;
extern volatile int s_signal_strength;
extern volatile bool s_scan_active;
extern volatile unsigned s_scan_progress;
extern volatile int s_cfo_khz;
extern volatile bool s_afc_locked;
extern volatile std_mode_t s_std_mode;
extern volatile video_standard_t s_video_std;      /* standard the menu emits */
extern volatile bool s_detected_std_valid;
extern volatile video_standard_t s_detected_std;
extern volatile int64_t s_last_video_us;           /* last burst-confirmed sync */
extern QueueHandle_t s_commands;
/* c5vrx-wifi bridge: s_commands value COMMAND_SET_CHANNEL + index tunes
 * straight to that channel (index = band * 8 + channel, rf.h order). */
#define COMMAND_SET_CHANNEL 0x100
/* c5vrx-wifi bridge: the bridge's keys arrive on D4 (GPIO23), the DAC's
 * bit 0, which then stays off the video bus (5-bit video, bit 0 always 0)
 * so no wire to the GPIO2 pad on the back is needed. 0: keys on GPIO2,
 * full 6-bit DAC. */
#ifndef BRIDGE_KEYS_ON_D4
#define BRIDGE_KEYS_ON_D4 1
#endif
void control_start(void);
video_standard_t menu_standard(void);
void control_print(void);
void control_print_beat(void);   /* STATUS line every 2 s (console.c) */

/* ---- calibration.c ---- */
void calibration_start(void);
bool filter_calibrate(bool automatic);
void filter_request(void);
/* True until the startup calibration has finished: afterwards nothing
 * intrusive (searches, sweeps, slips, retunes) runs for the rest of the
 * boot. A manual calibration from the menu reopens it once. */
bool calibration_in_startup(void);
/* Calibrated analog filter classes: normal (>= 12 MHz, the default),
 * narrow (>= 10 MHz, low-deviation VTXs), wide (>= 16 MHz, wide VTXs).
 * false when that class was not calibrated. */
enum { FILTER_NORMAL, FILTER_NARROW, FILTER_WIDE };
bool filter_setting(int filter_class, rf_filter_t *out);
void calibration_print(void);
void dco_post_gain(uint8_t gain);
/* Chain state for the diagnostics (no side effects). */
const char *calibration_filter_result(void);
int calibration_filter_class_mhz(void);
/* C/N model of the latest observer window; CN_UNKNOWN_X10 on a fixed IQ point. */
int receiver_obs_cn_x10(void);
unsigned calibration_narrow_width_khz(void);   /* measured -3 dB width, 0 = none */
const char *calibration_sphase_state(void);
bool calibration_sphase_settled(void);
/* DC correction in place for this gain: held, or not needed (outside the
 * calibrated rows, or below the measured noise floor). */
bool calibration_dc_ok(uint8_t gain);
/* Explicit bench-only lane comparison (console L): video stops ~2 s. */
void lane_compare_request(void);

/* ---- menu.c ---- */
enum { MENU_BAND, MENU_CHANNEL, MENU_SCAN, MENU_VIDEO, MENU_CALIBRATE, MENU_OUTPUT, MENU_EXIT, MENU_ITEMS };
extern volatile int s_menu_cursor;
bool menu_open(void);
void menu_close(void);
void menu_render(void);
void menu_set_standard_mode(std_mode_t mode);
void status_screen(const char *title, const char *line1, const char *line2, int percent);
void status_screen_end(void);
bool menu_status_active(void);

/* ---- profile.c ---- */
#include "afc.h"
const char *profile_name(int profile);
int profile_active(void);
int profile_deviation_x100(size_t channel);
int profile_live_deviation_x100(void);     /* measured this channel session, -1 = not yet */
int profile_for_deviation(int dev_x100);
int profile_base_level(int profile);       /* kp_tables.h level a profile starts from */
bool profile_set(int profile);
void profile_load(void);
void profile_boot(void);
void profile_channel_changed(void);
void profile_measure_begin(void);
void profile_measure_end(void);
int profile_measure_result(void);
void profile_observe(const afc2_result_t *r);
void profile_commit(int dev_x100);
void profile_live_begin(void);
const char *profile_reason(void);
void profile_set_reason(const char *reason);
void profile_print(void);
bool profile_live_mode(void);              /* live profile switch on (NVS prof_live) */
void profile_set_live_mode(bool on);

/* ---- diag.c: passive field diagnostics (diag_logic.h) ---- */
void diag_window(bool measured, bool video_valid, bool afc_step);   /* control task, every window */
void diag_poll(void);                   /* console task: prints queued records */
void diag_report(void);                 /* console D */
void diag_toggle_stream(void);          /* console R */


/* ---- settings.c ---- */
void settings_load(void);
void settings_save(void);
bool blob_load(const char *key, void *data, size_t size);
bool blob_store(const char *key, const void *data, size_t size);

/* ---- console.c ---- */
void console_start(void);
