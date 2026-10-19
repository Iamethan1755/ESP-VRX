/* C5VRX by Twotoz and contributors: exact shared filter-measurement math.
 * No prior DC calibration is required; failed copies contribute no samples. */
#pragma once
#include "dsp.h"
typedef struct {
    float psd[DSP_FFT_N];
    unsigned got, env_sum;
    int clip_sum;
} cal_noise_t;
static inline void cal_noise_add(cal_noise_t *a, const uint8_t *s, size_t n, int clip_pm)
{
    if (!s || n != 256u) return;
    for (unsigned i = 0; i < 4u; ++i) dsp_psd_accumulate(s + i * DSP_FFT_N, a->psd);
    a->env_sum += dsp_envelope_ratio_x100(s, n);
    a->clip_sum += clip_pm;
    ++a->got;
}
static inline bool cal_noise_finish(const cal_noise_t *a, unsigned *width, unsigned *nbw,
                                    unsigned *env, int *clip)
{
    if (a->got < 48u) return false;
    *width = dsp_psd_width_khz(a->psd);
    *nbw = dsp_psd_nbw_khz(a->psd);
    *env = a->env_sum / a->got;
    *clip = a->clip_sum / (int)a->got;
    return true;
}
static inline const char *cal_channel_quiet(const uint8_t *s, unsigned windows, bool recent_sync,
                                          unsigned *env, int *clip)
{
    if (recent_sync) return "video_sync";
    if (windows < 16u) return "probe_failed";
    return dsp_quiet_reason(s, windows * 256u, env, clip);
}
static inline bool cal_setting_quiet(unsigned env, int clip) { return env <= dsp_quiet_limit(clip); }
static inline bool cal_setting_carrier(unsigned env, int clip)
{
    return env > dsp_quiet_limit(clip) + DSP_QUIET_CARRIER_X100;
}
