/**
 * main.c - C5VRX-3 application entry point.
 *
 * Ultra-minimal single-purpose FPV receiver.
 * Starts RF frontend, starts video pipeline, then exits.
 * The hardware runs forever; the application is done.
 */

#include "rf.h"
#include "video.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bs_relative_worker_probe.h"
#include "bs_relative_middle_probe.h"
#include "bs_addctia_probe.h"
#include "phy_phase_tap_probe.h"
#ifdef C5VRX4_EXPERIMENT
#include "c5vrx4.h"
#endif

void app_main(void)
{
#if CONFIG_C5VRX_BS_RELATIVE_WORKER_PROBE
    bs_relative_worker_probe_run();
#endif
#if CONFIG_C5VRX_BS_RELATIVE_MIDDLE_PROBE
    bs_relative_middle_probe_run();
#endif
#if CONFIG_C5VRX_BS_ADDCTIA_PROBE
    bs_addctia_probe_run();
#endif
    ESP_ERROR_CHECK(rf_start());
#if CONFIG_C5VRX_PHY_PHASE_TAP_PROBE
    /* Give the host time to reopen the USB console after reset, otherwise
     * the boot-only report is printed before anyone listens. */
    vTaskDelay(pdMS_TO_TICKS(8000));
    phy_phase_tap_probe_run();
#endif
    ESP_ERROR_CHECK(video_start());
#ifdef C5VRX4_EXPERIMENT
    c5vrx4_start();
#endif
    /* Hardware pipeline is running. Application has nothing more to do. */
}
