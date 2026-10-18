/* SPDX-License-Identifier: GPL-3.0-only */
/* UART key link to the C5VRX receiver, see c5link.h. */
#include "c5link.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define LINK_UART UART_NUM_1

static const char *TAG = "c5link";
static bool s_ready;
static uint32_t s_sent;

esp_err_t c5link_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART, 256, 256, 0, NULL, 0), TAG, "driver");
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &cfg), TAG, "config");
    /* Open drain, no pull-up here: the S3 only ever pulls the line low and
     * the C5's own pull-up on GPIO2 makes the idle high. A push-pull high fed
     * ~2.5 mA through the 1 kOhm and the pad's protection diode into an
     * unpowered C5's 3.3 V rail; powered up from that half-way voltage the C5
     * hung (USB present, CPU not running) until reset (board, 2026-10-18).
     * Set before and after attaching the pin, so it is never driven high. */
    const gpio_num_t tx = (gpio_num_t)CONFIG_BRIDGE_GPIO_C5_TX;
    gpio_set_pull_mode(tx, GPIO_FLOATING);
    gpio_od_enable(tx);
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, CONFIG_BRIDGE_GPIO_C5_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE), TAG, "pin");
    gpio_od_enable(tx);
    s_ready = true;
    ESP_LOGI(TAG, "receiver keys on GPIO%d (UART 115200, open drain)", CONFIG_BRIDGE_GPIO_C5_TX);
    return ESP_OK;
}

bool c5link_send(char key)
{
    if (!s_ready || !key || !strchr(C5LINK_KEYS, key)) return false;
    const char frame[3] = {'<', key, '>'};
    if (uart_write_bytes(LINK_UART, frame, sizeof frame) != (int)sizeof frame) return false;
    s_sent++;
    return true;
}

uint32_t c5link_sent(void) { return s_sent; }

esp_err_t c5link_selftest(char *out, size_t len)
{
    if (!s_ready) {
        snprintf(out, len, "LINK not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    /* Read the pad back while sending 0x55 bytes (unframed: the receiver
     * waits for '<' and ignores them). The open-drain line needs a pull-up
     * to go high without a C5: the S3's own, only for this test. */
    gpio_input_enable(CONFIG_BRIDGE_GPIO_C5_TX);
    gpio_set_pull_mode(CONFIG_BRIDGE_GPIO_C5_TX, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(100);
    static const char probe[] = "UUUUUUUUUUUUUUUU";
    uart_write_bytes(LINK_UART, probe, sizeof probe - 1);
    int high = 0, low = 0;
    for (int i = 0; i < 2000; i++) {
        if (gpio_get_level(CONFIG_BRIDGE_GPIO_C5_TX)) high++;
        else low++;
        esp_rom_delay_us(1);
    }
    uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(50));
    gpio_set_pull_mode(CONFIG_BRIDGE_GPIO_C5_TX, GPIO_FLOATING);
    const bool ok = high > 100 && low > 100;
    snprintf(out, len, "LINK GPIO%d toggles: high=%d low=%d of 2000 samples, keys sent=%lu %s",
             CONFIG_BRIDGE_GPIO_C5_TX, high, low, (unsigned long)s_sent, ok ? "PASS" : "FAIL");
    return ok ? ESP_OK : ESP_FAIL;
}
