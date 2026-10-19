/* C5VRX by Twotoz and contributors: span75 transport and opt-in native gate. */
#include "c5vrx4.h"
#include "sdkconfig.h"
#if !CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT
#error "C5VRX-4 requires CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT=y for Direct Gain V5"
#endif
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rf.h"
#include "cvbs_tables.h"

#define AGC_CTRL (*(volatile uint32_t *)0x600a7030u)
#define AGC_HOLD (1u << 29)
#define PERIOD_US 1000u
#define WINDOW_US 20u
static gptimer_handle_t s_timer;
static SemaphoreHandle_t s_transition_lock;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_requested = true, s_running, s_open;
static unsigned s_suspend_depth;
static uint64_t s_next_open, s_opened_at;
static uint32_t s_opens, s_faults, s_late_max, s_open_max;
static bool s_history_loaded, s_history = false;
static bool s_cvbs_loaded;
static unsigned s_cvbs_mode;
static bool s_level_loaded, s_level;

bool c5vrx4_level_enabled(void)
{
    if (!s_level_loaded) {
        nvs_handle_t h; uint8_t enabled = 1;
        if (nvs_open("c5vrx4", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u8(h, "level_lab", &enabled); nvs_close(h);
        }
        s_level = enabled == 1; s_level_loaded = true;
    }
    return s_level;
}

static bool s_lane_mode_loaded;
static uint8_t s_lane_mode = C5VRX4_LANES_FINE;

unsigned c5vrx4_cvbs_mode(void)
{
    if (!s_cvbs_loaded) {
        nvs_handle_t handle;
        uint8_t mode = C5VRX4_CVBS_STD150;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            /* Historical key: 1 still means LEGACY_FULL. */
            (void)nvs_get_u8(handle, "cvbs_legacy", &mode);
            nvs_close(handle);
        }
        s_cvbs_mode = mode <= C5VRX4_CVBS_150 ? mode : C5VRX4_CVBS_STD150;
        s_cvbs_loaded = true;
    }
    return s_cvbs_mode;
}

bool c5vrx4_cvbs_legacy_enabled(void)
{
    return c5vrx4_cvbs_mode() == C5VRX4_CVBS_LEGACY;
}

const char *c5vrx4_cvbs_mode_name(void)
{
    static const char *const names[] = {"STD150", "LEGACY_FULL", "CVBS150"};
    return names[c5vrx4_cvbs_mode()];
}

uint8_t c5vrx4_lane_mode(void)
{
    if (!s_lane_mode_loaded) {
        nvs_handle_t handle;
        uint8_t mode = C5VRX4_LANES_FINE;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "lane_mode", &mode);
            nvs_close(handle);
        }
        /* The older force_ultra_v2 comparison key is deliberately ignored. */
        s_lane_mode = mode <= C5VRX4_LANES_ADAPTIVE ? mode : C5VRX4_LANES_FINE;
        s_lane_mode_loaded = true;
    }
    return s_lane_mode;
}

const char *c5vrx4_lane_mode_name(void)
{
    static const char *const names[] = {"fixed_fine", "fixed_ultrafine", "protected_v5"};
    return names[c5vrx4_lane_mode()];
}

uint8_t c5vrx4_fixed_lane(void)
{
    /* RF lane sets: 0 coarse {9,8,7,6}, 1 fine {9,7,6,5}, 2 ultrafine {9,6,5,4}.
     * Native AGC settles for the full 10-bit ADC, beyond the fine +-256
     * window (board 2026-10-06, with the restart patch: clip 148 pm fine,
     * 21-27 pm coarse), so native boots on coarse; fixed for the session. */
    if (rf_native_agc_active()) return 0u;
    static const uint8_t lanes[] = {1u, 2u, C5VRX4_LANE_ADAPTIVE};
    return lanes[c5vrx4_lane_mode()];
}

static bool nvs_flag(const char *key, bool fallback)
{
    nvs_handle_t handle;
    uint8_t value = fallback ? 1u : 0u;
    if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, key, &value);
        nvs_close(handle);
    }
    return value != 0;
}

