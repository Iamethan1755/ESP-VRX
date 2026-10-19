/* SPDX-License-Identifier: GPL-3.0-only */
/* Link to the C5VRX receiver: its on-screen menu, channel, band and scan
 * keys as "<k>" frames on a UART TX line: the video bit 0 wire (S3 D0 ->
 * C5 D4, CONFIG_BRIDGE_KEYS_ON_D0; the receiver then outputs 5-bit video)
 * or a separate wire (S3 D8 -> 1 kOhm -> C5 GPIO2, the MTMS pad on the
 * back of the XIAO ESP32-C5). The line is driven
 * only while a frame goes out and released in between, so an unpowered C5
 * gets no current from it. The receiving side is in receiver/main/console.c
 * on the wifi-bridge branch. */
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
/* Tunes the receiver straight to one channel: index = band * 8 + channel,
 * bands R A B E F L as in C5VRX's rf.c ("<#NN>" on the wire). */
#define C5LINK_CHANNELS 48
bool c5link_set_channel(int index);
/* The channel last set from here, or -1 when unknown (after boot, a scan
 * or a band/channel step: the receiver does not report back). */
int c5link_channel(void);
/* True when the released key line sits high: the receiver listens on it.
 * Keys are not sent otherwise (see c5link.c line_free). */
bool c5link_listening(void);
/* Checks that the TX pin really toggles (sends unframed bytes the
 * receiver ignores). Safe with the C5 attached. */
esp_err_t c5link_selftest(char *out, size_t len);
