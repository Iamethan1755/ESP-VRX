/* SPDX-License-Identifier: GPL-3.0-only */
/* Wi-Fi access point, web page and MJPEG stream of VRX Bridge. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

/* Frame buffers (VIDEO_W x VIDEO_H_MAX, 16-byte aligned): the one the
 * decoder renders into, the newest finished frame, and one per reader (the
 * JPEG encoder, with BRIDGE_SPI_DISPLAY also the screen). */
#ifdef CONFIG_BRIDGE_SPI_DISPLAY
#define STREAM_FRAMES 4
#else
#define STREAM_FRAMES 3
#endif

/* `frames`: STREAM_FRAMES buffers, [0] the one the decoder renders into first. */
esp_err_t stream_start(uint8_t *const *frames);
/* Hands a finished grayscale frame to the JPEG encoder (and the screen)
 * without waiting and returns the buffer to render the next frame into.
 * The newest frame wins: one no reader has taken yet is dropped (its
 * buffer comes back). */
uint8_t *stream_submit(uint8_t *fb, int w, int h);
#ifdef CONFIG_BRIDGE_SPI_DISPLAY
/* The screen's side of the frame mailbox, for the display task: after
 * stream_display_attach() every new frame notifies the calling task;
 * stream_display_take() returns the newest frame the screen has not had
 * (NULL if none). Hand it back with stream_display_release() once it is
 * read: until then the decoder does not render into it. */
void stream_display_attach(void);
const uint8_t *stream_display_take(int *w, int *h);
void stream_display_release(const uint8_t *fb);
#endif
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