static int8_t s_hw_dco = -1;
bool c5vrx4_blob_load(const char *key, void *data, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open("c5vrx4", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = size;
    esp_err_t err = nvs_get_blob(handle, key, data, &length);
    nvs_close(handle);
    return err == ESP_OK && length == size;
}

bool c5vrx4_blob_store(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, data, size);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) printf("C5VRX4 blob_store key=%s err=%s\n", key, esp_err_to_name(err));
    return err == ESP_OK;
}

bool c5vrx4_hw_dco_enabled(void)
{
    if (s_hw_dco < 0) s_hw_dco = nvs_flag("dco_auto", true);
    return s_hw_dco;
}

static int8_t s_native_patch = -1;
bool c5vrx4_native_patch_enabled(void)
{
    if (s_native_patch < 0) s_native_patch = nvs_flag("native_patch", true);
    return s_native_patch;
}

static int8_t s_dc_recenter = -1, s_sphase_auto = -1;
bool c5vrx4_dc_recenter_enabled(void)
{
    if (s_dc_recenter < 0) s_dc_recenter = nvs_flag("dc_recenter", true);
    return s_dc_recenter;
}

bool c5vrx4_sphase_auto_enabled(void)
{
    if (s_sphase_auto < 0) s_sphase_auto = nvs_flag("sphase_auto", true);
    return s_sphase_auto;
}

/* Fixed analog bandwidth: one measured RX filter code, never geared. */
static int8_t s_fixed_bw = -1;
bool c5vrx4_fixed_bw_enabled(void)
{
    if (s_fixed_bw < 0) s_fixed_bw = nvs_flag("fixed_bw", true);
    return s_fixed_bw;
}

