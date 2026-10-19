#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Optional native tracking gate; entirely inert under Direct Gain V5.
 * Startup gain ownership comes from the separate c5vrx4 NVS namespace. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
/* IQ lane policy, NVS c5vrx4/lane_mode, Z cycles it with a reboot. Fixed
 * fine {9,7,6,5} is the default: the lanes never switch at runtime. Fixed
 * ultrafine and protected adaptive V5 lanes remain comparisons. */
enum { C5VRX4_LANES_FINE = 0, C5VRX4_LANES_ULTRAFINE = 1, C5VRX4_LANES_ADAPTIVE = 2 };
#define C5VRX4_LANE_ADAPTIVE UINT8_MAX
uint8_t c5vrx4_lane_mode(void);
const char *c5vrx4_lane_mode_name(void);
/* RF lane set held for the whole session, or C5VRX4_LANE_ADAPTIVE. */
uint8_t c5vrx4_fixed_lane(void);
/* M: output transfer, NVS c5vrx4/cvbs_legacy (1 keeps LEGACY_FULL). */
enum { C5VRX4_CVBS_STD150 = 0, C5VRX4_CVBS_LEGACY = 1, C5VRX4_CVBS_150 = 2 };
unsigned c5vrx4_cvbs_mode(void);
const char *c5vrx4_cvbs_mode_name(void);
bool c5vrx4_cvbs_legacy_enabled(void);

/* Menu-editable boot options (NVS c5vrx4). The menu writes the next-boot
 * value; this boot keeps the value it started with (the getters cache it),
 * so an option whose stored value differs is pending until the reboot that
 * SAVE AND EXIT performs. Indices are the menu's item order. */
enum {
    C5VRX4_OPT_FIXED_BW, C5VRX4_OPT_LANES,           /* RF page */
    C5VRX4_OPT_AGC_MASK, C5VRX4_OPT_DC_RECENTER, C5VRX4_OPT_SPHASE,
    C5VRX4_OPT_IDLE_RASTER, C5VRX4_OPT_RADIUS_BOOST, C5VRX4_OPT_SYNC_FW,
    C5VRX4_OPT_LEVEL, C5VRX4_OPT_CVBS, C5VRX4_OPT_HISTORY, C5VRX4_OPT_NATIVE_PATCH, C5VRX4_OPT_HW_DCO,
    C5VRX4_OPT_COUNT
};
void c5vrx4_options_snapshot(void);
const char *c5vrx4_option_label(unsigned option);
const char *c5vrx4_option_value(unsigned option);
bool c5vrx4_option_pending(unsigned option);
bool c5vrx4_option_cycle(unsigned option);
bool c5vrx4_options_pending(void);
/* Native AGC restart patch 71C4[25:23]=7 (NVS native_patch, default on). */
bool c5vrx4_native_patch_enabled(void);
/* Range-edge hardware DC correction (NVS dco_auto, default on). */
bool c5vrx4_hw_dco_enabled(void);
/* Opaque NVS blob store (c5vrx4/<key>); returns false when absent/short. */
bool c5vrx4_blob_load(const char *key, void *data, size_t size);
bool c5vrx4_blob_store(const char *key, const void *data, size_t size);

bool c5vrx4_lane_window_ready(uint64_t now_us);
uint8_t c5vrx4_lane_target(uint8_t current, uint8_t requested, const uint8_t *sample, size_t bytes, uint64_t now_us);
void c5vrx4_lane_print(void);

/* Default-on pre-demodulation correction (#165), NVS opt-outs, reboot:
 * '%' digital DC recentring of the static Phase8 decoder (dc_recenter),
 * '&' one automatic sampling-phase check at the first carrier lock (sphase_auto). */
bool c5vrx4_dc_recenter_enabled(void);
bool c5vrx4_sphase_auto_enabled(void);
/* Fixed analog bandwidth (default on, '^' opts out with a reboot), after
 * ESPARGOS esp-sdr's C5 BANDWIDTH control: one absolute RX0 capacitor code in
 * BBTOP 0x67 regs 6/7, chosen from the receiver-noise width measured on this
 * chip so the full -3 dB width is the narrowest still >= bw_target (24 MHz),
 * or the widest available when none reaches it. The digital path stays BW40
 * and the BW20/BW40 gear is retired. Measured once without a carrier ('='
 * repeats it); NVS bw_code (255 = not yet measured) and bw_width. */
