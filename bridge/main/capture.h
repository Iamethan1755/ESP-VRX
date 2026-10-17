/* Capture of the C5's 6-bit video bus with the ESP32-S3 camera port
 * (LCD_CAM in camera mode, sample clock generated internally) into a
 * cyclic DMA ring in internal SRAM. No CPU in the sample path. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CAPTURE_RING_BYTES (1u << 16)  /* power of two: 6.5 ms at 10 MS/s */
#define CAPTURE_NODE_BYTES 2048u       /* one DMA descriptor, one EOF */

esp_err_t capture_init(uint32_t fs_hz);
uint8_t *capture_ring(void);
esp_err_t capture_start(void);
void capture_stop(void);
bool capture_running(void);
/* Samples written since capture_start(), in whole DMA nodes. */
uint32_t capture_write_pos(void);
/* Changes on every (re)start, when capture_write_pos() begins again at 0. */
uint32_t capture_generation(void);
/* Task notified (xTaskNotifyGive) each time a DMA node completes. */
void capture_set_notify(TaskHandle_t task);
/* Loopback test: drives the six data pins from the S3 itself and checks
 * what the camera port records, then the sample rate. Only with nothing
 * (no C5) connected to the data pins. */
esp_err_t capture_selftest(char *out, size_t len);
/* Bring-up diagnostics: clock pad activity, bytes the DMA wrote into a
 * marked ring, descriptor and register state. Safe with the C5 attached. */
void capture_debug(char *out, size_t len);
