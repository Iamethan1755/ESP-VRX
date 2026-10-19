/* C5VRX by Twotoz and contributors. Bounded video level servo. */
#pragma once
#include "cvbs_monitor.h"
#define C5V4_LEVEL_PERIOD_US 20000u
#define C5V4_LEVEL_FAST_US 5000u
#define C5V4_LEVEL_RECOVERY_US 100000u
#define C5V4_LEVEL_STEP_UV 32000u
/* Settled hold (HDZero, 2026-10-04): every live LUT write is a possible
 * output disturbance, so once a full update pass finds nothing beyond the
 * 8-mV deadband the servo only writes again for an entry >24 mV off target,
 * and at most every 250 ms. A context change or reacquisition unsettles it,
 * so recovery speed is unchanged. */
#define C5V4_LEVEL_DEADBAND_UV 8000u
#define C5V4_LEVEL_SETTLED_DEADBAND_UV 24000u
#define C5V4_LEVEL_SETTLED_US 250000u
typedef struct {
    unsigned good, updates, refusals;
    int blank, span;
    uint32_t context;
    uint64_t last_us;
    uint64_t evidence_us, recovery_until_us;
    int evidence_blank[3], evidence_span[3];
    unsigned target_depth_mv;
    unsigned evidence_slot, evidence_depth;
    bool context_valid, evidence_valid, settled;
    uint8_t codes[256];
} c5v4_level_t;
void c5v4_level_init(c5v4_level_t *state);
/* Start slewing from the DAC codes actually loaded (STD150 or CVBS150), not
 * from the default table: every update is limited to 32 mV in the loaded voltage table. */
void c5v4_level_seed(c5v4_level_t *state, const uint8_t codes[256]);
/* False freezes the last applied mapping, including through signal loss. */
bool c5v4_level_observe(c5v4_level_t *state, const c5v4_cvbs_stats_t *stats,
                      bool fresh, uint32_t context, uint64_t now_us);
uint16_t c5v4_level_word(uint16_t original, unsigned code);
/* A context switch opens a bounded fast reacquisition interval. */
unsigned c5v4_level_period(c5v4_level_t *, uint32_t context, uint64_t now_us);
/* Slew in electrical volts, including measured/nonmonotonic code tables. */
uint8_t c5v4_level_slew(uint8_t current, uint8_t target, const uint32_t volts[64]);
uint8_t c5v4_level_slew_band(uint8_t current, uint8_t target, const uint32_t volts[64],
                             uint32_t deadband_uv);
