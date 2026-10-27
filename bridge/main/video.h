/* SPDX-License-Identifier: GPL-3.0-only */
/* Composite video decoder for ESP-VRX.
 *
 * Input: a power-of-two ring of 6-bit composite samples (the C5VRX DAC code,
 * bits 0..5 of each byte; bits 6 and 7 must be 0) written by DMA or by the
 * test-pattern generator.
 * Output: one frame per field (VIDEO_W x 240 NTSC / 288 PAL), grayscale or,
 * for NTSC with a colour burst and colour on, luma plus a chroma plane
 * (docs/colour-study.md).
 *
 * Plain C with no ESP-IDF dependency, so test/test_video.c runs it on a PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VIDEO_W     320
#define VIDEO_H_MAX 288
#define VIDEO_BPP_MAX 2   /* frame buffers hold VIDEO_W x VIDEO_H_MAX x VIDEO_BPP_MAX bytes */

/* Frame formats: gray, or colour as planes: the luma rows exactly as a gray
 * frame, then at VIDEO_CHROMA_OFF one chroma row per two picture rows (from
 * the even row): VIDEO_CW Cb bytes, then VIDEO_CW Cr bytes (JFIF, 128 = no
 * colour). Chroma sample k covers pixels 5k .. 5k + 4. The decoder writes
 * luma only as wide rows go to PSRAM slowly (board: 640-byte rows cost
 * 4000 cycles each); the encoder builds YCbYCr from the planes. */
typedef enum { VIDEO_FMT_GRAY = 1, VIDEO_FMT_COLOUR = 2 } video_fmt_t;
#define VIDEO_CW 64
#define VIDEO_CHROMA_OFF (VIDEO_W * VIDEO_H_MAX)
#define VIDEO_CHROMA_ROW (2 * VIDEO_CW)

/* Colour tables (sizes for 20 MS/s). */
#define VIDEO_BTAB 48     /* burst window */
#define VIDEO_CSPAN 1024  /* chroma: VIDEO_CW blocks of 16 samples */

