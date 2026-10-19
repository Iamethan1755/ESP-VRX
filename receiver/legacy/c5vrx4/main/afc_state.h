#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Issue #115 AFC state hygiene. This is NOT the AFC V2 estimator (a
 * burst-free blanking-interval CFO reference is still open); it only makes
 * the existing acquisition-only AUTO AFC safe to own state:
 *
 *  - a CFO sample is accepted only from a window with no gain/PHY transition
 *    and no settle period in progress;
 *  - any change of AFC mode, channel, profile, bandwidth, frequency offset or
 *    RF generation resets the filter, the sample count and the persistence;
 *  - a correction needs a minimum number of fresh trusted samples since the
 *    last reset, not just a persistence counter left over from another mode.
 */

#define AFC_MIN_FRESH_SAMPLES 8u
#define AFC_PERSIST_TICKS     20u
#define AFC_DEADBAND_KHZ      35
#define AFC_CFO_LIMIT_KHZ     2000

typedef struct {
    uint32_t context;
    int filtered_khz;
    uint16_t samples;
    uint16_t persistence;
} afc_state_t;

static inline uint32_t afc_context(unsigned afc_mode, unsigned channel,
                                   uint32_t profile_generation, bool bw40,
                                   int offset_khz, uint32_t rf_generation)
{
    uint32_t h = 2166136261u;
    const uint32_t parts[6] = {
        afc_mode, channel, profile_generation, bw40 ? 1u : 0u,
        (uint32_t)offset_khz, rf_generation,
    };
    for (unsigned i = 0; i < 6u; ++i) h = (h ^ parts[i]) * 16777619u;
    return h;
}

static inline void afc_reset(afc_state_t *s, uint32_t context)
{
    s->context = context;
    s->filtered_khz = 0;
    s->samples = 0;
    s->persistence = 0;
}

/* Returns true when the stored state was invalidated by a context change. */
static inline bool afc_sync_context(afc_state_t *s, uint32_t context)
{
    if (s->context == context) return false;
    afc_reset(s, context);
    return true;
}

/* A gain/PHY transition or settle period discards history entirely: an IIR
 * must never remember a transient as carrier offset. */
static inline void afc_invalidate(afc_state_t *s)
{
    s->filtered_khz = 0;
    s->samples = 0;
    s->persistence = 0;
}

static inline void afc_observe(afc_state_t *s, int instant_khz)
{
    if (instant_khz > AFC_CFO_LIMIT_KHZ) instant_khz = AFC_CFO_LIMIT_KHZ;
    if (instant_khz < -AFC_CFO_LIMIT_KHZ) instant_khz = -AFC_CFO_LIMIT_KHZ;
    s->filtered_khz = s->samples ?
        (s->filtered_khz * 7 + instant_khz) / 8 : instant_khz;
    if (s->samples < 0xFFFFu) ++s->samples;
}

/* One tick of the acquisition-only decision. `eligible` carries the caller's
 * lock/quality gate. Returns true when a correction of filtered_khz should
 * be written now; the caller then invalidates via a context change. */
static inline bool afc_tick(afc_state_t *s, bool eligible)
{
    bool outside = s->filtered_khz > AFC_DEADBAND_KHZ ||
                   s->filtered_khz < -AFC_DEADBAND_KHZ;
    if (!eligible || !outside || s->samples < AFC_MIN_FRESH_SAMPLES) {
        s->persistence = 0;
        return false;
    }
    if (++s->persistence < AFC_PERSIST_TICKS) return false;
    s->persistence = 0;
    return true;
}
