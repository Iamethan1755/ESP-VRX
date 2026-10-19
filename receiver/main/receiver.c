/* C5VRX-5 by Twotoz and the C5VRX contributors: Direct Gain V5 runtime.
 * A 200 us timer wakes the observer (one fresh completed RX descriptor per
 * window, never the one GDMA is writing) and a sentinel that catches a
 * clipped envelope between observer windows. Only this file writes gain. */
#include "receiver.h"
#include <stdio.h>
#include <string.h>
#include "cn_estimate.h"
#include "esp_timer.h"
#include "gain_map_preset.h"
#include "phase8_gain_lut.h"
#include "phy.h"
#include "rf.h"
#include "soc/ahb_dma_struct.h"

direct_gain_t s_gain;
volatile uint8_t s_current_gain = 62u;
volatile uint32_t s_gain_epoch, s_control_epoch;
volatile int64_t s_last_gain_write_us;
volatile int s_obs_p50, s_obs_p95, s_obs_origin_pm, s_obs_clip_pm, s_obs_coherence;
volatile bool s_obs_stuck;
volatile int s_obs_env_ratio_x100;
volatile uint32_t s_obs_us_max, s_obs_windows;
dg_map_blob_t s_gain_map_saved;
bool s_gain_map_saved_valid;

static TaskHandle_t s_observer, s_sentinel;
static volatile bool s_paused;
static volatile uint32_t s_overload_state;   /* 0 free, 1 writing, 2 published */
static dg_observation_t s_overload;
static uint32_t s_overload_gain_epoch, s_overload_control_epoch, s_overload_phy;
static uint32_t s_map_imports, s_preset_imports;

bool receiver_active(void)
{
    return !s_paused && !s_menu_active && !phy_busy();
}

bool receiver_paused(void) { return s_paused; }

bool receiver_pause(const char *tag)
{
    if (s_paused) {
        printf("%s refused=busy\n", tag);
        return false;
    }
    s_paused = true;
    vTaskDelay(pdMS_TO_TICKS(20));   /* let an in-flight window finish */
    return true;
}

void receiver_resume(void)
{
    ++s_control_epoch;
    s_paused = false;
}

/* Gain write by the owner of a pause (calibration, scan). */
void receiver_set_gain(uint8_t gain)
{
    const arc_gain_table_t *t = rf_gain_table();
    if (gain < 2u) gain = 2u;
    if (gain > t->max_index) gain = t->max_index;
    rf_force_gain(gain);
    s_current_gain = gain;
    s_last_gain_write_us = esp_timer_get_time();
    ++s_gain_epoch;
}

static void apply_target(uint8_t target, uint32_t control, uint32_t phy)
{
    if (phy != phy_generation() || phy_busy() || control != s_control_epoch || !receiver_active()) return;
    if (target == s_current_gain) return;
    const arc_gain_table_t *t = rf_gain_table();
    if (target < 2u) target = 2u;
    if (target > t->max_index) target = t->max_index;
    /* A busy or changed PHY must not leave software ahead of hardware. */
    if (!rf_set_gain(target, phy)) {
        dg_sync_applied(&s_gain, s_current_gain, (uint64_t)esp_timer_get_time());
        return;
    }
    s_current_gain = target;
    s_last_gain_write_us = esp_timer_get_time();
    ++s_gain_epoch;
    dg_sync_applied(&s_gain, target, (uint64_t)s_last_gain_write_us);
}

int receiver_obs_cn_x10(void)
{
    /* Envelope model (cn_estimate.h); a fixed IQ point has no envelope to
     * judge and read 14-20 dB in the coherence model with no VTX at all. */
    return s_obs_stuck ? CN_UNKNOWN_X10 : cn_estimate_env_x10(s_obs_env_ratio_x100, calibration_filter_class_mhz());
}

static void tick_cb(void *arg)
{
    (void)arg;
    if (s_sentinel) xTaskNotifyGive(s_sentinel);
    if (s_observer) xTaskNotifyGive(s_observer);
}

