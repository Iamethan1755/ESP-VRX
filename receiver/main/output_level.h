/* C5VRX by Twotoz and contributors: issue #200 manual DAC6 scaling.
 * 1x is bit-exact. High gains can clip; output impedance is not calibrated.
 * No phase/token bits may change, including when LP carries DAC through A. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define OUTPUT_LEVELS 4
static inline uint16_t output_entry(uint16_t value, unsigned level, bool *clipped)
{
    static const int pivot[OUTPUT_LEVELS] = {21, 13, 16, 19};
    if (level >= OUTPUT_LEVELS) level = 0;
    int twice = 2 * pivot[level] + ((int)level + 2) * ((int)(value & 63u) - 21);
    /* Round to nearest code, including negative values before saturation. */
    int code = twice >= 0 ? (twice + 1) / 2 : twice / 2;
    if (clipped) *clipped = code < 0 || code > 63;
    if (code < 0) code = 0;
    if (code > 63) code = 63;
    return (uint16_t)((value & 0xffc0u) | (unsigned)code);
}
