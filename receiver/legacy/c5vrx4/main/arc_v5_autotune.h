#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arc_v3_controller.h"

#define ARC_V5_GAIN_STATES (ARC_VENDOR_GAIN_MAX + 1u)
#define ARC_V5_MODEL_MAGIC 0x35564341u /* "ACV5" little-endian */
/* v2 invalidates early PR62 models learned before the proven 500 ms settle gate. */
#define ARC_V5_MODEL_VERSION 2u

typedef enum {
    ARC_V5_CONTEXT_NO_CARRIER = 0,
    ARC_V5_CONTEXT_WEAK,
    ARC_V5_CONTEXT_CLEAN,
    ARC_V5_CONTEXT_BLOCKER,
    ARC_V5_CONTEXT_OVERLOAD,
} arc_v5_context_t;

typedef enum {
    ARC_V5_HOLD = 0,
    ARC_V5_VERIFY,
    ARC_V5_LOCK,
} arc_v5_state_t;

typedef struct {
    int p_median;
    int q_phase;
    int clip_permille;
    int origin_permille;
    int winding_permille;
    arc_v5_context_t context;
} arc_v5_observation_t;

/* Learned local response to one +1 vendor-gain step, Q8 fixed point.
 * Samples are deliberately local/short-horizon so room geometry is not stored
 * as a permanent distance->gain table. */
typedef struct {
    int16_t dp_q8;
    int16_t dq_q8;
    int16_t dorigin_q8;
    int16_t dclip_q8;
    uint16_t samples;
    uint16_t settle_ms;
} arc_v5_gain_model_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t table_fingerprint;
    uint32_t generation;
    arc_v5_gain_model_t gain[ARC_V5_GAIN_STATES];
    uint32_t crc;
} arc_v5_persisted_model_t;

typedef struct {
    arc_gain_table_t table;
    arc_v3_controller_t v3;
    arc_v5_gain_model_t model[ARC_V5_GAIN_STATES];

    uint32_t table_fingerprint;
    uint32_t model_generation;
    uint32_t learned_updates;
    uint32_t dirty_updates;
    uint32_t last_save_ms;

    uint8_t gain;
    uint8_t survival_gain;
    uint8_t loaded_from_nvs;
    arc_v5_state_t state;

    unsigned weak_votes;
    unsigned strong_votes;
    unsigned no_carrier_ticks;
    unsigned verify_ticks;

    uint8_t pending_from;
    uint8_t pending_to;
    arc_v5_observation_t pending_before;
} arc_v5_autotune_t;

void arc_v5_autotune_reset(arc_v5_autotune_t *a,
                           const arc_gain_table_t *table,
                           uint8_t gain,
                           uint8_t survival_gain);

/* Re-arm runtime state while retaining a compatible learned model. */
void arc_v5_autotune_rearm(arc_v5_autotune_t *a,
                           const arc_gain_table_t *table,
                           uint8_t gain,
                           uint8_t survival_gain);

uint8_t arc_v5_autotune_tick(arc_v5_autotune_t *a,
                             const arc_v5_observation_t *o);

unsigned arc_v5_model_confidence(const arc_v5_autotune_t *a, uint8_t gain);

bool arc_v5_export_model(const arc_v5_autotune_t *a,
                         arc_v5_persisted_model_t *out);
bool arc_v5_import_model(arc_v5_autotune_t *a,
                         const arc_v5_persisted_model_t *in);

/* ESP-IDF NVS wrappers. Host builds keep these as harmless stubs. */
bool arc_v5_load_nvs(arc_v5_autotune_t *a);
bool arc_v5_should_save(const arc_v5_autotune_t *a, uint32_t now_ms);
bool arc_v5_save_nvs(arc_v5_autotune_t *a, uint32_t now_ms);

const char *arc_v5_state_name(arc_v5_state_t state);
