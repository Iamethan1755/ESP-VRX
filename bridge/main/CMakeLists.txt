idf_component_register(
    SRCS "main.c" "capture.c" "video.c" "testpattern.c" "stream.c" "console.c" "c5link.c" "denoise.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_dma esp_hal_cam esp_hal_dma esp_driver_usb_serial_jtag esp_driver_uart
             esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer esp_psram
             esp_hw_support esp_app_format
    EMBED_TXTFILES "web/index.html")
