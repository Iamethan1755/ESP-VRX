#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RX_AUTO_REJECT = 0,
    RX_AUTO_POOR,
    RX_AUTO_USABLE,
    RX_AUTO_SWEET,
} rx_auto_class_t;

typedef struct {
    int p_median;
    int q_phase;
    int clip_permille;
    int origin_permille;
    int iq_skew_permille;
    int iq_cross_permille;
    int winding_permille;
    int sync_quality;
    uint32_t transport_faults;
} rx_auto_observation_t;

rx_auto_class_t rx_auto_classify(const rx_auto_observation_t *o);
const char *rx_auto_class_name(rx_auto_class_t c);

/* Strict ordering for lab candidates. This deliberately does not reward raw
 * amplitude monotonically: P is compared only by distance from the Q4 target
 * window after hard clipping/transport rejection and coherence checks. */
bool rx_auto_better(const rx_auto_observation_t *a,
                    const rx_auto_observation_t *b);

bool rx_auto_reference_stable(const rx_auto_observation_t *before,
                              const rx_auto_observation_t *after);

bool rx_auto_is_rf_limit(const rx_auto_observation_t *o);
bool rx_auto_is_overload(const rx_auto_observation_t *o);