static bool s_bw_loaded;
static uint8_t s_bw_code = C5VRX4_BW_UNCALIBRATED, s_bw_target_mhz = 24;
static uint16_t s_bw_width_khz, s_bw_nbw_khz;
static uint8_t s_bw_skirt;
static uint8_t s_bw_edge_code = C5VRX4_BW_UNCALIBRATED, s_bw_edge_dig;
static uint16_t s_bw_edge_nbw_khz;
static uint8_t s_bw_edge_skirt = C5VRX4_BW_EDGE_SKIRT_NONE;
static void bw_load(void)
{
    if (s_bw_loaded) return;
    nvs_handle_t handle;
    if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, "bw_code", &s_bw_code);
        (void)nvs_get_u16(handle, "bw_width", &s_bw_width_khz);
        (void)nvs_get_u8(handle, "bw_target", &s_bw_target_mhz);
        (void)nvs_get_u8(handle, "bw_skirt", &s_bw_skirt);
        (void)nvs_get_u16(handle, "bw_nbw", &s_bw_nbw_khz);
        (void)nvs_get_u8(handle, "bw_ecode", &s_bw_edge_code);
        (void)nvs_get_u8(handle, "bw_edig", &s_bw_edge_dig);
        (void)nvs_get_u16(handle, "bw_enbw", &s_bw_edge_nbw_khz);
        (void)nvs_get_u8(handle, "bw_eskirt", &s_bw_edge_skirt);
        nvs_close(handle);
    }
    if (s_bw_skirt > 60u) s_bw_skirt = 0;
    if (s_bw_code != C5VRX4_BW_UNCALIBRATED && s_bw_code > 63u) s_bw_code = C5VRX4_BW_UNCALIBRATED;
    if (s_bw_target_mhz < 12u || s_bw_target_mhz > 40u) s_bw_target_mhz = 24u;
    if (s_bw_edge_code > 63u) s_bw_edge_code = C5VRX4_BW_UNCALIBRATED;
    if (s_bw_edge_skirt > 60u) s_bw_edge_skirt = C5VRX4_BW_EDGE_SKIRT_NONE;
    s_bw_loaded = true;
}
uint8_t c5vrx4_bw_code(void) { bw_load(); return s_bw_code; }
unsigned c5vrx4_bw_width_khz(void) { bw_load(); return s_bw_width_khz; }
unsigned c5vrx4_bw_target_khz(void) { bw_load(); return s_bw_target_mhz * 1000u; }
unsigned c5vrx4_bw_skirt(void) { bw_load(); return s_bw_code == C5VRX4_BW_UNCALIBRATED ? 0u : s_bw_skirt; }
unsigned c5vrx4_bw_nbw_khz(void) { bw_load(); return s_bw_nbw_khz; }
bool c5vrx4_bw_skirt_store(unsigned skirt, unsigned nbw_khz)
{
    bw_load();
    if (skirt > 60u) return false;
    uint16_t nbw = (uint16_t)(nbw_khz > 65535u ? 65535u : nbw_khz);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bw_skirt", (uint8_t)skirt);
        if (err == ESP_OK) err = nvs_set_u16(handle, "bw_nbw", nbw);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) { s_bw_skirt = (uint8_t)skirt; s_bw_nbw_khz = nbw; }
    else printf("C5VRX4 bw_skirt_store err=%s\n", esp_err_to_name(err));
    return err == ESP_OK;
}
uint8_t c5vrx4_bw_edge_code(void)
{
    bw_load();
    return s_bw_code == C5VRX4_BW_UNCALIBRATED ? C5VRX4_BW_UNCALIBRATED : s_bw_edge_code;
}
bool c5vrx4_bw_edge_digital(void) { bw_load(); return s_bw_edge_dig != 0; }
uint8_t c5vrx4_bw_edge_skirt(void)
{
    bw_load();
    return c5vrx4_bw_edge_code() == C5VRX4_BW_UNCALIBRATED ? C5VRX4_BW_EDGE_SKIRT_NONE : s_bw_edge_skirt;
}
bool c5vrx4_bw_edge_skirt_store(uint8_t skirt)
{
    bw_load();
    if (skirt != C5VRX4_BW_EDGE_SKIRT_NONE && skirt > 60u) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bw_eskirt", skirt);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) s_bw_edge_skirt = skirt;
    else printf("C5VRX4 bw_edge_skirt_store err=%s\n", esp_err_to_name(err));
    return err == ESP_OK;
}
unsigned c5vrx4_bw_edge_nbw_khz(void) { bw_load(); return s_bw_edge_nbw_khz; }
bool c5vrx4_bw_edge_store(uint8_t code, bool digital_bw20, unsigned nbw_khz)
{
    bw_load();
    if (code != C5VRX4_BW_UNCALIBRATED && code > 63u) return false;
    uint16_t nbw = (uint16_t)(nbw_khz > 65535u ? 65535u : nbw_khz);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bw_ecode", code);
        if (err == ESP_OK) err = nvs_set_u8(handle, "bw_edig", digital_bw20 ? 1u : 0u);
        if (err == ESP_OK) err = nvs_set_u16(handle, "bw_enbw", nbw);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) { s_bw_edge_code = code; s_bw_edge_dig = digital_bw20; s_bw_edge_nbw_khz = nbw; }
    else printf("C5VRX4 bw_edge_store err=%s\n", esp_err_to_name(err));
    return err == ESP_OK;
}
bool c5vrx4_bw_store(uint8_t code, unsigned width_khz)
{
    bw_load();
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bw_code", code);
        if (err == ESP_OK) err = nvs_set_u16(handle, "bw_width", (uint16_t)(width_khz > 65535u ? 65535u : width_khz));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) { s_bw_code = code; s_bw_width_khz = (uint16_t)(width_khz > 65535u ? 65535u : width_khz); }
    else printf("C5VRX4 bw_store err=%s\n", esp_err_to_name(err));
    return err == ESP_OK;
}

/* Native AGC acquisition mask: calibrated witness bit and opt-out. */
static int16_t s_agc_flag = -1;
static int8_t s_agc_mask = -1;
uint8_t c5vrx4_agc_flag(void)
{
    if (s_agc_flag < 0) {
        uint8_t value = C5VRX4_AGC_FLAG_UNKNOWN;
        nvs_handle_t handle;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "agc_flag", &value);
            nvs_close(handle);
        }
        if (value != C5VRX4_AGC_FLAG_UNKNOWN && (value & 0x7cu)) value = C5VRX4_AGC_FLAG_UNKNOWN;
        s_agc_flag = value;
    }
    return (uint8_t)s_agc_flag;
}
bool c5vrx4_agc_flag_store(uint8_t flag)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "agc_flag", flag);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) s_agc_flag = flag;
    else printf("C5VRX4 agc_flag store err=%s\n", esp_err_to_name(err));
    return err == ESP_OK;
}
bool c5vrx4_agc_mask_enabled(void)
{
    if (s_agc_mask < 0) s_agc_mask = nvs_flag("agc_mask", true);
    return s_agc_mask;
}
bool c5vrx4_agc_mask_active(void)
{
    /* Latched per boot: program and lane route are chosen together at start,
     * so a calibration stored later only takes effect after a reboot. */
    static int8_t active = -1;
    if (active < 0)
        active = rf_native_agc_active() && c5vrx4_agc_mask_enabled() &&
                 c5vrx4_agc_flag() != C5VRX4_AGC_FLAG_UNKNOWN && !c5vrx4_history_enabled();
    return active;
}

