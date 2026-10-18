/* SPDX-License-Identifier: GPL-3.0-only */
/* Wi-Fi access point, web page and MJPEG stream of VRX Bridge. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t stream_start(void);
/* Hands a finished grayscale frame to the JPEG encoder without waiting.
 * Returns false while the encoder is still busy with the previous one;
 * after true the caller must not touch `fb` until the next call succeeds. */
bool stream_submit(const uint8_t *fb, int w, int h);
void stream_stats(float *fps, uint32_t *jpeg_bytes, uint32_t *frames, int *clients);
/* Temporal denoise before JPEG (on by default). */
void stream_set_denoise(bool on);
bool stream_denoise(void);
/* Devices joined to the bridge's Wi-Fi network. */
int stream_wifi_stations(void);
/* Console report: network name, channel, address, power, joined devices. */
void stream_print_wifi(void);
/* Copies the latest JPEG; returns its size (0 if none or too big). */
size_t stream_copy_jpeg(uint8_t *dst, size_t max);
/* Copies the latest JPEG only if it is newer than *seq (then updates *seq);
 * returns 0 when there is no new frame. */
size_t stream_copy_jpeg_new(uint8_t *dst, size_t max, uint32_t *seq);
