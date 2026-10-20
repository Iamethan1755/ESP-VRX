# SPDX-License-Identifier: GPL-3.0-only
set(srcs "main.c" "capture.c" "video.c" "testpattern.c" "stream.c" "console.c" "c5link.c" "denoise.c" "mbox.c")
if(CONFIG_BRIDGE_SPI_DISPLAY)
    list(APPEND srcs "display.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_dma esp_hal_cam esp_hal_dma esp_driver_usb_serial_jtag esp_driver_uart
             esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer esp_psram
             esp_hw_support esp_app_format esp_driver_spi esp_lcd
    EMBED_TXTFILES "web/index.html")
