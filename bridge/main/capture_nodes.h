/* SPDX-License-Identifier: GPL-3.0-only */
/* How many DMA nodes were completed between two end-of-node interrupts of
 * the capture ring (capture.c). Plain C: the host test checks it. */
#pragma once
#include <stdint.h>

/* `idx` and `last_idx`: the ring nodes (0 .. nodes - 1, a power of two) the
 * two interrupts reported; `dt_us`: the time between them; `node_us_q8`:
 * the time one node takes to fill, Q8 microseconds.
 *
 * The node index alone wraps every ring. An interrupt held off for a ring
 * or more (a flash write stalls both cores for milliseconds, a sector erase
 * for tens of them) would lose whole rings: the write position would fall
 * behind the truth, and the decoder would take overwritten samples for
 * fresh ones instead of seeing that it fell a ring behind. The time between
 * the interrupts says how many rings went by; it only has to be right to
 * half a ring. */
static inline uint32_t capture_nodes_advance(uint32_t idx, uint32_t last_idx, uint32_t nodes, uint32_t dt_us,
                                             uint32_t node_us_q8)
{
    uint32_t adv = (idx - last_idx) & (nodes - 1);
    if (!adv) adv = nodes;   /* the same node again: a whole ring later */
    if (dt_us > 0x00ffffffu) dt_us = 0x00ffffffu;   /* 16 s: keeps the shift below in 32 bits */
    const uint32_t est = (dt_us << 8) / node_us_q8;
    if (est > adv + nodes / 2) adv += (est - adv + nodes / 2) / nodes * nodes;
    return adv;
}
