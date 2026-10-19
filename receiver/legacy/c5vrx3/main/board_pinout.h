#pragma once
#include <stdint.h>

typedef enum {
    BOARD_PINS_OK,
    BOARD_PINS_INVALID_WIDTH,
    BOARD_PINS_INVALID_GPIO,
    BOARD_PINS_FLASH_GPIO,
    BOARD_PINS_CONSOLE_GPIO,
    BOARD_PINS_DUPLICATE_GPIO,
} board_pins_error_t;

/* Order: eight IQ loopbacks, eight DAC bits, optional button. Unused DAC
 * entries must be -1. The IQ loopbacks need output AND input capability. */
static inline board_pins_error_t board_pinout_check(const int pins[17],
    unsigned dac_bits, uint64_t valid_output_mask, uint64_t console_mask,
    unsigned *bad_index)
{
    uint64_t used = 0;
    if (dac_bits != 6 && dac_bits != 8) return BOARD_PINS_INVALID_WIDTH;
    for (unsigned i = 0; i < 17; ++i) {
        const int pin = pins[i];
        *bad_index = i;
        if (i >= 8 + dac_bits && i < 16) {
            if (pin != -1) return BOARD_PINS_INVALID_GPIO;
            continue;
        }
        if (i == 16 && pin == -1) continue;
        if (pin < 0 || pin > 28 || !(valid_output_mask & (1ULL << pin)))
            return BOARD_PINS_INVALID_GPIO;
        if (pin >= 16 && pin <= 22) return BOARD_PINS_FLASH_GPIO;
        if (console_mask & (1ULL << pin)) return BOARD_PINS_CONSOLE_GPIO;
        if (used & (1ULL << pin)) return BOARD_PINS_DUPLICATE_GPIO;
        used |= 1ULL << pin;
    }
    return BOARD_PINS_OK;
}
