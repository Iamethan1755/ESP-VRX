/* C5VRX-5 by Twotoz and the C5VRX contributors: built-in gain map preset.
 *
 * Starting point for Direct Gain on a board without a saved map (first boot,
 * another channel, or after a map version change). Relative power per
 * vendor gain index (Q10, any common scale; 0 = unknown), taken from MEASURED
 * boards: console M prints a board's learned map, and
 * tools/gain_map_preset.py turns one or more such logs into this table
 * (median of the ratios). It is imported with the lowest confidence and a
 * high uncertainty, so the first real transitions with a confirmed VTX
 * replace it. Not simulated: the dB value of the RF stages and BB codes is
 * not known well enough to model.
 *
 * Empty until board data is collected: then nothing is imported and Direct
 * Gain starts as before (local fine-step prior). */
#pragma once
#include <stdint.h>

#define GAIN_MAP_PRESET_MAX_INDEX 83u
#define GAIN_MAP_PRESET_BAND5     1u
#define GAIN_MAP_PRESET_BOARDS    0u   /* boards the table was built from */
static const uint16_t c5vrx5_gain_map_preset_q10[GAIN_MAP_PRESET_MAX_INDEX + 1u] = {0};
