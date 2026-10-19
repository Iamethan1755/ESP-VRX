/* SPDX-License-Identifier: GPL-3.0-only */
/* UART key link to the C5VRX receiver, see c5link.h. */
#include "c5link.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_sig_map.h"
#include "freertos/semphr.h"

#define LINK_UART UART_NUM_1
#ifdef CONFIG_BRIDGE_KEYS_ON_D0
#define LINK_PIN CONFIG_BRIDGE_GPIO_D0   /* the video bit 0 wire (C5 D4) */
#else
#define LINK_PIN CONFIG_BRIDGE_GPIO_C5_TX
#endif
#define LINK_TX ((gpio_num_t)LINK_PIN)

static const char *TAG = "c5link";
static bool s_ready;
static uint32_t s_sent;
static int s_channel = -1;
static SemaphoreHandle_t s_lock;

/* The line is driven only while a frame goes out: push-pull from a 2 ms
 * idle high before the start bit until the last stop bit, then released
 * (open drain, output high = not driven). Released, an unpowered C5 gets no
 * current from the S3: a permanent push-pull high fed ~2.5 mA through the
 * 1 kOhm into an unpowered C5's 3.3 V rail, and a C5 powered up from that
 * half-way voltage hung. Driven, the C5's UART gets clean edges: open drain
 * alone relied on the C5's pull-up on GPIO2 and no key arrived (board,
 * 2026-10-18). */
static bool send_frame(const char *frame, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_od_disable(LINK_TX);
    vTaskDelay(pdMS_TO_TICKS(2));   /* idle high: the receiver's UART settles */
    const bool ok = uart_write_bytes(LINK_UART, frame, n) == (int)n &&
                    uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(50)) == ESP_OK;
    esp_rom_delay_us(100);          /* the last stop bit */
    gpio_od_enable(LINK_TX);
    xSemaphoreGive(s_lock);
    return ok;
}

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
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART, 256, 256, 0, NULL, 0), TAG, "driver");
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &cfg), TAG, "config");
    /* Released between frames (see send_frame); set before and after
     * attaching the pin, so it is not driven at start-up either. */
    gpio_reset_pin(LINK_TX);   /* D0 may come from the capture's input setup */
    gpio_set_pull_mode(LINK_TX, GPIO_FLOATING);
    gpio_od_enable(LINK_TX);
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, LINK_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE), TAG, "pin");
    gpio_od_enable(LINK_TX);
    s_ready = true;
    ESP_LOGI(TAG, "receiver keys on GPIO%d (UART 115200, driven only while sending)", LINK_PIN);
    return ESP_OK;
}

bool c5link_send(char key)
{
    if (!s_ready || !key || !strchr(C5LINK_KEYS, key)) return false;
    const char frame[3] = {'<', key, '>'};
    if (!send_frame(frame, sizeof frame)) return false;
    s_sent++;
    if (strchr("bcs", key)) s_channel = -1;   /* the receiver picks the channel itself */
    return true;
}

uint32_t c5link_sent(void) { return s_sent; }

bool c5link_set_channel(int index)
{
    if (!s_ready || index < 0 || index >= C5LINK_CHANNELS) return false;
    char frame[6];
    snprintf(frame, sizeof frame, "<#%02d>", index);
    if (!send_frame(frame, 5)) return false;
    s_sent++;
    s_channel = index;
    return true;
}

int c5link_channel(void) { return s_channel; }

esp_err_t c5link_selftest(char *out, size_t len)
{
    if (!s_ready) {
        snprintf(out, len, "LINK not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    /* Read the pad back while sending 0x55 bytes (unframed: the receiver
     * waits for '<' and ignores them), driven as for a real frame. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_input_enable(LINK_TX);
    gpio_od_disable(LINK_TX);
    esp_rom_delay_us(200);
    static const char probe[] = "UUUUUUUUUUUUUUUU";
    uart_write_bytes(LINK_UART, probe, sizeof probe - 1);
    int high = 0, low = 0;
    for (int i = 0; i < 2000; i++) {
        if (gpio_get_level(LINK_TX)) high++;
        else low++;
        esp_rom_delay_us(1);
    }
    uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(50));
    esp_rom_delay_us(100);

    /* The wire to the C5: pull the line low as a plain GPIO, release it and
     * time the rise. The receiver's pull-up on GPIO2 raises it within a few
     * us through the 1 kOhm; an open wire, an unpowered C5 or a receiver
     * without the bridge build (no pull-up) leaves it low. */
    esp_rom_gpio_connect_out_signal(LINK_TX, SIG_GPIO_OUT_IDX, false, false);
    gpio_od_disable(LINK_TX);
    gpio_set_level(LINK_TX, 0);
    esp_rom_delay_us(50);
    gpio_od_enable(LINK_TX);
    gpio_set_level(LINK_TX, 1);   /* released */
    int rise_us = -1;
    for (int us = 0; us < 300; us++) {
        if (gpio_get_level(LINK_TX)) {
            rise_us = us;
            break;
        }
        esp_rom_delay_us(1);
    }
    uart_set_pin(LINK_UART, LINK_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_od_enable(LINK_TX);
    xSemaphoreGive(s_lock);
    const bool ok = high > 100 && low > 100;
    char wire[96];
    if (rise_us >= 0) snprintf(wire, sizeof wire, "C5 pull-up seen (line rose in %d us)", rise_us);
    else snprintf(wire, sizeof wire, "NO C5 pull-up (line stayed low 300 us: wire open, C5 off or not the bridge build)");
    snprintf(out, len, "LINK GPIO%d toggles: high=%d low=%d of 2000 samples, keys sent=%lu %s; wire: %s",
             LINK_PIN, high, low, (unsigned long)s_sent, ok ? "PASS" : "FAIL", wire);
    return ok ? ESP_OK : ESP_FAIL;
}
