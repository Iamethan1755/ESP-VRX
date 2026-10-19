/* C5VRX-5 by Twotoz and the C5VRX contributors: verified LUT16 updates.
 * ESP32-C5 TRM: CPU addressing follows the configured LUT width too. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define KP_LUT_ENTRIES 1024u
typedef bool (*kp_lut_write_fn)(void *ctx, unsigned entry, uint16_t value);
typedef void (*kp_lut_select_fn)(void *ctx, unsigned entry);
typedef void (*kp_lut_data_fn)(void *ctx, uint16_t value);
typedef uint32_t (*kp_lut_read_fn)(void *ctx);

/* Selection refreshes the CPU read latch. After storing CFG1, select CFG0
 * again before verifying; the upper 16 read bits are outside LUT16. MMIO
 * callbacks supply ordering barriers around their register accesses. */
static inline bool kp_lut_write16(void *ctx, unsigned entry, uint16_t value,
                                  kp_lut_select_fn select_entry,
                                  kp_lut_data_fn store, kp_lut_read_fn read)
{
    select_entry(ctx, entry);
    store(ctx, value);
    select_entry(ctx, entry);
    return (read(ctx) & 0xffffu) == value;
}

/* Caller owns a halted core. Verify the entire candidate before publishing
 * its shadow. Failure leaves the last verified shadow intact for rollback. */
static inline bool kp_lut_load(uint16_t *shadow, const uint16_t *want,
                               kp_lut_write_fn write_entry, void *ctx,
                               uint32_t *writes, uint32_t *verify_fail)
{
    bool verified = true;
    for (unsigned entry = 0; entry < KP_LUT_ENTRIES; ++entry) {
        ++*writes;
        if (!write_entry(ctx, entry, want[entry])) {
            ++*verify_fail;
            verified = false;
        }
    }
    if (verified && shadow != want) memcpy(shadow, want, KP_LUT_ENTRIES * sizeof(*shadow));
    return verified;
}