/* Median of three, from min/max only (single instructions on the S3). */
static inline int video_med3(int a, int b, int c)
{
    const int lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

typedef enum { VIDEO_STD_UNKNOWN = 0, VIDEO_STD_NTSC, VIDEO_STD_PAL } video_std_t;
typedef enum { VIDEO_LEVELS_AUTO = 0, VIDEO_LEVELS_SYNC } video_levels_t;
/* Horizontal filter per pixel: median of three samples two apart (removes
 * FM clicks, the white and black speckles, without blurring edges) or the
 * [1 2 1] low-pass. */
typedef enum { VIDEO_FILTER_MEDIAN = 0, VIDEO_FILTER_SMOOTH } video_filter_t;

/* Called when a field is complete, in format `fmt`; `rendered` is false for
 * a field that was skipped. Return the buffer the next field is rendered
 * into (a different buffer to keep `done`, or `done` to drop it) and set
 * *render_next to false to skip drawing the next field (it would be dropped
 * anyway): the sync is still tracked, only the pixels are not computed.
 * Buffers hold VIDEO_W x VIDEO_H_MAX x VIDEO_BPP_MAX bytes. */
typedef uint8_t *(*video_field_cb_t)(uint8_t *done, int w, int h, video_fmt_t fmt, bool rendered, bool *render_next,
                                     void *ctx);

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
    video_filter_t filter;
    bool colour;              /* colour decoding on (user setting) */
    bool colour_shown;        /* the last field was drawn in colour */
    bool colour_simd;         /* chroma sums with the S3's SIMD instructions (self-test passed) */
    float burst_level;        /* colour burst, DAC codes peak (0: none tracked) */
    uint32_t slips;           /* burst phase steps of about one sample: S3/C5 clock slips */
    int32_t slip_sum;         /* duplicated minus dropped samples (> 0: the S3 runs fast) */
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
    int c_box;                /* sync search averages this many samples (0.4 us) */
    int c_tap;                /* smoothing filter tap spacing (0.1 us) */
    int c_broad[7];           /* broad-pulse probes from line start (6..24 us) */
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
    uint8_t lut[256];               /* filtered sum (4x code, 0..252) -> pixel, rebuilt per field */
    uint32_t hist[64];              /* filtered active-area levels of this field */

    /* vertical */
    int line_in_field;
    int broad_run;
    int active_first_ntsc, active_first_pal;
    int v_offset;
    int std_votes;
    video_std_t std;
    bool v_locked;
    int v_miss;               /* vertical syncs missed in a row (flywheel) */
    int field_len[2];         /* lengths of the last two fields (interlace alternates them) */

    /* output */
    uint8_t *fb;
    bool render;              /* draw this field's pixels */
    bool delivered;           /* this field already went to the callback */
    video_field_cb_t cb;
    void *cb_ctx;
    video_levels_t levels;
    video_filter_t filter;

    /* colour (NTSC). The burst and the chroma are summed against cos/sin
     * tables that start at the burst window and at the first chroma block
     * (whole samples from the line's sync edge); each table entry is
     * (cos << 16) + sin at 31 peak, with a zero sum over every block, so flat
     * luma adds nothing and one 32-bit multiply-add per sample sums both.
     * The burst reference is kept at the absolute sample index, where NTSC's
     * line-to-line alternation is gone and only clock drift and slips move
     * it. */
    bool colour;              /* user setting */
    bool colour_field;        /* this field is drawn in colour */
    int c_burst;              /* burst window start after the sync edge, samples */
    int c_bblk, c_bblocks;    /* burst sums: blocks of c_bblk samples */
    int c_cblk;               /* samples per chroma block (VIDEO_CW blocks a line) */
    int c_coff;               /* first chroma block, samples after the picture start */
    uint32_t sc_inc;          /* subcarrier phase per sample, 2^32 per cycle */
    float sc_cos[256];        /* cos of the subcarrier phase's top 8 bits */
    int32_t btab[VIDEO_BTAB];
    int8_t ccos[VIDEO_CSPAN] __attribute__((aligned(16)));   /* chroma tables, 31 peak, zero sum per block */
    int8_t csin[VIDEO_CSPAN] __attribute__((aligned(16)));
    uint8_t cline[VIDEO_CSPAN + 16] __attribute__((aligned(16)));   /* the line's chroma samples, copied */
    bool pie;                 /* the S3's SIMD chroma sums passed their self-test (firmware only) */
    float ref_re, ref_im;     /* burst reference (raw burst-sum units, absolute phase) */
    float pend_re, pend_im;   /* a burst far off the reference, waiting for a second one */
    bool have_ref, have_pend;
    bool pend_back;           /* the waiting burst is behind the reference (a duplicated sample) */
    float line_re, line_im;   /* the reference this line uses */
    bool line_ok;             /* it has a usable burst */
    bool burst_ok;            /* the burst is strong enough for colour (hysteresis) */
    int burst_clip;           /* burst samples are limited to blanking +- this (FM clicks) */
    int weak_bursts;          /* lines in a row without a usable burst */
    float chroma_gain;        /* per field: chroma sums / reference -> luma 0..1 units */
    uint32_t slips;
    int32_t slip_sum;

    /* stats */
    uint32_t max_line_us;     /* slowest single line since the last read (firmware only) */
    uint32_t max_line_kind;   /* what that line did: 1 rendered, 2 field start, 4 unlocked search */
    uint32_t drawn_us, drawn_lines;     /* time spent on lines of drawn fields since the last read (firmware only) */
    uint32_t skipped_us, skipped_lines; /* ... and of fields that are not drawn */
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
void video_set_filter(video_t *v, video_filter_t filter);
/* Colour on/off: NTSC fields with a colour burst come out as YCbYCr. */
void video_set_colour(video_t *v, bool on);
