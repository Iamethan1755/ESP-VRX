#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARC_RX_STAGE_COUNT 9u
#define ARC_VENDOR_GAIN_MAX 89u

/* The installed RX gain table, reconstructed exactly as the pinned vendor
 * generator builds it (phy_gen_rx_gain_table, esp-phy-lib 59c1234): entry
 * k of stage s has counter c = start[s] + k, BB = {1,3,...,127}[c / 6] and
 * fine = 5 - c % 6. The 5 GHz band (phy_param[0x2a] != 0) uses its own
 * constant spans / start counters / RF codes and its maximum at +0x126. */
typedef struct {
    uint8_t spans[ARC_RX_STAGE_COUNT];
    uint8_t start[ARC_RX_STAGE_COUNT];
    uint16_t rf_code[ARC_RX_STAGE_COUNT];
    uint8_t max_index;
    bool band5;
    bool runtime_spans_valid;
} arc_gain_table_t;

#define ARC_5G_GAIN_MAX 83u     /* 84 entries, counters 12..41 in the last stage */

typedef struct {
    uint8_t gain_index;
    uint8_t rf_stage;
    uint16_t rf_code;
    uint8_t bb_code;
    uint8_t fine_code;
    uint32_t packed_state;
} arc_gain_tuple_t;

/* Capture the gain-table description produced by the pinned vendor PHY.
 * This function is read-only and must run after normal PHY initialization. */
void arc_phy_capture_gain_table(arc_gain_table_t *table);

/* Pure helpers are kept public so the closed-PHY reconstruction can be
 * regression-tested on the host. */
/* spans == NULL: the 5 GHz table (this receiver's band); otherwise the
 * 2.4 GHz table with those runtime spans (+0x422). */
void arc_gain_table_from_bytes(arc_gain_table_t *table,
                               const uint8_t spans[ARC_RX_STAGE_COUNT],
                               uint8_t max_index);
bool arc_gain_tuple_decode(const arc_gain_table_t *table, uint8_t gain_index,
                           arc_gain_tuple_t *tuple);
uint8_t arc_gain_highest_rf_stage_start(const arc_gain_table_t *table);