/* 64 bytes from the middle of the newest completed descriptor every 200 us:
 * a clipped envelope requests the observer's emergency path at once. */
static void sentinel_task(void *arg)
{
    (void)arg;
    uint8_t sample[64];
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!receiver_active() || s_overload_state || s_rx_dma_ch < 0 || s_rx_node_count < 2) continue;
        uint32_t gain_epoch = s_gain_epoch, control = s_control_epoch, phy = phy_generation();
        int active = ring_node_index(AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val);
        if (active < 0) continue;
        int idx = (active - 1 + s_rx_node_count) % s_rx_node_count;
        uint8_t *src = s_rx_nodes[idx].buffer;
        if (!src || s_rx_nodes[idx].length < CONTROL_BYTES) continue;
        sync_dma_m2c(src + 1984u, sizeof(sample));
        memcpy(sample, src + 1984u, sizeof(sample));
        if (ring_node_index(AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val) == idx ||
            gain_epoch != s_gain_epoch) continue;
        dg_observation_t o = dg_measure(sample, sizeof(sample), c5vrx_phase8_gain_lut,
                                        (uint64_t)esp_timer_get_time());
        if (o.clip_pm < 125u && o.p95 < 95u) continue;
        if (control != s_control_epoch || phy != phy_generation() || phy_busy()) continue;
        if (!__sync_bool_compare_and_swap(&s_overload_state, 0u, 1u)) continue;
        s_overload = o;
        s_overload_gain_epoch = gain_epoch;
        s_overload_control_epoch = control;
        s_overload_phy = phy;
        __sync_synchronize();
        __sync_lock_test_and_set(&s_overload_state, 2u);
        if (s_observer) xTaskNotifyGive(s_observer);
    }
}

static void observer_task(void *arg)
{
    (void)arg;
    uint8_t sample[PROBE_BYTES];
    uint32_t seen_control = UINT32_MAX, seen_table = UINT32_MAX, seen_phy = UINT32_MAX;
    bool was_active = false;
    int last_node = -1;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        if (!receiver_active()) {
            was_active = false;
            if (__sync_bool_compare_and_swap(&s_overload_state, 2u, 1u))
                __sync_lock_release(&s_overload_state);
            continue;
        }
        uint32_t control = s_control_epoch, table = rf_table_generation(), phy = phy_generation();
        if (!was_active || control != seen_control || table != seen_table || phy != seen_phy ||
            s_gain.current_gain != s_current_gain) {
            dg_reset(&s_gain, rf_gain_table(), s_current_gain);
            /* A fresh boot starts from the persisted map, not the model. */
            if (!s_gain.learned && s_gain_map_saved_valid && dg_import_map(&s_gain, &s_gain_map_saved))
                ++s_map_imports;
            else if (!s_gain.learned && GAIN_MAP_PRESET_BOARDS &&
                     dg_import_preset(&s_gain, c5vrx5_gain_map_preset_q10, GAIN_MAP_PRESET_MAX_INDEX + 1u,
                                      GAIN_MAP_PRESET_MAX_INDEX, GAIN_MAP_PRESET_BAND5))
                ++s_preset_imports;
            seen_control = control;
            seen_table = table;
            seen_phy = phy;
            was_active = true;
        }
        if (__sync_bool_compare_and_swap(&s_overload_state, 2u, 1u)) {
            __sync_synchronize();
            dg_observation_t o = s_overload;
            bool current = s_overload_gain_epoch == s_gain_epoch &&
                           s_overload_control_epoch == control && s_overload_phy == phy &&
                           (uint64_t)esp_timer_get_time() - o.observed_us <= 1000u;
            __sync_lock_release(&s_overload_state);
            if (current) apply_target(dg_tick(&s_gain, &o), control, phy);
            continue;
        }
        uint32_t gain_epoch = s_gain_epoch;
        int node = -1;
        if (!copy_probe_window(sample, &node) || gain_epoch != s_gain_epoch) continue;
        /* Never the same descriptor twice: settling counts fresh windows. */
        if (node == last_node) continue;
        last_node = node;
        const int64_t t0 = esp_timer_get_time();
        dg_observation_t o = dg_measure(sample, sizeof(sample), c5vrx_phase8_gain_lut, (uint64_t)t0);
        s_obs_p50 = o.p50;
        s_obs_p95 = o.p95;
        s_obs_origin_pm = o.origin_pm;
        s_obs_clip_pm = o.clip_pm;
        s_obs_coherence = o.coherence;
        s_obs_stuck = dg_stuck(&o);
        s_obs_env_ratio_x100 = o.env_ratio_x100;
        if (phy != phy_generation() || phy_busy()) continue;
        /* Learn the gain map only from a real VTX: burst-confirmed sync seen
         * by the AFC within the last 500 ms (never from receiver noise). */
        const int64_t video = s_last_video_us;
        s_gain.learn_enabled = video != 0 && t0 - video < 500000;
        apply_target(dg_tick(&s_gain, &o), control, phy);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_obs_us_max) s_obs_us_max = us;
        ++s_obs_windows;
    }
}

