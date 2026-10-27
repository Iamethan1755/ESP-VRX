/* SPDX-License-Identifier: GPL-3.0-only */
/* Optional SPI TFT screen of ESP-VRX (menuconfig BRIDGE_SPI_DISPLAY):
 * the live picture on a 320x240 ILI9341 or ST7789 wired to the XIAO
 * ESP32-S3, at the same time as the Wi-Fi stream. */
#pragma once
#include "esp_err.h"

/* Sets up the SPI bus and starts the display task on core 0 (the panel is
 * initialised there). Call before Wi-Fi starts: the two strip buffers need
 * internal DMA memory. */
esp_err_t display_start(void);
/* Frames per second sent to the screen. */
float display_fps(void);
