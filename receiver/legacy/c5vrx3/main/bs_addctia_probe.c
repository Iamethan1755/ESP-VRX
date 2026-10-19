#include "bs_addctia_probe.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "hal/bitscrambler_peri_select.h"

BITSCRAMBLER_PROGRAM(s_addctia_probe, "bs_addctia_probe");

static const uint8_t s_dac32[32] = {
    20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50,
    0,  0,  0,  0,  0,  0,  0,  2,  4,  6,  8,  10, 12, 14, 16, 18
};

static uint8_t s_input[256] __attribute__((aligned(4)));
static uint8_t s_output[256] __attribute__((aligned(4)));
static bool s_ran;
static bool s_pass;
static size_t s_written;
static unsigned s_mismatches;
static unsigned s_dac_mismatches;
static esp_err_t s_err;

void bs_addctia_probe_report(void)
{
    printf("BS_ADDCTIA status=%s written=%u mismatches=%u dac_mismatches=%u err=%s\n",
           !s_ran ? "NOT_RUN" : s_pass ? "PASS" : "FAIL",
           (unsigned)s_written, s_mismatches, s_dac_mismatches, esp_err_to_name(s_err));
}

void bs_addctia_probe_run(void)
{
    for (unsigned pair = 0; pair < 128; ++pair) {
        uint8_t p = (uint8_t)((pair * 7u + 3u) & 31u);
        uint8_t c = (uint8_t)((pair * 13u + 19u) & 31u);
        s_input[2 * pair]     = (uint8_t)((256u - p) & 255u);
        s_input[2 * pair + 1] = c;
    }
    memset(s_output, 0xa5, sizeof(s_output));

    bitscrambler_handle_t bs = NULL;
    size_t written = 0;
    esp_err_t err = bitscrambler_loopback_create(&bs,
                                                  SOC_BITSCRAMBLER_ATTACH_I2S0,
                                                  sizeof(s_input));
    if (err == ESP_OK) {
        err = bitscrambler_load_program(bs, s_addctia_probe);
    }
    if (err == ESP_OK) {
        err = bitscrambler_loopback_run(bs, s_input, sizeof(s_input),
                                        s_output, sizeof(s_output), &written);
    }
    if (bs) {
        bitscrambler_free(bs);
    }

    unsigned mismatches = 0;
    unsigned dac_mismatches = 0;
    for (unsigned pair = 1; pair < 128; ++pair) {
        unsigned src_pair = pair - 1u;
        uint8_t p = (uint8_t)((src_pair * 7u + 3u) & 31u);
        uint8_t c = (uint8_t)((src_pair * 13u + 19u) & 31u);
        uint8_t expected_delta = (uint8_t)((c - p + 32u) % 32u);
        uint8_t expected_dac = s_dac32[expected_delta];

        mismatches += (s_output[2 * pair] != expected_delta);
        mismatches += (s_output[2 * pair + 1] != expected_delta);

        uint8_t hw_delta = (uint8_t)(s_output[2 * pair] & 31u);
        dac_mismatches += (s_dac32[hw_delta] != expected_dac);
    }

    s_ran = true;
    s_pass = (err == ESP_OK && written == sizeof(s_output) && mismatches == 0 && dac_mismatches == 0);
    s_written = written;
    s_mismatches = mismatches;
    s_dac_mismatches = dac_mismatches;
    s_err = err;
    bs_addctia_probe_report();
}
