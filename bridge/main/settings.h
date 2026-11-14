/* SPDX-License-Identifier: GPL-3.0-only */
/* Picture settings of ESP-VRX that outlive a power cycle (NVS): the picture
 * position, levels, despeckle, colour and denoise, as last set from the page
 * or the console. The Wi-Fi name, pins and rates are build settings
 * (menuconfig) and not stored here. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int8_t h_tenths;   /* picture shift, 0.1 us steps (video_shift_h) */
    int8_t v_lines;    /* picture shift, lines (video_shift_v) */
    uint8_t levels;    /* video_levels_t */
    uint8_t filter;    /* video_filter_t */
    uint8_t colour;    /* 0 gray, 1 colour */
    uint8_t denoise;   /* 0 off, 1 on */
} settings_t;

/* Brings up NVS (also needed by the Wi-Fi driver) and the task that writes
 * the settings. Call once, before settings_load() and before Wi-Fi starts. */
esp_err_t settings_init(void);
/* The stored settings; false (and *s untouched) when none are stored or
 * they come from a different layout. */
bool settings_load(settings_t *s);
/* Remembers `s` and returns at once (safe from the decoder task). It is
 * written to flash 2 s after the last call, and only if it changed: a flash
 * write stalls both cores for a few ms (the decoder then skips some lines
 * once), so taps on the arrow buttons are saved as one. */
void settings_save(const settings_t *s);
/* Forgets the stored settings: the next start uses the firmware's defaults. */
void settings_erase(void);