static int8_t s_idle_raster = -1;
bool c5vrx4_idle_raster_enabled(void)
{
    if (s_idle_raster < 0) s_idle_raster = nvs_flag("idle_raster", true);
    return s_idle_raster;
}

static int8_t s_radius_boost = -1;
bool c5vrx4_radius_boost_enabled(void)
{
    /* Opt-in since 2026-10-04: extra gain writes for a benefit that an earlier
     * lane/radius hardware A/B did not show (RADIUS_BOOST.md). */
    if (s_radius_boost < 0) s_radius_boost = nvs_flag("radius_boost", false);
    return s_radius_boost;
}

static int8_t s_sync_fw = -1;
bool c5vrx4_sync_flywheel_enabled(void)
{
    /* Default off (operator, 2026-10-05): on hardware the 100 us wake starved
     * IDLE and the USB console (task watchdog) and the menu stopped working.
     * 'w' opts in until its CPU gate passes (SYNC_FLYWHEEL.md). */
    if (s_sync_fw < 0) s_sync_fw = nvs_flag("sync_fw", false);
    return s_sync_fw;
}

static int16_t s_last_std = -1;
uint8_t c5vrx4_last_standard(void)
{
    if (s_last_std < 0) {
        uint8_t value = C5VRX4_STD_UNKNOWN;
        nvs_handle_t handle;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "last_std", &value);
            nvs_close(handle);
        }
        s_last_std = value > 1u ? C5VRX4_STD_UNKNOWN : value;
    }
    return (uint8_t)s_last_std;
}

/* Written only when the stable standard changes (normally once per camera). */
void c5vrx4_last_standard_store(uint8_t standard)
{
    if (standard > 1u || standard == c5vrx4_last_standard()) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "last_std", standard);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) s_last_std = standard;
    else printf("C5VRX4 last_std store err=%s\n", esp_err_to_name(err));
}

typedef struct {
    const char *label, *key;
    uint8_t fallback, count;
    const char *const *names;
} c5vrx4_option_t;
static const char *const s_off_on[] = {"OFF", "ON"};
static const char *const s_bw_names[] = {"VENDOR", "FIXED"};
static const char *const s_lane_names[] = {"FINE", "ULTRAFINE", "ADAPTIVE"};
static const char *const s_cvbs_names[] = {"STD150", "LEGACY", "CVBS150"};
/* Keys and fallbacks are the getters' own (one source per key below). */
static const c5vrx4_option_t s_options[C5VRX4_OPT_COUNT] = {
    [C5VRX4_OPT_FIXED_BW]     = {"ANALOG BW",     "fixed_bw",     1, 2, s_bw_names},
    [C5VRX4_OPT_LANES]        = {"IQ LANES",      "lane_mode",    C5VRX4_LANES_FINE, 3, s_lane_names},
    [C5VRX4_OPT_AGC_MASK]     = {"AGC MASK",      "agc_mask",     1, 2, s_off_on},
    [C5VRX4_OPT_DC_RECENTER]  = {"DC RECENTER",   "dc_recenter",  1, 2, s_off_on},
    [C5VRX4_OPT_SPHASE]       = {"SAMPLE PHASE",  "sphase_auto",  1, 2, s_off_on},
    [C5VRX4_OPT_IDLE_RASTER]  = {"IDLE RASTER",   "idle_raster",  1, 2, s_off_on},
    [C5VRX4_OPT_RADIUS_BOOST] = {"RADIUS BOOST",  "radius_boost", 0, 2, s_off_on},
    [C5VRX4_OPT_SYNC_FW]      = {"SYNC FLYWHEEL", "sync_fw",      0, 2, s_off_on},
    [C5VRX4_OPT_LEVEL]        = {"LEVEL SERVO",   "level_lab",    1, 2, s_off_on},
    [C5VRX4_OPT_CVBS]         = {"CVBS SCALE",    "cvbs_legacy",  C5VRX4_CVBS_STD150, 3, s_cvbs_names},
    [C5VRX4_OPT_HISTORY]      = {"HISTORY DEMOD", "unwrap_hc",    0, 2, s_off_on},
    [C5VRX4_OPT_NATIVE_PATCH] = {"NATIVE PATCH",  "native_patch", 1, 2, s_off_on},
    [C5VRX4_OPT_HW_DCO]       = {"HW DC CORR",    "dco_auto",     1, 2, s_off_on},
};
static uint8_t s_option_boot[C5VRX4_OPT_COUNT];
static bool s_option_snapshot;

