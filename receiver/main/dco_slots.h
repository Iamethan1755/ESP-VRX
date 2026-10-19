/* C5VRX-5 by Twotoz and the C5VRX contributors: per-channel DC tables in NVS
 * (pure logic, host test test/test_dco_slots.c).
 *
 * The DC DAC codes are measured per gain row for one tuning. One stored
 * table meant that every channel change was followed by a new DC search at
 * the next boot. Tables are now kept per frequency in DCO_SLOTS fixed NVS
 * blobs ("dco0".."dco5", bounded NVS use, no erase needed) with a small
 * index of their frequencies and use order. A frequency without its own
 * table starts from the nearest stored one: the baseband offset per gain
 * row changes little across the band (calibration.c), and the live drift
 * nudges trim the rest. The first measured table is thus the template for
 * every channel. Per row, the index remembers which rows of a channel still
 * come from a template; a measurement on the own channel (VTX off at boot,
 * CALIBRATE, or on the carrier) replaces exactly those rows. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DCO_SLOTS          6u
#define DCO_INDEX_VERSION  2u
#define DCO_MASK_ROWS      96u   /* >= ARC_VENDOR_GAIN_MAX + 1 gain rows */
#define DCO_MASK_BYTES     (DCO_MASK_ROWS / 8u)

typedef uint8_t dco_mask_t[DCO_MASK_BYTES];

typedef struct {
    uint16_t version;
    uint16_t freq[DCO_SLOTS];   /* MHz, 0 = empty slot */
    uint16_t seq[DCO_SLOTS];    /* use order, higher = more recent */
    uint16_t next_seq;
    dco_mask_t templ[DCO_SLOTS]; /* rows still taken from another channel */
} dco_index_t;

static inline bool dco_mask_get(const uint8_t *m, unsigned row)
{
    return row < DCO_MASK_ROWS && (m[row / 8u] >> (row % 8u)) & 1u;
}
static inline void dco_mask_put(uint8_t *m, unsigned row, bool on)
{
    if (row >= DCO_MASK_ROWS) return;
    if (on) m[row / 8u] |= (uint8_t)(1u << (row % 8u));
    else m[row / 8u] &= (uint8_t)~(1u << (row % 8u));
}
static inline unsigned dco_mask_count(const uint8_t *m, unsigned lo, unsigned hi)
{
    unsigned n = 0;
    for (unsigned r = lo; r <= hi && r < DCO_MASK_ROWS; ++r) n += dco_mask_get(m, r);
    return n;
}

static inline void dco_index_init(dco_index_t *x)
{
    *x = (dco_index_t){.version = DCO_INDEX_VERSION, .next_seq = 1u};
}

static inline int dco_slot_of(const dco_index_t *x, uint16_t freq)
{
    for (unsigned k = 0; k < DCO_SLOTS; ++k)
        if (freq && x->freq[k] == freq) return (int)k;
    return -1;
}

/* Slot to store `freq` in: its own slot, else an empty one, else the least
 * recently used. */
static inline unsigned dco_slot_for_store(const dco_index_t *x, uint16_t freq)
{
    int own = dco_slot_of(x, freq);
    if (own >= 0) return (unsigned)own;
    unsigned best = 0;
    for (unsigned k = 0; k < DCO_SLOTS; ++k) {
        if (!x->freq[k]) return k;
        if (x->seq[k] < x->seq[best]) best = k;
    }
    return best;
}

/* Nearest stored frequency other than `freq` (ties: the more recent one),
 * preferring tables measured on their own channel over tables that still
 * hold template rows; -1 when nothing is stored. */
static inline int dco_nearest_slot(const dco_index_t *x, uint16_t freq)
{
    int best = -1;
    unsigned best_d = 0;
    bool measured_exists = false;
    for (unsigned k = 0; k < DCO_SLOTS; ++k)
        if (x->freq[k] && x->freq[k] != freq && !dco_mask_count(x->templ[k], 0, DCO_MASK_ROWS - 1u))
            measured_exists = true;
    for (unsigned k = 0; k < DCO_SLOTS; ++k) {
        if (!x->freq[k] || x->freq[k] == freq) continue;
        if (measured_exists && dco_mask_count(x->templ[k], 0, DCO_MASK_ROWS - 1u)) continue;
        unsigned d = x->freq[k] > freq ? (unsigned)(x->freq[k] - freq) : (unsigned)(freq - x->freq[k]);
        if (best < 0 || d < best_d || (d == best_d && x->seq[k] > x->seq[best])) { best = (int)k; best_d = d; }
    }
    return best;
}

static inline void dco_index_touch(dco_index_t *x, unsigned slot, uint16_t freq)
{
    if (slot >= DCO_SLOTS) return;
    if (x->next_seq == UINT16_MAX) {            /* renumber, keeping the order */
        for (unsigned k = 0; k < DCO_SLOTS; ++k) x->seq[k] = x->freq[k] ? (uint16_t)(x->seq[k] / 2u) : 0u;
        x->next_seq = UINT16_MAX / 2u + 1u;
    }
    x->freq[slot] = freq;
    x->seq[slot] = x->next_seq++;
}
