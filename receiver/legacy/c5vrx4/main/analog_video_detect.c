#include "analog_video_detect.h"

#define AVD_LAG_MIN   1266
#define AVD_LAG_MAX   1285

analog_video_t analog_video_detect(uint8_t *ep, size_t n, const uint8_t phase[256])
{
    analog_video_t out = {0, 0, 0, 0};
    if (!ep || !phase || n < (size_t)AVD_LAG_MAX + 257u) return out;
    /* In place: ep[k] becomes the wrapped 50 ns phase step into endpoint
     * k+1 (a signed byte). No extra memory. */
    uint8_t prev = phase[ep[0]];
    for (size_t k = 0; k + 1u < n; ++k) {
        uint8_t p = phase[ep[k + 1u]];
        ep[k] = (uint8_t)(p - prev);
        prev = p;
    }
    --n;
    const int8_t *d = (const int8_t *)ep;

    int32_t sum = 0;
    for (size_t k = 0; k < n; ++k) sum += d[k];
    int32_t mean_q4 = (int32_t)((sum * 16) / (int32_t)n);
    /* One Phase8 bin per 50 ns = 20 MHz / 256 = 78.125 kHz. */
    out.offset_khz = (int)((int64_t)mean_q4 * 78125 / 16000);
    int64_t var = 0;
    for (size_t k = 0; k < n; ++k) {
        int32_t x = d[k] * 16 - mean_q4;
        var += (int64_t)x * x;
    }
    var /= (int64_t)n;
    if (var <= 0) return out;

    int best = -1000, best_lag = 0;
    for (int lag = AVD_LAG_MIN; lag <= AVD_LAG_MAX; ++lag) {
        int64_t cov = 0;
        size_t m = n - (size_t)lag;
        for (size_t k = 0; k < m; ++k)
            cov += (int64_t)(d[k] * 16 - mean_q4) * (d[k + (size_t)lag] * 16 - mean_q4);
        cov /= (int64_t)m;
        int rho = (int)(cov * 100 / var);
        if (rho > best) { best = rho; best_lag = lag; }
    }
    out.confidence = best;
    out.lag = best_lag;
    out.standard = best_lag >= 1277 ? 1 : 2;
    return out;
}