static uint8_t option_stored(unsigned option)
{
    const c5vrx4_option_t *o = &s_options[option];
    uint8_t value = o->fallback;
    nvs_handle_t handle;
    if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, o->key, &value);
        nvs_close(handle);
    }
    /* Out-of-range bytes read as the fallback, like the getters. */
    return value < o->count ? value : o->fallback;
}

void c5vrx4_options_snapshot(void)
{
    for (unsigned k = 0; k < C5VRX4_OPT_COUNT; ++k) s_option_boot[k] = option_stored(k);
    s_option_snapshot = true;
}

const char *c5vrx4_option_label(unsigned option)
{
    return option < C5VRX4_OPT_COUNT ? s_options[option].label : "";
}

const char *c5vrx4_option_value(unsigned option)
{
    if (option >= C5VRX4_OPT_COUNT) return "";
    return s_options[option].names[option_stored(option)];
}

bool c5vrx4_option_pending(unsigned option)
{
    return option < C5VRX4_OPT_COUNT && s_option_snapshot &&
           option_stored(option) != s_option_boot[option];
}

bool c5vrx4_options_pending(void)
{
    for (unsigned k = 0; k < C5VRX4_OPT_COUNT; ++k)
        if (c5vrx4_option_pending(k)) return true;
    return false;
}

bool c5vrx4_option_cycle(unsigned option)
{
    if (option >= C5VRX4_OPT_COUNT) return false;
    const c5vrx4_option_t *o = &s_options[option];
    uint8_t next = (uint8_t)((option_stored(option) + 1u) % o->count);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, o->key, next);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    printf("C5VRX4 option %s_next=%s err=%s action=%s\n", o->key, o->names[next],
           esp_err_to_name(err), err == ESP_OK ? "applies_after_reboot" : "unchanged");
    return err == ESP_OK;
}

static bool toggle_flag(const char *key, bool current, const char *name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, key, current ? 0u : 1u);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    printf("C5VRX4 %s_next=%u err=%s action=%s\n", name, !current,
           esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
    if (err == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
    return true;
}

bool c5vrx4_history_enabled(void)
{
    if (!s_history_loaded) {
        nvs_handle_t handle;
        uint8_t enabled = 0;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "unwrap_hc", &enabled);
            nvs_close(handle);
        }
        s_history = enabled != 0;
        s_history_loaded = true;
    }
    return s_history;
}

