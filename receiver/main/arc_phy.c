#include "arc_phy.h"

#include <stddef.h>
#include <string.h>

/* Exact library contract (verified by disassembly, 2026-10-06):
 * esp-phy-lib 59c1234e929212aec0fdda75769b759951235536, ESP32-C5,
 * libphy.a SHA256 dbf33c41...104fffb.
 * phy_set_rx_gain_table() builds the 2.4 GHz table from the runtime spans at
 * +0x422 and, when phy_param[0x2a] != 0, the 5 GHz table from constant
 * spans/start counters/RF codes; the 5 GHz maximum lands at +0x126.
 * phy_gen_rx_gain_table() packs RF[20:12], BB[10:4], fine[2:0] with
 * counter c = start[stage] + k, BB = BBCODE[c / 6], fine = 5 - c % 6, and
 * runs the last stage up to counter 41.
 *
 * Until 2026-10-06 this file decoded the 5 GHz table with the 2.4 GHz RF
 * codes and spans and no start counters, and took the minimum of the three
 * maxima: every tuple from G0 on was mis-labelled (the top RF stage starts at
 * G54, not G61) and G81..G83 were refused (external audit, PR #174). */
#define PHY_PARAM_BAND5_OFFSET    0x2au
#define PHY_PARAM_RX_MAX_A_OFFSET 0x124u
#define PHY_PARAM_RX_MAX_B_OFFSET 0x125u
#define PHY_PARAM_RX_MAX_C_OFFSET 0x126u
#define PHY_PARAM_RX_SPANS_OFFSET 0x422u
#define LAST_COUNTER 41u

extern unsigned char phy_param[];

static const uint8_t s_bb_codes[7] = {1u, 3u, 7u, 15u, 31u, 63u, 127u};

static const uint8_t s_spans_24[ARC_RX_STAGE_COUNT] = {15u, 13u, 5u, 8u, 6u, 4u, 4u, 6u, 0u};
static const uint8_t s_start_24[ARC_RX_STAGE_COUNT] = {0u, 11u, 20u, 20u, 22u, 22u, 23u, 22u, 22u};
static const uint16_t s_rf_24[ARC_RX_STAGE_COUNT] = {64u, 100u, 93u, 94u, 107u, 119u, 124u, 125u, 127u};

static const uint8_t s_spans_5g[ARC_RX_STAGE_COUNT] = {10u, 7u, 8u, 5u, 8u, 5u, 4u, 7u, 0u};
static const uint8_t s_start_5g[ARC_RX_STAGE_COUNT] = {12u, 13u, 15u, 12u, 12u, 12u, 12u, 12u, 12u};
static const uint16_t s_rf_5g[ARC_RX_STAGE_COUNT] = {24u, 20u, 0u, 65u, 97u, 162u, 354u, 419u, 487u};

/* Entries the generator emits for these spans and start counters. */
static unsigned table_entries(const arc_gain_table_t *t)
{
    unsigned n = 0u;
    for (unsigned i = 0; i + 1u < ARC_RX_STAGE_COUNT; ++i) n += t->spans[i];
    unsigned s = t->start[ARC_RX_STAGE_COUNT - 1u];
    return n + (LAST_COUNTER >= s ? LAST_COUNTER - s + 1u : 0u);
}

static void finish_table(arc_gain_table_t *table, uint8_t max_index)
{
    unsigned entries = table_entries(table);
    unsigned top = entries ? entries - 1u : 0u;
    if (top > ARC_VENDOR_GAIN_MAX) top = ARC_VENDOR_GAIN_MAX;
    table->max_index = max_index >= 2u && max_index <= top ? max_index : (uint8_t)top;
    unsigned covered = 0u;
    for (unsigned i = 0; i + 1u < ARC_RX_STAGE_COUNT; ++i) covered += table->spans[i];
    table->spans[ARC_RX_STAGE_COUNT - 1u] =
        covered <= table->max_index ? (uint8_t)(table->max_index + 1u - covered) : 0u;
}

void arc_gain_table_from_bytes(arc_gain_table_t *table,
                               const uint8_t spans[ARC_RX_STAGE_COUNT],
                               uint8_t max_index)
{
    if (!table) return;
    memset(table, 0, sizeof(*table));
    if (!spans) {
        memcpy(table->spans, s_spans_5g, sizeof(table->spans));
        memcpy(table->start, s_start_5g, sizeof(table->start));
        memcpy(table->rf_code, s_rf_5g, sizeof(table->rf_code));
        table->band5 = true;
        table->runtime_spans_valid = true;   /* constants of the generator */
        finish_table(table, max_index);
        return;
    }
    bool valid = true;
    for (unsigned i = 0; i + 1u < ARC_RX_STAGE_COUNT; ++i)
        if (spans[i] == 0u) { valid = false; break; }
    memcpy(table->spans, valid ? spans : s_spans_24, sizeof(table->spans));
    memcpy(table->start, s_start_24, sizeof(table->start));
    memcpy(table->rf_code, s_rf_24, sizeof(table->rf_code));
    table->runtime_spans_valid = valid;
    finish_table(table, max_index);
}

void arc_phy_capture_gain_table(arc_gain_table_t *table)
{
    if (phy_param[PHY_PARAM_BAND5_OFFSET]) {
        arc_gain_table_from_bytes(table, NULL, phy_param[PHY_PARAM_RX_MAX_C_OFFSET]);
        return;
    }
    uint8_t max = phy_param[PHY_PARAM_RX_MAX_B_OFFSET];
    if (max < 2u) max = phy_param[PHY_PARAM_RX_MAX_A_OFFSET];
    arc_gain_table_from_bytes(table, &phy_param[PHY_PARAM_RX_SPANS_OFFSET], max);
}

bool arc_gain_tuple_decode(const arc_gain_table_t *table, uint8_t gain_index,
                           arc_gain_tuple_t *tuple)
{
    if (!table || !tuple || gain_index > table->max_index) return false;

    unsigned start = 0u;
    unsigned stage = ARC_RX_STAGE_COUNT - 1u;
    for (unsigned i = 0; i + 1u < ARC_RX_STAGE_COUNT; ++i) {
        unsigned end = start + table->spans[i];
        if (gain_index < end) { stage = i; break; }
        start = end;
    }
    unsigned counter = table->start[stage] + ((unsigned)gain_index - start);
    unsigned coarse = counter / 6u;
    if (coarse > 6u) coarse = 6u;

    tuple->gain_index = gain_index;
    tuple->rf_stage = (uint8_t)stage;
    tuple->rf_code = table->rf_code[stage];
    tuple->bb_code = s_bb_codes[coarse];
    tuple->fine_code = (uint8_t)(5u - (counter % 6u));
    tuple->packed_state = ((uint32_t)tuple->rf_code << 12) |
                          ((uint32_t)tuple->bb_code << 4) |
                          tuple->fine_code;
    return true;
}

uint8_t arc_gain_highest_rf_stage_start(const arc_gain_table_t *table)
{
    if (!table) return 54u;
    unsigned start = 0u;
    for (unsigned i = 0; i + 1u < ARC_RX_STAGE_COUNT; ++i)
        start += table->spans[i];
    return start <= table->max_index ? (uint8_t)start : table->max_index;
}
