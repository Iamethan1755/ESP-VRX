/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic composite source for VRX Bridge: writes interlaced
 * NTSC (525 lines) or PAL (625 lines) as 6-bit DAC codes into the capture
 * ring, with the standard equalising and broad pulses of both fields, so
 * the decoder, JPEG and Wi-Fi path can be tested without a receiver.
 * Picture: eight bars, a ramp and a box moving every field.
 * Plain C, also used by the host test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TP_SYNC  4    /* DAC codes, like a C5VRX output at roughly 1x OUTPUT */
#define TP_BLANK 18
#define TP_WHITE 53   /* blanking + 2.5 x (blanking - sync): 100 IRE */

typedef struct {
    uint8_t *ring;
    uint32_t mask;
    uint32_t fs_hz;
    bool pal;
    uint32_t pos;          /* absolute samples written */
    uint32_t line_q16;     /* line length, Q16 samples */
    uint32_t acc_q16;
    int line;              /* 0-based line within the frame */
    int lines;             /* 525 or 625 */
    uint32_t field;        /* fields written, moves the box */
    int noise;             /* +-noise codes of random noise per sample (0 = clean) */
    int clicks;            /* FM clicks per 10000 samples: 1-2 samples forced to black or white */
    int lv_sync, lv_blank, lv_white;   /* DAC codes of sync tip, blanking, white */
    uint32_t rng;
    int len_max, half;
    int x0, x_len;         /* active picture, samples from the line start */
    uint8_t *t_eq, *t_broad, *t_blank, *t_bars, *t_ramp, *pic, *buf;
} testpattern_t;

bool testpattern_init(testpattern_t *t, uint8_t *ring, uint32_t ring_size, uint32_t fs_hz, bool pal);
/* Same with chosen levels (DAC codes) and, with dark_left, the bars and
 * ramp mirrored so every line starts dark (where vertical sync is probed). */
bool testpattern_init_levels(testpattern_t *t, uint8_t *ring, uint32_t ring_size, uint32_t fs_hz, bool pal,
                             int sync, int blank, int white, bool dark_left);
void testpattern_free(testpattern_t *t);
/* Continue writing at absolute position `pos` from the start of a frame. */
void testpattern_restart(testpattern_t *t, uint32_t pos);
/* Write whole lines until at least `target` samples have been written. */
void testpattern_run_until(testpattern_t *t, uint32_t target);
