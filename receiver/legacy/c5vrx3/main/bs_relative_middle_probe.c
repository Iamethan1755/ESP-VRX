#include "bs_relative_middle_probe.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "hal/bitscrambler_peri_select.h"

BITSCRAMBLER_PROGRAM(s_relative_middle_probe, "bs_relative_middle_probe");

static const uint8_t s_phase5[256] = {
    4, 6, 7, 7, 7, 8, 8, 8, 24, 24, 24, 25, 25, 25, 26, 28,
    2, 4, 5, 6, 6, 7, 7, 7, 25, 25, 25, 26, 26, 27, 28, 30,
    1, 3, 4, 5, 5, 6, 6, 6, 26, 26, 26, 27, 27, 28, 29, 31,
    1, 2, 3, 4, 5, 5, 5, 6, 26, 27, 27, 27, 28, 29, 30, 31,
    1, 2, 3, 3, 4, 5, 5, 5, 27, 27, 27, 28, 29, 29, 30, 31,
    0, 1, 2, 3, 3, 4, 4, 5, 27, 28, 28, 28, 29, 30, 31, 0,
    0, 1, 2, 3, 3, 4, 4, 4, 28, 28, 28, 29, 29, 30, 31, 0,
    0, 1, 2, 2, 3, 3, 4, 4, 28, 28, 29, 29, 30, 30, 31, 0,
    16, 15, 14, 14, 13, 13, 12, 12, 20, 20, 19, 19, 18, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 12, 20, 20, 20, 19, 19, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 11, 21, 20, 20, 19, 19, 18, 17, 16,
    15, 14, 13, 13, 12, 12, 11, 11, 21, 21, 21, 20, 19, 19, 18, 17,
    15, 14, 13, 12, 11, 11, 11, 10, 22, 21, 21, 21, 20, 19, 18, 17,
    15, 13, 12, 11, 11, 10, 10, 10, 22, 22, 22, 21, 21, 20, 19, 17,
    14, 12, 11, 10, 10, 9, 9, 9, 23, 23, 23, 22, 22, 21, 20, 18,
    12, 10, 9, 9, 9, 8, 8, 8, 24, 24, 24, 23, 23, 23, 22, 20
};

static uint8_t s_input[256] __attribute__((aligned(4)));
static uint8_t s_output[256] __attribute__((aligned(4)));
static bool s_ran;
static bool s_pass;
static size_t s_written;
static unsigned s_mismatches;
static esp_err_t s_err;

void bs_relative_middle_probe_report(void)
{
    printf("BS_REL_MIDDLE status=%s written=%u mismatches=%u err=%s\n",
           !s_ran ? "NOT_RUN" : s_pass ? "PASS" : "FAIL",
           (unsigned)s_written, s_mismatches, esp_err_to_name(s_err));
}

void bs_relative_middle_probe_run(void)
{
    for (unsigned pair = 0; pair < 128; ++pair) {
        s_input[2 * pair]     = (uint8_t)(pair * 37u + 5u);
        s_input[2 * pair + 1] = (uint8_t)(pair * 13u + 0x43u);
    }
    memset(s_output, 0xa5, sizeof(s_output));

    bitscrambler_handle_t bs = NULL;
    size_t written = 0;
    esp_err_t err = bitscrambler_loopback_create(&bs,
                                                  SOC_BITSCRAMBLER_ATTACH_I2S0,
                                                  sizeof(s_input));
    if (err == ESP_OK) {
        err = bitscrambler_load_program(bs, s_relative_middle_probe);
    }
    if (err == ESP_OK) {
        err = bitscrambler_loopback_run(bs, s_input, sizeof(s_input),
                                        s_output, sizeof(s_output), &written);
    }
    if (bs) {
        bitscrambler_free(bs);
    }

    unsigned mismatches = 0;
    for (unsigned pair = 1; pair < 128; ++pair) {
        unsigned src_pair = pair - 1u;
        uint8_t selected_byte = (src_pair % 2u == 0u)
            ? s_input[2 * src_pair]
            : s_input[2 * src_pair + 1];
        uint8_t expected_phase = s_phase5[selected_byte];
        mismatches += (s_output[2 * pair] != expected_phase);
        mismatches += (s_output[2 * pair + 1] != expected_phase);
    }

    s_ran = true;
    s_pass = err == ESP_OK && written == sizeof(s_output) && mismatches == 0;
    s_written = written;
    s_mismatches = mismatches;
    s_err = err;
    bs_relative_middle_probe_report();
}
