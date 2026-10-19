/* C5VRX by Twotoz and contributors: control observations are not RF samples. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t profile, phy, gain; } rx_control_epoch_t;
static inline bool rx_control_epoch_equal(rx_control_epoch_t a, rx_control_epoch_t b)
{
    return a.profile == b.profile && a.phy == b.phy && a.gain == b.gain;
}
/* The sentinel runs every 200 us. A delayed emergency must use fresh input,
 * rather than cut gain because of an old strong signal/channel/lane state. */
static inline bool rx_control_observation_current(rx_control_epoch_t observed,
    rx_control_epoch_t current, uint64_t observed_us, uint64_t now_us)
{
    return rx_control_epoch_equal(observed, current) && now_us >= observed_us &&
           now_us - observed_us <= 1000u;
}
