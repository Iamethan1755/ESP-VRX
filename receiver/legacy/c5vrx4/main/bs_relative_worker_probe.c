#include "bs_relative_worker_probe.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "hal/bitscrambler_peri_select.h"

BITSCRAMBLER_PROGRAM(s_relative_worker_probe, "bs_relative_worker_probe");

static uint8_t s_input[256] __attribute__((aligned(4)));
static uint8_t s_output[256] __attribute__((aligned(4)));
static bool s_ran;
static bool s_pass;
static size_t s_written;
static unsigned s_mismatches;
static esp_err_t s_err;

void bs_relative_worker_probe_report(void)
{
    printf("BS_REL_WORKER status=%s written=%u mismatches=%u err=%s\n",
           !s_ran ? "NOT_RUN" : s_pass ? "PASS" : "FAIL",
           (unsigned)s_written, s_mismatches, esp_err_to_name(s_err));
}

void bs_relative_worker_probe_run(void)
{
    for (unsigned pair = 0; pair < 128; ++pair) {
        s_input[2 * pair] = (uint8_t)(pair * 37u + 3u);
        s_input[2 * pair + 1] = (uint8_t)(pair * 13u + 0x80u);
    }
    memset(s_output, 0xa5, sizeof(s_output));

    bitscrambler_handle_t bs = NULL;
    size_t written = 0;
    esp_err_t err = bitscrambler_loopback_create(&bs,
                                                  SOC_BITSCRAMBLER_ATTACH_I2S0,
                                                  sizeof(s_input));
    if (err == ESP_OK) {
        err = bitscrambler_load_program(bs, s_relative_worker_probe);
    }
    if (err == ESP_OK) {
        err = bitscrambler_loopback_run(bs, s_input, sizeof(s_input),
                                        s_output, sizeof(s_output), &written);
    }
    if (bs) {
        bitscrambler_free(bs);
    }

    unsigned mismatches = 0;
    for (unsigned pair = 0; pair < 128; ++pair) {
        uint8_t first = s_input[2 * pair];
        uint8_t selected = (first & 1u) ? s_input[2 * pair + 1] : first;
        mismatches += s_output[2 * pair] != selected;
        mismatches += s_output[2 * pair + 1] != selected;
    }
    s_ran = true;
    s_pass = err == ESP_OK && written == sizeof(s_output) && mismatches == 0;
    s_written = written;
    s_mismatches = mismatches;
    s_err = err;
    bs_relative_worker_probe_report();
}
