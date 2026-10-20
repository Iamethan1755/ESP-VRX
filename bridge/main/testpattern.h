/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic composite source for VRX Bridge: writes interlaced
 * NTSC (525 lines) or PAL (625 lines) as 6-bit DAC codes into the capture
 * ring, with the standard equalising and broad pulses of both fields, so
 * the decoder, JPEG and Wi-Fi path can be tested without a receiver.
 * Picture: eight bars, a ramp and a box moving every field. With colour
 * (NTSC only) the bars are 75 % colour bars and every line with a line
 * sync carries the colour burst.
 * Plain C, also used by the host test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TP_SYNC  4    /* DAC codes, like a C5VRX output at roughly 1x OUTPUT */
#define TP_BLANK 18
#define TP_WHITE 53   /* blanking + 2.5 x (blanking - sync): 100 IRE */
#define TP_PHASES 16  /* colour lines are drawn at 16 subcarrier phases (22.5 degree steps) */

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
    int drop_vsync;        /* the next this many fields lose their broad pulses (fades) */
    bool dropping;
    bool five_bit;         /* bit 0 always 0, as with the keys on the bit 0 wire */
    bool colour;           /* colour bars and burst (testpattern_set_colour) */
    uint32_t sc_inc;       /* subcarrier phase per sample, 2^32 per cycle */
    int cq;                /* this line's subcarrier phase step (of TP_PHASES) */
    int b0, b_len;         /* burst, samples from the line start */
    /* Colour lines: the bars line at each phase (one allocation each, small
     * enough for internal RAM: from PSRAM the generator cost the decoder's
     * core so much time that it overran), the burst alone at each phase, and
     * a blanking line that gets this line's burst. */
    uint8_t *c_bars[TP_PHASES], *c_burst, *c_blank;
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
/* NTSC colour bars and burst on/off; false if not possible (PAL, no memory). */
bool testpattern_set_colour(testpattern_t *t, bool on);
/* Continue writing at absolute position `pos` from the start of a frame. */
void testpattern_restart(testpattern_t *t, uint32_t pos);
/* Write whole lines until at least `target` samples have been written. */
void testpattern_run_until(testpattern_t *t, uint32_t target);
