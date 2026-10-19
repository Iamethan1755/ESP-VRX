/* C5VRX-5 by Twotoz and the C5VRX contributors: carrier-to-noise models.
 * Runtime decisions use cn_estimate_env_x10 below. The historical model
 * is retained for comparison tests: it uses phase coherence (share of
 * adjacent pairs with power >= 8 and a phase step within 45 degrees).
 *
 * The same coherence means a different C/N per analog filter: 4-bit
 * receiver noise alone reads ~42 % through the ~20 MHz chip channel but
 * ~51 / 58 / 61 % through the 16 / 12 / 10 MHz classes, because the narrower noise
 * is more correlated from sample to sample. A fixed coherence threshold is
 * therefore wrong as soon as the filter class changes.
 *
 * The tables come from the C5VRX-5 chain model (research/sim.py: FM PAL
 * video, deviation 1.0, Direct Gain radius, 4-bit ultrafine, firmware LUT;
 * 4092-byte windows, median over 3 seeds; reproduce with
 * research/afc_bias.py --coherence). C/N is per 20 MHz channel, as in
 * research/README.md (usable-picture threshold ~1-5 dB). A model
 * estimate for decisions and diagnostics, not a calibrated RF measurement. */
#pragma once
#include <stdint.h>

#define CN_UNKNOWN_X10 (-990)   /* no usable carrier-to-noise evidence */

/* filter_mhz: 10, 12, 16 or 20 (uncalibrated chip channel); others use the
 * nearest. Returns C/N x10 dB, -3..20 dB (slightly negative just above
 * the noise-only point); CN_UNKNOWN_X10 when the coherence is noise-like. */
static inline int cn_estimate_x10(int coherence, int filter_mhz)
{
    /* C/N (dB) :          0     2     4     6     8    10    15    20 */
    static const int16_t c20[] = {572, 632, 700, 772, 842, 902, 986, 1000};
    static const int16_t c16[] = {650, 704, 764, 828, 888, 936, 994, 1000};
    static const int16_t c12[] = {705, 756, 814, 868, 918, 957, 997, 1000};
    static const int16_t c10[] = {732, 782, 838, 890, 936, 968, 998, 1000};
    static const int16_t cn[] = {0, 20, 40, 60, 80, 100, 150, 200};
    const int16_t *t = filter_mhz <= 11 ? c10 : filter_mhz <= 14 ? c12 : filter_mhz <= 18 ? c16 : c20;
    const int noise = filter_mhz <= 11 ? 610 : filter_mhz <= 14 ? 577 : filter_mhz <= 18 ? 508 : 419;   /* noise only */
    int c = coherence * 10;
    if (c <= noise + 20) return CN_UNKNOWN_X10;
    if (c <= t[0]) return -1 - (t[0] - c) * 30 / (t[0] - noise);   /* below 0 dB: -0.1..-3 dB */
    for (unsigned k = 1; k < 8u; ++k)
        if (c <= t[k]) return cn[k - 1] + (c - t[k - 1]) * (cn[k] - cn[k - 1]) / (t[k] - t[k - 1]);
    return 200;
}

/* C/N from envelope constancy (gain.c dg_measure env_ratio_x100): with the
 * window DC removed, |z|^2 of an FM carrier plus noise has mean^2/var
 * (c+1)^2/(2c+1); noise alone 1 (x100: 100; 114-116 after the 4-bit
 * quantiser). It avoids the phase-step model's direct dependence on FM
 * swings. Residual filter/quantiser effects remain: sim.py FM PAL with
 * deviation 0.69 / 1.0 / 1.4 differs by up to ~1.1 dB through 12 dB C/N
 * (research/cn_envelope.py, 2 seeds, 256-byte probes as
 * copy_probe_window). The coherence model read 9-13 dB next to the VTX on
 * the board (walks 2026-10-13). Tables: deviation 1.0 medians, C/N per
 * 20 MHz channel; above ~15 dB the ratio flattens (filter AM, quantiser). */
#define CN_ENV_NOISE_X100 130   /* at or below: noise-like (0 dB reads 140-177) */
static inline int cn_estimate_env_x10(int ratio_x100, int filter_mhz)
{
    /* C/N (dB) :          0     2     4     6     8    10    12    15    20 */
    static const int16_t e20[] = {140, 161, 197, 255, 342, 464, 629, 929, 1419};
    static const int16_t e16[] = {150, 178, 225, 298, 403, 542, 730, 1066, 1501};
    static const int16_t e12[] = {166, 204, 262, 355, 479, 644, 846, 1167, 1543};
    static const int16_t e10[] = {177, 222, 294, 395, 532, 707, 928, 1208, 1522};
    static const int16_t cn[] = {0, 20, 40, 60, 80, 100, 120, 150, 200};
    const int16_t *t = filter_mhz <= 11 ? e10 : filter_mhz <= 14 ? e12 : filter_mhz <= 18 ? e16 : e20;
    if (ratio_x100 <= CN_ENV_NOISE_X100) return CN_UNKNOWN_X10;
    if (ratio_x100 <= t[0]) return -1 - (t[0] - ratio_x100) * 30 / (t[0] - CN_ENV_NOISE_X100);
    for (unsigned k = 1; k < 9u; ++k)
        if (ratio_x100 <= t[k]) return cn[k - 1] + (ratio_x100 - t[k - 1]) * (cn[k] - cn[k - 1]) / (t[k] - t[k - 1]);
    return 200;
}

/* AFC re-acquisition may retune only on a carrier this strong: below it the
 * porch estimate is pulled toward the receiver centre (FM clicks) and a
 * correction would step the wrong way (afc_bias.py: +65..250 kHz bias at
 * 0-2 dB, < 20 kHz from 6 dB, against a 50 kHz deadband). The VTX carrier
 * does not move during a fade; the offset found on a strong signal stays. */
#define AFC_MIN_CN_X10 60
static inline int afc_correction_allowed(int coherence, int filter_mhz)
{
    return cn_estimate_x10(coherence, filter_mhz) >= AFC_MIN_CN_X10;
}