void receiver_start(void)
{
    s_gain_map_saved_valid = blob_load("gain_map", &s_gain_map_saved, sizeof(s_gain_map_saved)) &&
                             s_gain_map_saved.version == DG_MAP_VERSION &&
                             s_gain_map_saved.freq_mhz == rf_frequency_mhz();
    ESP_ERROR_CHECK(xTaskCreate(observer_task, "gain_obs", 4096, NULL, 3, &s_observer) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(sentinel_task, "gain_fast", 3072, NULL, 4, &s_sentinel) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t args = {.callback = tick_cb, .dispatch_method = ESP_TIMER_TASK, .name = "gain_tick"};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 200));
}

void receiver_print(void)
{
    printf("GAIN owner=direct_gain_v5 gain=%u state=%u p50=%d p95=%d coherence=%d env_x100=%d stuck=%u cn_x10=%d origin_pm=%d clip_pm=%d "
           "writes=%lu holds=%lu overloads=%lu bursts=%lu fade_holds=%lu damp=%lu map_learned=%lu learn_refused=%lu "
           "map_imports=%lu preset_imports=%lu "
           "obs_windows=%lu obs_us_max=%lu\n",
           s_current_gain, (unsigned)s_gain.state, s_obs_p50, s_obs_p95, s_obs_coherence, s_obs_env_ratio_x100,
           (unsigned)s_obs_stuck, receiver_obs_cn_x10(),
           s_obs_origin_pm, s_obs_clip_pm, (unsigned long)s_gain.writes, (unsigned long)s_gain.holds,
           (unsigned long)s_gain.overloads, (unsigned long)s_gain.bursts_ignored, (unsigned long)s_gain.fade_holds,
           (unsigned long)s_gain.damp_events, (unsigned long)s_gain.learned, (unsigned long)s_gain.learn_refused,
           (unsigned long)s_map_imports, (unsigned long)s_preset_imports, (unsigned long)s_obs_windows,
           (unsigned long)s_obs_us_max);
}

/* Console M: the learned map on one line, for gain_map_preset.py. */
void receiver_print_map(void)
{
    dg_map_blob_t blob;
    unsigned n = dg_export_map(&s_gain, &blob);
    printf("GAINMAP v=%u max=%u band5=%u freq=%u entries=%u", DG_MAP_VERSION, blob.max_index, blob.band5,
           rf_frequency_mhz(), n);
    for (unsigned k = 0; k <= blob.max_index && k < DG_STATES; ++k)
        if (blob.confidence[k] && blob.power_q10[k])
            printf(" %u:%u:%u:%u", k, blob.power_q10[k], blob.confidence[k], blob.uncertainty_pm[k]);
    printf("\n");
}
