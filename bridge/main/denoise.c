/* SPDX-License-Identifier: GPL-3.0-only */
/* Motion-adaptive temporal denoise, see denoise.h. */
#include "denoise.h"
#include <string.h>

void denoise_row(uint8_t *acc, uint8_t *row, int w, bool first)
{
    if (!first) {
        uint8_t prev[DENOISE_W_MAX];   /* acc lives in PSRAM: read it in one go */
        memcpy(prev, acc, (size_t)w);
        for (int x = 0; x < w; x++) {
            const int d = row[x] - prev[x];
            if (d <= DENOISE_MOTION && d >= -DENOISE_MOTION) row[x] = (uint8_t)((row[x] + prev[x] + 1) >> 1);
        }
    }
    memcpy(acc, row, (size_t)w);
}

void denoise_frame(uint8_t *acc, const uint8_t *in, int w, int h, bool first)
{
    /* Row by row through internal buffers: both frames live in PSRAM. */
    uint8_t cur[DENOISE_W_MAX];
    if (w > DENOISE_W_MAX) return;
    for (int y = 0; y < h; y++) {
        memcpy(cur, in + (size_t)y * (size_t)w, (size_t)w);
        denoise_row(acc + (size_t)y * (size_t)w, cur, w, first);
    }
}
