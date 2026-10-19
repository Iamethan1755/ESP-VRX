/* C5VRX by Twotoz and contributors: PASSIVE host prototype, no actuator.
 * Uses production AFC/Direct Gain helpers over captured Q4 windows. White
 * p99 is NOT guaranteed peak white; a conservative span reserve is included
 * for study only. A few lines cannot qualify output gain for future scenes. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "afc.h"
#include "dsp.h"
#include "gain.h"
#include "cn_estimate.h"
#include "phase8_gain_lut.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    int filter_mhz = atoi(argv[2]);
    uint8_t s[4096];
    unsigned window = 0;
    size_t n;
    while ((n = fread(s, 1, sizeof(s), f)) != 0) {
        afc2_result_t r = afc2_measure(s, n, c5vrx_phase8_gain_lut);
        dg_observation_t o = dg_measure(s, n, c5vrx_phase8_gain_lut, 0);
        int cn = cn_estimate_x10(o.coherence, filter_mhz);
        unsigned hist[255] = {0}, got = 0;
        afc2_smoother_t smoother = {0};
        for (size_t k = 1; k < n; ++k) {
            int v = afc2_smooth_push(&smoother, k, afc2_delta(s[k-1], s[k], c5vrx_phase8_gain_lut));
            if (k >= AFC2_SMOOTH && smoother.count * 4u >= AFC2_BOX1 * 3u) {
                ++hist[v + 127]; ++got;
            }
        }
        unsigned cumulative = 0;
        int peak = 127;
        for (int k = 0; k < 255; ++k) {
            cumulative += hist[k];
            if (cumulative * 1000u >= got * 999u) { peak = k - 127; break; }
        }
        double depth = r.porch_khz - r.sync_khz;
        double p999 = peak * 78.125;
        /* Do not treat a dark scene's low percentile as full-scale white.
         * This 5x sync-span reserve is a conservative hypothesis, not a bound
         * guaranteed by all VTXs/cameras. Never use this prototype to write. */
        double guard_peak = fmax(p999 + 500., r.porch_khz + 5 * depth + 500.);
        const char *why = n < 2048 ? "short" : !afc2_envelope_stationary(s,n) ? "nonstationary" :
            !r.lines || !r.standard || !r.sync_pairs || !r.porch_pairs || r.polarity != -1 ? "no_confirmed_sync" :
            cn < 80 ? "weak_or_noise" : o.clip_pm > 25 || o.origin_pm > 50 ? "iq_headroom" :
            depth < 500 || depth > 5000 || r.sync_khz <= -5500 || guard_peak >= 8500 ? "level_headroom" : NULL;
        double gain = 1, source_code = 21;
        if (!why) {
            source_code = (r.sync_khz + 6000.) * 63 / 15000.;
            gain = fmin(2.5, fmin(18 / (depth * 63 / 15000.), 57 / ((guard_peak-r.sync_khz)*63/15000.)));
        }
        printf("{\"window\":%u,\"samples\":%zu,\"usable\":%s,\"reason\":\"%s\","
               "\"lines\":%u,\"cn_model_x10\":%d,\"sync_khz\":%ld,\"porch_khz\":%ld,"
               "\"white_p999_khz\":%.2f,\"guard_peak_khz\":%.2f,\"candidate_gain\":%.5f,"
               "\"source_pivot_code\":%.5f,\"target_tip_code\":3}\n",
               window++,n,why?"false":"true",why?why:"candidate_only",r.lines,cn,
               (long)r.sync_khz,(long)r.porch_khz,p999,guard_peak,gain,source_code);
    }
    fclose(f);
    return 0;
}