static bool IRAM_ATTR gate_alarm(gptimer_handle_t timer,
                                const gptimer_alarm_event_data_t *event,
                                void *context)
{
    (void)context;
    portENTER_CRITICAL_ISR(&s_lock);
    if (!s_running) {
        AGC_CTRL &= ~AGC_HOLD;
        portEXIT_CRITICAL_ISR(&s_lock);
        return false;
    }
    uint64_t now = event->count_value;
    uint64_t delay = now > event->alarm_value ? now - event->alarm_value : 0;
    if (delay > s_late_max) s_late_max = (uint32_t)delay;
    uint64_t next;
    if (s_open) {
        AGC_CTRL |= AGC_HOLD;
        uint64_t duration = now - s_opened_at;
        if (duration > s_open_max) s_open_max = (uint32_t)duration;
        s_open = false;
        /* A late callback skips missed periods, never emits a catch-up burst. */
        if (s_next_open <= now)
            s_next_open += ((now - s_next_open) / PERIOD_US + 1) * PERIOD_US;
        next = s_next_open;
    } else {
        AGC_CTRL &= ~AGC_HOLD;
        s_open = true;
        s_opened_at = now;
        ++s_opens;
        s_next_open += PERIOD_US;
        next = now + WINDOW_US;
    }
    gptimer_alarm_config_t alarm = {.alarm_count = next};
    if (gptimer_set_alarm_action(timer, &alarm) != ESP_OK) {
        ++s_faults;
        s_running = false;
        AGC_CTRL &= ~AGC_HOLD;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    return false;
}

void c5vrx4_suspend(void)
{
    /* Direct Gain owns the hold state. No timer/register writes in this mode. */
    if (!rf_native_agc_active()) return;
    if (!s_transition_lock) return;
    xSemaphoreTakeRecursive(s_transition_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    ++s_suspend_depth;
    s_running = false;
    AGC_CTRL &= ~AGC_HOLD;
    portEXIT_CRITICAL(&s_lock);
    if (s_timer) (void)gptimer_stop(s_timer);
}

void c5vrx4_resume(void)
{
    /* Direct Gain owns the hold state. No timer/register writes in this mode. */
    if (!rf_native_agc_active()) return;
    if (!s_transition_lock) return;
    portENTER_CRITICAL(&s_lock);
    if (s_suspend_depth) --s_suspend_depth;
    /* The acquisition mask needs every native re-acquisition: no pacing. */
    bool start = s_timer && s_requested && rf_native_agc_active() &&
                 !c5vrx4_agc_mask_active() && !s_suspend_depth && !s_running;
    portEXIT_CRITICAL(&s_lock);
    if (!start) {
        xSemaphoreGiveRecursive(s_transition_lock);
        return;
    }
    /* Operator-selected native acquisition profile 127, no gain index write. */
    volatile uint32_t *speed = (volatile uint32_t *)0x600a7034u;
    *speed = (*speed & ~(0x7fu << 24)) | (127u << 24);
    ESP_ERROR_CHECK(gptimer_set_raw_count(s_timer, 0));
    gptimer_alarm_config_t alarm = {.alarm_count = WINDOW_US};
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &alarm));
    portENTER_CRITICAL(&s_lock);
    AGC_CTRL &= ~AGC_HOLD;
    s_running = s_open = true;
    s_opened_at = 0;
    s_next_open = PERIOD_US;
    ++s_opens;
    portEXIT_CRITICAL(&s_lock);
    ESP_ERROR_CHECK(gptimer_start(s_timer));
    xSemaphoreGiveRecursive(s_transition_lock);
}

static void print_state(void)
{
    c5vrx4_lane_print();
    portENTER_CRITICAL(&s_lock);
    bool running = s_running, open = s_open;
    uint32_t opens = s_opens, faults = s_faults;
    uint32_t late = s_late_max, duration = s_open_max;
    uint32_t control = AGC_CTRL;
    portEXIT_CRITICAL(&s_lock);
    printf("C5VRX4 pipeline=unwrap75_%s span_ns=75 phase_bits=8 winding=quadrant3 bound_step_bins=63 dac_delta_bits=6 iq_bits=4+4 "
           "iq_hz=40000000 dac_hz=40000000 unique_hz=13333333 "
           "gain_owner=%s pace=%d acquiring=%d period_us=%u window_us=%u "
           "opens=%" PRIu32 " late_max_us=%" PRIu32 " open_max_us=%" PRIu32
           " faults=%" PRIu32 " ctrl=0x%08" PRIx32 "\n",
           c5vrx4_history_enabled() ? "history" : "static",
           rf_native_agc_active() ? "native" : "direct_gain_v5",
           running, open, PERIOD_US, WINDOW_US, opens, late, duration, faults,
            control);
    static const char *const slopes[] = {"0.150", "full_span", "0.150"};
    static const unsigned blanks[] = {310u, 0u, 300u};
    unsigned mode = c5vrx4_cvbs_mode();
    printf("C5VRX4_CVBS transfer=%s reference_mv=%u volts_per_mhz=%s "
           "calibration=%s load_ohms=75 level_lab=%u "
           "sync_repair=0 keys=M_cycle_reboot,J_snapshot\n",
           c5vrx4_cvbs_mode_name(),
           mode == C5VRX4_CVBS_LEGACY ? (unsigned)(c5v4_dac_uv[c5v4_dac_legacy_codes[32]] / 1000u) :
                                        blanks[mode],
           slopes[mode],
           C5V4_DAC_MEASURED ? "measured" : "nominal", c5vrx4_level_enabled());
    bool fixed = c5vrx4_fixed_lane() != C5VRX4_LANE_ADAPTIVE;
    printf("C5VRX4_LANES policy=%s lane=%u adc_step=%u window_codes=%u "
           "runtime_switching=%d fold_guard=%s\n", c5vrx4_lane_mode_name(),
           rf_get_iq_lanes(), 64u >> rf_get_iq_lanes(),
           512u >> rf_get_iq_lanes(), !fixed,
           fixed ? "fixed_lane_severe_clip_g20" : "baseline");
}

