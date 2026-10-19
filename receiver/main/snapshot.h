/* C5VRX by Twotoz and contributors: completed circular IQ snapshot geometry. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define C5V4_LEVEL_SAMPLE_BYTES 8190u
typedef struct { size_t offset, first, safe_bytes; } c5v4_snapshot_plan_t;
/* Descriptor discovery begins at the current DMA node, not necessarily at
 * buffer zero. Validate one contiguous physical segment in that cyclic order.
 * The caller must also require total coverage == ring after the last node. */
static inline bool c5v4_snapshot_segment(size_t ring, size_t first,
    size_t covered, size_t offset, size_t bytes)
{
    if (!ring || first >= ring || covered >= ring || offset >= ring ||
        !bytes || bytes > ring-covered || bytes > ring-offset) return false;
    size_t expected = covered < ring-first ? first+covered : covered-(ring-first);
    return offset == expected;
}
static inline bool c5v4_snapshot_plan(size_t ring, size_t active_offset,
    size_t active_bytes, size_t bytes, c5v4_snapshot_plan_t *out)
{
    if (!out || !bytes || active_offset >= ring || !active_bytes ||
        active_bytes > ring-active_offset || bytes >= ring-active_bytes) return false;
    out->offset = (active_offset + ring - bytes) % ring;
    out->first = bytes < ring-out->offset ? bytes : ring-out->offset;
    out->safe_bytes = ring-bytes-active_bytes;
    return true;
}
/* The copy ends where the active descriptor began; RX may keep writing ahead
 * of it. `advanced` is the forward distance from that start to the start of
 * the descriptor active after the copy, `after_bytes` that descriptor's
 * length: everything RX can have touched lies inside them. The copy is valid
 * if that span never reaches the copied bytes (ring - copied), and the copy
 * time with a 10 % margin could not have lapped the ring unseen (safe_bytes,
 * about 512 us at IQ40). The former <=50 us/no-advance rule refused every
 * board copy (65-234 us under observer load, 2026-10-08) although RX still
 * had ~512 us to go before reaching the copied bytes. */
static inline bool c5v4_snapshot_current(size_t ring, size_t copied,
    size_t advanced, size_t after_bytes, uint64_t elapsed_us,
    size_t safe_bytes, unsigned rate)
{
    return rate && copied < ring && advanced < ring && after_bytes <= ring &&
        advanced + after_bytes <= ring - copied &&
        elapsed_us * rate * 11u < (uint64_t)safe_bytes * 10000000u;
}
