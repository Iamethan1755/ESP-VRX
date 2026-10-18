/* Motion-adaptive temporal denoise, see denoise.h. */
#include "denoise.h"
#include <string.h>

void denoise_frame(uint8_t *acc, const uint8_t *in, int w, int h, bool first)
{
    /* Row by row through internal buffers: both frames live in PSRAM. */
    uint8_t cur[DENOISE_W_MAX], prev[DENOISE_W_MAX];
    if (w > DENOISE_W_MAX) return;
    for (int y = 0; y < h; y++) {
        uint8_t *a = acc + (size_t)y * (size_t)w;
        memcpy(cur, in + (size_t)y * (size_t)w, (size_t)w);
        if (!first) {
            memcpy(prev, a, (size_t)w);
            for (int x = 0; x < w; x++) {
                const int d = cur[x] - prev[x];
                if (d <= DENOISE_MOTION && d >= -DENOISE_MOTION) cur[x] = (uint8_t)((cur[x] + prev[x] + 1) >> 1);
            }
        }
        memcpy(a, cur, (size_t)w);
    }
}
