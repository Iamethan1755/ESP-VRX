/* SPDX-License-Identifier: GPL-3.0-only */
/* Link to the C5VRX receiver: its on-screen menu, channel, band and scan
 * keys as "<k>" frames on a UART TX line (S3 GPIO7 / D8 -> 1 kOhm -> C5
 * GPIO2, the MTMS pad on the back of the XIAO ESP32-C5), driven open drain:
 * the C5's pull-up makes the idle high, so an unpowered C5 gets no current
 * from it. The receiving side is in receiver/main/console.c on the
 * wifi-bridge branch. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Keys the receiver accepts from the bridge:
 * o menu open/close, n/x menu next/select, e close menu,
 * c next channel, b next band, s scan, v video standard. */
#define C5LINK_KEYS "ocbsnxve"

esp_err_t c5link_init(void);
/* Sends one key; false for keys the receiver would not accept. */
bool c5link_send(char key);
uint32_t c5link_sent(void);
/* Checks that the TX pin really toggles (sends unframed bytes the
 * receiver ignores). Safe with the C5 attached. */
esp_err_t c5link_selftest(char *out, size_t len);
