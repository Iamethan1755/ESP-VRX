#pragma once
#include "esp_err.h"

/**
 * video_start() - Initialize and start the PARLIO RX+TX realtime pipeline.
 *
 * Sets up:
 *   - PARLIO RX @ 40 MHz, POS edge, 8-bit, 32 KiB cyclic DMA ring
 *   - PARLIO TX @ 40 MHz, [D,D] output, loop_transmission
 *   - TX BitScrambler with embedded Phase5 LUT (fm.bsasm)
 *   - One-time RX start, then TX starts after a half-ring delay
 *
 * After video_start() returns ESP_OK, the CPU is done.
 * The hardware pipeline runs forever without any software involvement.
 *
 * NO periodic tasks, NO telemetry, NO calibration loading.
 */
esp_err_t video_start(void);