#define C5VRX4_BW_UNCALIBRATED UINT8_MAX
bool c5vrx4_fixed_bw_enabled(void);
uint8_t c5vrx4_bw_code(void);
unsigned c5vrx4_bw_width_khz(void);
unsigned c5vrx4_bw_target_khz(void);
bool c5vrx4_bw_store(uint8_t code, unsigned width_khz);
/* Second filter stage (regs 8..13 offset, 0 = calibrated bytes) chosen by the
 * same measurement for the lowest noise bandwidth at the target width, alias
 * included; NVS bw_skirt and bw_nbw (measured noise bandwidth, kHz). */
unsigned c5vrx4_bw_skirt(void);
unsigned c5vrx4_bw_nbw_khz(void);
bool c5vrx4_bw_skirt_store(unsigned skirt, unsigned nbw_khz);
/* Edge profile (measured by the same calibration, VTX off): the filter
 * setting the V5 bandwidth gear selects at the range edge. Code 255 = none
 * (no measured setting was >= 0.5 dB better in noise bandwidth); digital =
 * phy_wifi_fbw_sel(0) with that analog code. NVS bw_ecode, bw_edig, bw_enbw. */
uint8_t c5vrx4_bw_edge_code(void);
bool c5vrx4_bw_edge_digital(void);
unsigned c5vrx4_bw_edge_nbw_khz(void);
bool c5vrx4_bw_edge_store(uint8_t code, bool digital_bw20, unsigned nbw_khz);
/* Edge-profile second stage (regs 8..13), NVS bw_eskirt; NONE keeps the
 * normal skirt in the edge profile. */
#define C5VRX4_BW_EDGE_SKIRT_NONE UINT8_MAX
uint8_t c5vrx4_bw_edge_skirt(void);
bool c5vrx4_bw_edge_skirt_store(uint8_t skirt);
/* Native AGC acquisition mask (native mode only, '|' opts out with a
 * reboot). PARLIO data bit 0 (fine Q LSB) carries a MODEM_DIAG AGC state bit
 * found by the witness calibration ('*'); the static program holds the DAC
 * through every acquisition. agc_flag: bits 0..1 = state bit (DIAG[28+n]),
 * bit 7 = inverted, 255 = not calibrated. */
#define C5VRX4_AGC_FLAG_UNKNOWN UINT8_MAX
uint8_t c5vrx4_agc_flag(void);
bool c5vrx4_agc_flag_store(uint8_t flag);
bool c5vrx4_agc_mask_enabled(void);
/* Native + enabled + calibrated + STATIC decode: masked program and lane. */
bool c5vrx4_agc_mask_active(void);
/* No-carrier idle raster (default on, '_' opts out with a reboot): after 2 s
 * without any carrier or sync, the standalone raster emits clean black video
 * in the last live standard so strict goggles (HDZero/TP2825) stay locked;
 * live video returns at the first carrier or sync. NVS idle_raster. */
bool c5vrx4_idle_raster_enabled(void);
/* Last stable live standard (0 NTSC, 1 PAL, 255 unknown), NVS last_std:
 * the idle raster and an AUTO menu start in it after a reboot. */
#define C5VRX4_STD_UNKNOWN UINT8_MAX
uint8_t c5vrx4_last_standard(void);
void c5vrx4_last_standard_store(uint8_t standard);
/* V5 strong-signal radius boost (opt-in since 2026-10-04, 'y' toggles, reboot):
 * on a strong, tight, rail-free carrier the healthy P50 band moves from
 * 13..32 to 30..46 for finer phase quantization; the first rail code, P95
 * or level jump drops it. NVS radius_boost. */
bool c5vrx4_radius_boost_enabled(void);
/* Sync flywheel (default on, 'w' toggles, reboot; SYNC_FLYWHEEL.md): missing
 * or noisy H/V sync pulses are rebuilt in the raw ring ahead of the TX read so
 * the goggles always see a valid PAL/NTSC raster. NVS sync_fw. */
bool c5vrx4_sync_flywheel_enabled(void);
/* u: experimental automatic CVBS level servo, opt-in/reboot. */
bool c5vrx4_level_enabled(void);