void c5vrx4_start(void)
{
    /* V5 owns gain in the default build: do not allocate/start a native gate
     * or touch its control/profile registers in this mode. */
    if (!rf_native_agc_active()) {
        print_state();
        return;
    }
    gptimer_config_t config = {.clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_timer));
    gptimer_event_callbacks_t callbacks = {.on_alarm = gate_alarm};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &callbacks, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));
    s_transition_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(s_transition_lock ? ESP_OK : ESP_ERR_NO_MEM);
    c5vrx4_suspend();
    c5vrx4_resume();
    print_state();
}

bool c5vrx4_console(int key)
{
    if (key == 'u') {
        nvs_handle_t handle;
        bool enabled = !c5vrx4_level_enabled();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "level_lab", enabled ? 1 : 0);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 level_lab_next=%u err=%s action=%s\n", enabled,
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
        return true;
    }
    if (key == 'M') {
        /* STD150 -> CVBS150 -> LEGACY_FULL -> STD150. */
        static const uint8_t next_mode[] = {C5VRX4_CVBS_150, C5VRX4_CVBS_STD150,
                                            C5VRX4_CVBS_LEGACY};
        static const char *const next_name[] = {"CVBS150", "STD150", "LEGACY_FULL"};
        nvs_handle_t handle;
        unsigned mode = c5vrx4_cvbs_mode();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "cvbs_legacy", next_mode[mode]);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 cvbs_next=%s err=%s action=%s\n",
               next_name[mode], esp_err_to_name(err),
               err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
        return true;
    }
    if (key == '%') return toggle_flag("dc_recenter", c5vrx4_dc_recenter_enabled(), "dc_recenter");
    if (key == '&') return toggle_flag("sphase_auto", c5vrx4_sphase_auto_enabled(), "sphase_auto");
    if (key == '^') return toggle_flag("fixed_bw", c5vrx4_fixed_bw_enabled(), "fixed_bw");
    if (key == '|') return toggle_flag("agc_mask", c5vrx4_agc_mask_enabled(), "agc_mask");
    if (key == '_') return toggle_flag("idle_raster", c5vrx4_idle_raster_enabled(), "idle_raster");
    if (key == 'y') return toggle_flag("radius_boost", c5vrx4_radius_boost_enabled(), "radius_boost");
    if (key == 'w') return toggle_flag("sync_fw", c5vrx4_sync_flywheel_enabled(), "sync_fw");
    if (key == 'Z') {
        /* Fixed fine -> fixed ultrafine -> protected V5 lanes -> fixed fine. */
        static const char *const next_name[] = {"fixed_ultrafine", "protected_v5", "fixed_fine"};
        nvs_handle_t handle;
        uint8_t mode = c5vrx4_lane_mode();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "lane_mode", (uint8_t)((mode + 1u) % 3u));
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 lanes_next=%s err=%s action=%s\n", next_name[mode],
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_restart();
        }
        return true;
    }
    if (key == 'h') {
        nvs_handle_t handle;
        bool enabled = !c5vrx4_history_enabled();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "unwrap_hc", enabled ? 1 : 0);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 history_next=%d err=%s action=%s\n", enabled,
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_restart();
        }
        return true;
    }
    if (key == 'T') {
        print_state();
        return false; /* Also print the ordinary receiver diagnostics. */
    }
    if (key == '~' && !rf_native_agc_active()) {
        printf("C5VRX4 pace_toggle=ignored gain_owner=direct_gain_v5 "
               "hint=N_selects_native_on_reboot\n");
        return true;
    }
    if (!s_transition_lock) return false;
    if (key == '~') {
        c5vrx4_suspend();
        if (s_requested) {
            s_requested = false;
        } else {
            s_requested = true;
        }
        c5vrx4_resume();
        print_state();
        return true;
    }
    return false;
}
