#include "board_config.h"
#include "board_pinout.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"

static const char *TAG = "board";

static uint64_t pin_mask(int pin)
{
    return pin >= 0 && pin <= 28 ? 1ULL << pin : 0;
}

esp_err_t board_config_validate(void)
{
    const int iq[8] = C5VRX_IQ_GPIOS;
    const int dac[8] = C5VRX_DAC_GPIOS;
    int pins[17];
    for (unsigned i = 0; i < 8; ++i) {
        pins[i] = iq[i];
        pins[8 + i] = dac[i];
    }
    pins[16] = CONFIG_C5VRX_BUTTON_GPIO;
    uint64_t console = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    console |= pin_mask(13) | pin_mask(14);
#endif
#if CONFIG_ESP_CONSOLE_UART_CUSTOM
    console |= pin_mask(CONFIG_ESP_CONSOLE_UART_TX_GPIO) |
               pin_mask(CONFIG_ESP_CONSOLE_UART_RX_GPIO);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
    console |= pin_mask(U0TXD_GPIO_NUM) | pin_mask(U0RXD_GPIO_NUM);
#endif
    /* Also keep pin_mask referenced when the console is disabled. */
    console |= pin_mask(-1);
    unsigned bad = 0;
    board_pins_error_t error = board_pinout_check(pins, CONFIG_C5VRX_DAC_BITS,
        SOC_GPIO_VALID_OUTPUT_GPIO_MASK, console, &bad);
    if (error != BOARD_PINS_OK) {
        ESP_LOGE(TAG, "Invalid pinout: %s bit %u GPIO%d, reason=%d "
                 "(1=width 2=invalid/unassigned 3=flash 4=console 5=duplicate)",
                 bad < 8 ? "IQ" : bad < 16 ? "DAC" : "button",
                 bad < 8 ? bad : bad < 16 ? bad - 8 : 0,
                 pins[bad], (int)error);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "DAC %d bits, LSB->MSB GPIOs %d,%d,%d,%d,%d,%d,%d,%d; button=%d",
             CONFIG_C5VRX_DAC_BITS, dac[0], dac[1], dac[2], dac[3],
             dac[4], dac[5], dac[6], dac[7], pins[16]);
    return ESP_OK;
}
