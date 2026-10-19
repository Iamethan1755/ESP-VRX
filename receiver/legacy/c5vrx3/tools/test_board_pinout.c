#include "board_pinout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const int defaults[17] = {1,0,25,7,10,5,3,4,23,24,11,12,8,9,-1,-1,28};
static const uint64_t valid = (1ULL << 29) - 1;
static const uint64_t usb = (1ULL << 13) | (1ULL << 14);

int main(void)
{
    int pins[17]; unsigned bad = 0;
    memcpy(pins, defaults, sizeof(pins));
    assert(board_pinout_check(pins, 6, valid, usb, &bad) == BOARD_PINS_OK);
    assert(board_pinout_check(pins, 7, valid, usb, &bad) == BOARD_PINS_INVALID_WIDTH);
    assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_INVALID_GPIO);
    pins[14] = 2; pins[15] = 6;
    assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_OK);
    for (unsigned i = 0; i < 17; ++i) {
        int saved = pins[i];
        pins[i] = 16;
        assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_FLASH_GPIO);
        pins[i] = 29;
        assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_INVALID_GPIO);
        pins[i] = 13;
        assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_CONSOLE_GPIO);
        assert(board_pinout_check(pins, 8, valid, 0, &bad) == BOARD_PINS_OK);
        pins[i] = saved;
    }
    pins[14] = pins[0];
    assert(board_pinout_check(pins, 8, valid, usb, &bad) == BOARD_PINS_DUPLICATE_GPIO);
    assert(bad == 14);
    memcpy(pins, defaults, sizeof(pins));
    pins[1] = pins[0];
    assert(board_pinout_check(pins, 6, valid, usb, &bad) == BOARD_PINS_DUPLICATE_GPIO);
    memcpy(pins, defaults, sizeof(pins));
    pins[16] = -1;
    assert(board_pinout_check(pins, 6, valid, usb, &bad) == BOARD_PINS_OK);
    assert(board_pinout_check(pins, 6, valid, 1ULL << 11, &bad) == BOARD_PINS_CONSOLE_GPIO);
    assert(board_pinout_check(pins, 6, valid & ~(1ULL << 25), usb, &bad) == BOARD_PINS_INVALID_GPIO);
    puts("Board pinout validation PASS");
}
