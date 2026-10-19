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
#include "freertos/semphr.h"
#include "soc/gpio_sig_map.h"

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

/* Released (not driven by the S3), the line must sit steadily high: the
 * receiver's pull-up on its key pin is there and nothing else drives it.
 * Low or toggling means the C5 is unpowered (its pin clamps to a dead rail),
 * not listening, or still outputting video on that pin (C5VRX without this
 * repo's receiver build, which drives D4 as video bit 0). Driving then would
 * fight the C5's output or feed an unpowered C5 (no series resistor on D0),
 * so no key is sent. Called with the line released. */
static bool line_free(void)
{
    for (int i = 0; i < 50; i++) {
        if (!gpio_get_level(LINK_TX)) return false;
        esp_rom_delay_us(2);
    }
    return true;
}

/* The line is driven only while a frame goes out: push-pull from a 2 ms
 * idle high before the start bit until the last stop bit, then released
 * (open drain, output high = not driven). Released, an unpowered C5 gets no
 * current from the S3 (a permanently driven high once kept an unpowered C5
 * half powered, and it hung at power-up). Driven, the C5's UART gets clean
 * edges (open drain alone, relying on the C5's weak pull-up, let no key
 * through; board, 2026-10-18). */
static bool send_frame(const char *frame, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!line_free()) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "key not sent: the receiver is not listening on the key line (u on the console checks it)");
        return false;
    }
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
     * attaching the pin, so it is not driven at start-up either. The input
     * stays enabled for line_free(). */
    gpio_reset_pin(LINK_TX);   /* D0 may come from the capture's input setup */
    gpio_set_pull_mode(LINK_TX, GPIO_FLOATING);
    gpio_od_enable(LINK_TX);
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, LINK_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE), TAG, "pin");
    gpio_od_enable(LINK_TX);
    gpio_input_enable(LINK_TX);
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
    /* band, channel and scan, and Select in the receiver's own menu (whose
     * items include band, channel and scan): the receiver picks the channel */
    if (strchr("bcsx", key)) s_channel = -1;
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

bool c5link_listening(void)
{
    if (!s_ready) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool free = line_free();
    xSemaphoreGive(s_lock);
    return free;
}

/* The wire to the C5: pull the line low as a plain GPIO for 50 us, release
 * it and time the rise through the receiver's pull-up (about 1 us on the
 * board). Returns the rise in us, or -1 when it stays low for 300 us. */
static int rise_us(void)
{
    esp_rom_gpio_connect_out_signal(LINK_TX, SIG_GPIO_OUT_IDX, false, false);
    gpio_od_disable(LINK_TX);
    gpio_set_level(LINK_TX, 0);
    esp_rom_delay_us(50);
    gpio_od_enable(LINK_TX);
    gpio_set_level(LINK_TX, 1);   /* released */
    int rise = -1;
    for (int us = 0; us < 300; us++) {
        if (gpio_get_level(LINK_TX)) {
            rise = us;
            break;
        }
        esp_rom_delay_us(1);
    }
    uart_set_pin(LINK_UART, LINK_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_od_enable(LINK_TX);
    return rise;
}

esp_err_t c5link_selftest(char *out, size_t len)
{
    if (!s_ready) {
        snprintf(out, len, "LINK not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* First the wire, released: only a listening receiver gets the probe. */
    const int rise = line_free() ? rise_us() : -1;
    int high = 0, low = 0;
    if (rise >= 0 && rise < 20) {
        /* Read the pad back while sending 0x55 bytes (unframed: the receiver
         * waits for '<' and ignores them), driven as for a real frame. A busy
         * CPU can start sampling after the 16 bytes went out (all high): try
         * again then. */
        for (int attempt = 0; attempt < 3 && (high < 100 || low < 100); attempt++) {
            high = low = 0;
            gpio_od_disable(LINK_TX);
            esp_rom_delay_us(200);
            static const char probe[] = "UUUUUUUUUUUUUUUU";
            uart_write_bytes(LINK_UART, probe, sizeof probe - 1);
            for (int i = 0; i < 2000; i++) {
                if (gpio_get_level(LINK_TX)) high++;
                else low++;
                esp_rom_delay_us(1);
            }
            uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(50));
            esp_rom_delay_us(100);
            gpio_od_enable(LINK_TX);
        }
    }
    xSemaphoreGive(s_lock);
    const bool listening = rise >= 0 && rise < 20, ok = listening && high > 100 && low > 100;
    char wire[112];
    if (listening) snprintf(wire, sizeof wire, "receiver pull-up seen (line rose in %d us)", rise);
    else if (rise >= 0) snprintf(wire, sizeof wire, "slow rise (%d us): no receiver pull-up, line floating", rise);
    else snprintf(wire, sizeof wire, "line low or busy: C5 off, wire open, C5 not running this repo's receiver "
                                     "build, or a resistor DAC on D4");
    snprintf(out, len, "LINK GPIO%d wire: %s; send test: high=%d low=%d, keys sent=%lu %s", LINK_PIN, wire, high,
             low, (unsigned long)s_sent, ok ? "PASS" : listening ? "FAIL" : "SKIPPED (not driven)");
    return ok ? ESP_OK : ESP_FAIL;
}
