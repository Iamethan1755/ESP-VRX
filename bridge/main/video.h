/* Composite video decoder for the C5VRX Wi-Fi bridge.
 *
 * Input: a power-of-two ring of 6-bit composite samples (the C5VRX DAC code,
 * bits 0..5 of each byte) written by DMA or by the test-pattern generator.
 * Output: one grayscale frame per field (VIDEO_W x 240 NTSC / 288 PAL).
 *
 * Plain C with no ESP-IDF dependency, so test/test_video.c runs it on a PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VIDEO_W     320
#define VIDEO_H_MAX 288

typedef enum { VIDEO_STD_UNKNOWN = 0, VIDEO_STD_NTSC, VIDEO_STD_PAL } video_std_t;
typedef enum { VIDEO_LEVELS_AUTO = 0, VIDEO_LEVELS_SYNC } video_levels_t;

/* Called when a field is complete; `rendered` is false for a field that was
 * skipped. Return the buffer the next field is rendered into (a different
 * buffer to keep `done`, or `done` to drop it) and set *render_next to
 * false to skip drawing the next field (it would be dropped anyway): the
 * sync is still tracked, only the pixels are not computed. */
typedef uint8_t *(*video_field_cb_t)(uint8_t *done, int w, int h, bool rendered, bool *render_next, void *ctx);

typedef struct {
    bool h_locked;            /* line sync tracked */
    bool v_locked;            /* last field had a vertical sync of plausible length */
    video_std_t std;
    int lines_per_field;      /* lines between the last two vertical syncs */
    float line_us;            /* tracked line period */
    float sync_level;         /* sync tip, in DAC codes (0..63) */
    float blank_level;        /* back porch, in DAC codes */
    float black_level;        /* output mapping: code shown as black */
    float white_level;        /* output mapping: code shown as white */
    uint32_t fields;
    uint32_t lines;
    uint32_t overruns;        /* decoder fell a ring behind and resynchronised */
    uint32_t lock_losses;
    int h_offset_tenths_us;   /* user picture shift, 0.1 us steps */
    int v_offset_lines;       /* user picture shift, lines */
    video_levels_t levels;
} video_stats_t;

typedef struct {
    /* source */
    const uint8_t *ring;
    uint32_t mask;
    uint32_t ring_size;
    uint32_t fs_hz;
    int64_t wr;               /* samples available (absolute, extended from 32 bit) */
    uint32_t wr32;

    /* timing constants, Q8 samples unless noted (computed from fs_hz) */
    int64_t p_ntsc, p_pal, p_min, p_max;
    int64_t win;              /* locked edge search half-width */
    int c_skip;               /* samples, unlocked search start after an edge */
    int c_min_low_locked;     /* samples a sync must stay low (locked) */
    int c_min_low_unlocked;   /* samples a sync must stay low (unlocked) */
    int c_hs_max;             /* longest low run still counted as a line sync */
    int c_max_run;            /* low-run measurement cap */
    int c_broad1, c_broad2;   /* broad-pulse probes from line start */
    int c_sync_a, c_sync_b;   /* sync tip window */
    int c_porch_a, c_porch_b; /* back porch window */
    int c_tail;               /* samples needed past the next edge search */
    int c_margin;             /* distance kept from the DMA write head */
    int64_t h_start_base[2];  /* active video start after the sync edge, Q8 [NTSC, PAL] */
    int64_t h_start[2];       /* h_start_base + user offset, Q8 */
    uint32_t h_step[2];       /* Q8 samples per output pixel */

    /* line PLL */
    bool h_locked;
    int64_t t_edge;         /* Q8 start (sync falling edge) of the current line */
    int64_t period;           /* Q8 line period */
    int64_t prev_edge;
    bool have_prev;
    int good, miss;
    int cur_run;              /* low run of the current line's sync (0: flywheel) */
    uint32_t unlocked_lines;
    int64_t reset_pos;        /* first sample of the current source */

    /* levels, Q8 DAC codes */
    int32_t sync_q8, blank_q8, slice_q8;
    bool levels_valid;
    int32_t map_lo_q8, map_hi_q8;   /* output mapping, smoothed */
    int32_t map_off;                /* Q2 (sum of 4 samples) */
    int32_t map_gain;               /* Q16 */
    uint8_t lut[1024];              /* filtered sum (4x code) -> pixel, rebuilt per field */
    uint32_t hist[64];              /* filtered active-area levels of this field */

    /* vertical */
    int line_in_field;
    int broad_run;
    int active_first_ntsc, active_first_pal;
    int v_offset;
    int std_votes;
    video_std_t std;
    bool v_locked;

    /* output */
    uint8_t *fb;
    bool render;              /* draw this field's pixels */
    bool delivered;           /* this field already went to the callback */
    video_field_cb_t cb;
    void *cb_ctx;
    video_levels_t levels;

    /* stats */
    uint32_t max_line_us;     /* slowest single line since the last read (firmware only) */
    uint32_t max_line_kind;   /* what that line did: 1 rendered, 2 field start, 4 unlocked search */
    int lines_per_field;
    uint32_t fields, lines, overruns, lock_losses;
    int h_offset_tenths;
} video_t;

void video_init(video_t *v, const uint8_t *ring, uint32_t ring_size, uint32_t fs_hz,
                uint8_t *fb, video_field_cb_t cb, void *cb_ctx);
/* Drop all tracking and continue from `write_pos` (after a source change). */
void video_reset(video_t *v, uint32_t write_pos);
/* Decode every complete line up to `write_pos` (absolute samples written). */
void video_process(video_t *v, uint32_t write_pos);
void video_get_stats(const video_t *v, video_stats_t *out);

/* Picture adjustments (call from the decoder's own task). */
void video_shift_h(video_t *v, int tenths_us);
void video_shift_v(video_t *v, int lines);
void video_set_levels(video_t *v, video_levels_t mode);
