/* Composite video decoder for the C5VRX Wi-Fi bridge: line PLL on the sync
 * edges, vertical sync from broad pulses, one grayscale frame per field.
 * See video.h. */
#include "video.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#define VIDEO_HOT IRAM_ATTR   /* per-sample loops never wait on flash cache */
#define NOW_US() ((uint32_t)esp_timer_get_time())
#else
#define VIDEO_HOT
#define NOW_US() 0u
#endif

#define FREE_RUN_LINES 400    /* no vertical sync after this many lines: emit the field anyway */
#define LOCK_GOOD      5      /* consecutive line-spaced syncs needed to lock */
#define LOCK_MISS      30     /* consecutive missing syncs that drop the lock */

static int32_t us_q8(uint32_t fs_hz, double us) { return (int32_t)(us * fs_hz / 1e6 * 256.0 + 0.5); }
static int us_n(uint32_t fs_hz, double us) { return (int)(us * fs_hz / 1e6 + 0.5); }

static inline bool below(const video_t *v, int64_t pos)
{
    return (int32_t)((v->ring[(uint32_t)pos & v->mask] & 63) << 8) < v->slice_q8;
}

/* Histogram percentile: smallest bin whose cumulative count reaches pct % of total. */
static int pct(const uint32_t *h, uint32_t total, uint32_t pct_x10)
{
    uint64_t want = (uint64_t)total * pct_x10 / 1000, acc = 0;
    for (int b = 0; b < 64; b++) {
        acc += h[b];
        if (acc > want) return b;
    }
    return 63;
}

/* First falling slice crossing in [from, from + n) whose low run lasts at
 * least min_low samples. The edge is interpolated to 1/256 sample. */
static VIDEO_HOT bool find_edge(const video_t *v, int64_t from, int n, int min_low,
                                int64_t *edge_q8, int *run_out)
{
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    const int32_t slice = v->slice_q8;
    uint32_t i = (uint32_t)from;
    int32_t prev = (r[(i - 1) & m] & 63) << 8;
    for (int k = 0; k < n; k++, i++) {
        int32_t cur = (r[i & m] & 63) << 8;
        if (prev >= slice && cur < slice) {
            int run = 1;
            while (run < v->c_max_run && (int32_t)((r[(i + run) & m] & 63) << 8) < slice) run++;
            if (run >= min_low) {
                int32_t frac = ((prev - slice) << 8) / (prev - cur);
                *edge_q8 = ((from + k - 1) << 8) + frac;
                *run_out = run;
                return true;
            }
            /* Short dip (noise, equalising pulse): continue after it. */
            k += run - 1;
            i += run - 1;
            cur = (r[i & m] & 63) << 8;
        }
        prev = cur;
    }
    return false;
}

/* Same result as find_edge() for syncs of at least min_low samples, but
 * probes only every min_low/2-th sample: a low run that long always covers
 * a probe whose predecessor probe lies before the run. Used while
 * unlocked, where most samples are picture or noise (17x fewer reads at
 * 10 MS/s). The exact crossing is then located between the two probes. */
static VIDEO_HOT bool find_edge_coarse(const video_t *v, int64_t from, int n, int min_low,
                                       int64_t *edge_q8, int *run_out)
{
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    const int32_t slice = v->slice_q8;
    const int st = min_low / 2 > 1 ? min_low / 2 : 1;
    const uint32_t base = (uint32_t)from;
#define LOW(i) ((int32_t)((r[(base + (uint32_t)(i)) & m] & 63) << 8) < slice)
    /* Crossings only after `from` (k - st >= 0): when called just past a
     * sync edge, `from` lies inside that pulse and must not find it again. */
    bool prev_low = LOW(0);
    for (int k = st; k < n; k += st) {
        const bool low = LOW(k);
        if (low && !prev_low) {
            int i = k;   /* last high->low crossing in (k - st, k] */
            while (i > k - st && !(!LOW(i - 1) && LOW(i))) i--;
            if (i > k - st) {
                int run = 1;
                while (run < v->c_max_run && LOW(i + run)) run++;
                if (run >= min_low) {
                    const int32_t prev = (r[(base + (uint32_t)i - 1) & m] & 63) << 8;
                    const int32_t cur = (r[(base + (uint32_t)i) & m] & 63) << 8;
                    *edge_q8 = ((from + i - 1) << 8) + ((prev - slice) << 8) / (prev - cur);
                    *run_out = run;
                    return true;
                }
            }
        }
        prev_low = low;
    }
#undef LOW
    return false;
}

static VIDEO_HOT int32_t avg_q8(const video_t *v, int64_t start, int n)
{
    uint32_t i = (uint32_t)start;
    int32_t s = 0;
    for (int k = 0; k < n; k++) s += v->ring[(i + k) & v->mask] & 63;
    return (s << 8) / n;
}

/* Slicer level from recent samples while no sync is tracked: 20 % of the
 * way from the sync tip (2nd percentile) to the picture (60th percentile).
 * That stays between sync and blanking for an all-black and an all-white
 * picture alike. */
static void estimate_slice(video_t *v)
{
    int64_t avail = v->wr - v->reset_pos;
    int n = avail > 8192 ? 8192 : (int)avail;
    if (n < 1024) return;
    uint32_t h[64] = {0};
    uint32_t i = (uint32_t)(v->wr - n);
    for (int k = 0; k < n; k += 4) h[v->ring[(i + k) & v->mask] & 63]++;
    int lo = pct(h, n / 4, 20), hi = pct(h, n / 4, 600);
    int32_t span = (hi - lo) * 256 / 5;
    v->slice_q8 = (lo << 8) + (span > 128 ? span : 128);
    v->sync_q8 = lo << 8;
    v->blank_q8 = 2 * v->slice_q8 - v->sync_q8;
    v->levels_valid = false;
}

static void update_mapping(video_t *v)
{
    int32_t lo, hi;
    if (v->levels == VIDEO_LEVELS_SYNC && v->levels_valid) {
        /* Standard levels: blanking is black, 100 IRE above it is white
         * (sync is 40 IRE below blanking). */
        lo = v->blank_q8;
        hi = v->blank_q8 + (v->blank_q8 - v->sync_q8) * 5 / 2;
        v->map_lo_q8 = lo;
        v->map_hi_q8 = hi;
    } else {
        uint32_t total = 0;
        for (int b = 0; b < 64; b++) total += v->hist[b];
        if (total < 500) goto apply;   /* field not drawn: keep the mapping */
        lo = pct(v->hist, total, 10) << 8;
        hi = (pct(v->hist, total, 995) + 1) << 8;
        v->map_lo_q8 += (lo - v->map_lo_q8) / 4;
        v->map_hi_q8 += (hi - v->map_hi_q8) / 4;
    }
apply:
    if (v->map_hi_q8 - v->map_lo_q8 < 3 * 256) v->map_hi_q8 = v->map_lo_q8 + 3 * 256;
    v->map_off = v->map_lo_q8 >> 6;                       /* Q8 -> Q2 */
    v->map_gain = (255 << 16) / ((v->map_hi_q8 - v->map_lo_q8) >> 6);
    for (int sum = 0; sum < 1024; sum++) {
        int p = ((sum - v->map_off) * v->map_gain) >> 16;
        v->lut[sum] = p < 0 ? 0 : p > 255 ? 255 : (uint8_t)p;
    }
}

/* Hand the field to the callback once: right after its last picture row
 * (no waiting for the next vertical sync), or at the field start. */
static void deliver(video_t *v)
{
    if (v->delivered || !v->cb) return;
    v->delivered = true;
    bool render_next = true;
    uint8_t *next = v->cb(v->fb, VIDEO_W, v->std == VIDEO_STD_PAL ? 288 : 240, v->render, &render_next, v->cb_ctx);
    if (next) v->fb = next;
    v->render = render_next;
}

static void field_start(video_t *v, bool vsync)
{
    int n = v->line_in_field;
    v->lines_per_field = n;
    if (vsync) {
        video_std_t s = (n >= 290 && n <= 330) ? VIDEO_STD_PAL
                      : (n >= 245 && n < 290)  ? VIDEO_STD_NTSC : VIDEO_STD_UNKNOWN;
        if (s != VIDEO_STD_UNKNOWN && s != v->std) {
            if (++v->std_votes >= 3) {
                v->std = s;
                v->std_votes = 0;
            }
        } else {
            v->std_votes = 0;
        }
        v->v_locked = s != VIDEO_STD_UNKNOWN;
    } else {
        v->v_locked = false;
    }
    update_mapping(v);
    memset(v->hist, 0, sizeof v->hist);
    v->fields++;
    deliver(v);   /* a short or free-running field that never reached its last row */
    v->delivered = false;
    v->line_in_field = 0;
}

static VIDEO_HOT void render_line(video_t *v, int row)
{
    /* Built in internal RAM, then copied in one go: the frame is in PSRAM. */
    uint8_t line[VIDEO_W] __attribute__((aligned(4)));
    const int s = v->std == VIDEO_STD_PAL;
    const int64_t start = v->t_edge + v->h_start[s];
    const uint32_t base = (uint32_t)(start >> 8);
    const uint32_t step = v->h_step[s];
    const uint32_t pos0 = (uint32_t)(start & 255) + (step >> 1);
    uint32_t pos = pos0;
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    const uint8_t *lut = v->lut;
    /* [1 2 1]/4 low-pass: -15 dB at the NTSC colour subcarrier (10 MS/s).
     * The sum of the three taps (4x the code) indexes the level LUT. */
    const uint32_t first = base & m, last = first + ((pos0 + step * (VIDEO_W - 1)) >> 8) + 1;
    if (first >= 1 && last < v->ring_size) {
        const uint8_t *q = r + first;   /* the whole line is contiguous in the ring */
        for (int x = 0; x < VIDEO_W; x++, pos += step) {
            const uint8_t *s = q + (pos >> 8);
            line[x] = lut[s[-1] + 2 * s[0] + s[1]];
        }
    } else {
        for (int x = 0; x < VIDEO_W; x++, pos += step) {
            uint32_t i = base + (pos >> 8);
            line[x] = lut[(r[(i - 1) & m] & 63) + 2 * (r[i & m] & 63) + (r[(i + 1) & m] & 63)];
        }
    }
    memcpy(v->fb + row * VIDEO_W, line, VIDEO_W);
    if ((row & 3) == 0) {
        /* Auto-contrast statistics from every fourth line, on the same
         * filtered values the pixels show (raw samples would widen the
         * percentiles by the noise and wash the picture out). */
        uint32_t *hist = v->hist;
        pos = pos0;
        for (int x = 0; x < VIDEO_W; x += 2, pos += 2 * step) {
            const uint32_t i = base + (pos >> 8);
            hist[((r[(i - 1) & m] & 63) + 2 * (r[i & m] & 63) + (r[(i + 1) & m] & 63)) >> 2]++;
        }
    }
}

static void restart_tracking(video_t *v, int64_t pos)
{
    if (v->h_locked) v->lock_losses++;
    v->h_locked = false;
    v->have_prev = false;
    v->good = v->miss = 0;
    v->cur_run = 0;
    v->t_edge = pos << 8;
}

static VIDEO_HOT void process_line(video_t *v)
{
    const int64_t t0 = v->t_edge;
    const int64_t base = t0 >> 8;
    const bool broad = below(v, base + v->c_broad1) && below(v, base + v->c_broad2);

    /* The first regular line after a run of broad pulses starts a field. */
    if (broad) {
        v->broad_run++;
    } else {
        if (v->broad_run >= 2) field_start(v, true);
        v->broad_run = 0;
    }
    if (v->line_in_field >= FREE_RUN_LINES) field_start(v, false);

    const bool pal = v->std == VIDEO_STD_PAL;
    const int rows = pal ? 288 : 240;
    const int row = v->line_in_field - (pal ? v->active_first_pal : v->active_first_ntsc) - v->v_offset;
    if (!broad && row >= 0 && row < rows) {
        if (v->render) render_line(v, row);
        if (row == rows - 1) deliver(v);
    }

    /* Levels only from lines that started with a real line sync. */
    if (v->h_locked && !broad && v->cur_run >= v->c_min_low_unlocked && v->cur_run <= v->c_hs_max) {
        int32_t s = avg_q8(v, base + v->c_sync_a, v->c_sync_b - v->c_sync_a);
        int32_t b = avg_q8(v, base + v->c_porch_a, v->c_porch_b - v->c_porch_a);
        v->sync_q8 += (s - v->sync_q8) / 16;
        v->blank_q8 += (b - v->blank_q8) / 16;
        if (v->blank_q8 - v->sync_q8 >= 2 * 256) {
            v->slice_q8 = (v->sync_q8 + v->blank_q8) / 2;
            v->levels_valid = true;
        }
    }

    /* Next line's sync. */
    int64_t edge;
    int run;
    if (v->h_locked) {
        const int64_t pred = t0 + v->period;
        const int64_t from = (pred - v->win) >> 8;
        if (find_edge(v, from, (int)(((pred + v->win) >> 8) - from), v->c_min_low_locked, &edge, &run)) {
            const int64_t err = edge - pred;
            v->t_edge = pred + err / 2;
            v->period += err / 32;
            if (v->period < v->p_min) v->period = v->p_min;
            if (v->period > v->p_max) v->period = v->p_max;
            v->miss = 0;
            v->cur_run = run;
        } else {
            v->t_edge = pred;   /* flywheel */
            v->cur_run = 0;
            if (++v->miss > LOCK_MISS) restart_tracking(v, pred >> 8);
        }
    } else {
        if (v->unlocked_lines++ % 64 == 0) estimate_slice(v);
        const int64_t from = base + v->c_skip;
        const int n = (int)((v->period * 3 / 2) >> 8);
        if (find_edge_coarse(v, from, n, v->c_min_low_unlocked, &edge, &run)) {
            if (v->have_prev) {
                const int64_t d = edge - v->prev_edge;
                if (d >= v->p_min && d <= v->p_max) {
                    v->good++;
                    v->period = (v->period * 3 + d) / 4;
                } else {
                    v->good = 0;
                }
            }
            v->prev_edge = edge;
            v->have_prev = true;
            v->t_edge = edge;
            v->cur_run = run;
            if (v->good >= LOCK_GOOD) {
                v->h_locked = true;
                v->miss = 0;
            }
        } else {
            v->t_edge = (from + n) << 8;
            v->have_prev = false;
            v->good = 0;
            v->cur_run = 0;
        }
    }
    v->line_in_field++;
    v->lines++;
}

void video_init(video_t *v, const uint8_t *ring, uint32_t ring_size, uint32_t fs_hz,
                uint8_t *fb, video_field_cb_t cb, void *cb_ctx)
{
    memset(v, 0, sizeof *v);
    v->ring = ring;
    v->ring_size = ring_size;
    v->mask = ring_size - 1;
    v->fs_hz = fs_hz;
    v->fb = fb;
    v->cb = cb;
    v->cb_ctx = cb_ctx;

    v->p_ntsc = us_q8(fs_hz, 63.5556);
    v->p_pal = us_q8(fs_hz, 64.0);
    v->p_min = us_q8(fs_hz, 62.0);
    v->p_max = us_q8(fs_hz, 65.5);
    v->win = us_q8(fs_hz, 2.0);
    v->c_skip = us_n(fs_hz, 1.0);
    v->c_min_low_locked = us_n(fs_hz, 1.5);
    v->c_min_low_unlocked = us_n(fs_hz, 3.5);
    v->c_hs_max = us_n(fs_hz, 7.5);
    v->c_max_run = us_n(fs_hz, 8.0);
    v->c_broad1 = us_n(fs_hz, 10.0);
    v->c_broad2 = us_n(fs_hz, 20.0);
    v->c_sync_a = us_n(fs_hz, 1.0);
    v->c_sync_b = us_n(fs_hz, 3.5);
    v->c_porch_a = us_n(fs_hz, 5.8);
    v->c_porch_b = us_n(fs_hz, 8.8);
    v->c_tail = us_n(fs_hz, 10.0);
    v->c_margin = (int)(ring_size / 8);
    /* Active picture: NTSC starts 9.4 us and PAL 10.5 us after the sync
     * edge, 52.6 / 52.0 us long; trim 0.2 us at each side. */
    v->h_start_base[0] = us_q8(fs_hz, 9.6);
    v->h_start_base[1] = us_q8(fs_hz, 10.7);
    v->h_step[0] = (uint32_t)(us_q8(fs_hz, 52.2) / VIDEO_W);
    v->h_step[1] = (uint32_t)(us_q8(fs_hz, 51.6) / VIDEO_W);
    v->h_start[0] = v->h_start_base[0];
    v->h_start[1] = v->h_start_base[1];
    v->active_first_ntsc = 14;   /* line 21 counted from the end of the broad pulses */
    v->active_first_pal = 20;    /* line 24 */
    v->std = VIDEO_STD_NTSC;
    v->levels = VIDEO_LEVELS_AUTO;
    v->render = true;
    video_reset(v, 0);
}

void video_reset(video_t *v, uint32_t write_pos)
{
    v->wr32 = write_pos;
    v->wr = write_pos;
    v->reset_pos = write_pos;
    v->period = v->p_ntsc;
    v->h_locked = false;
    restart_tracking(v, v->wr);
    v->unlocked_lines = 0;
    v->slice_q8 = 128;   /* nothing below it until estimate_slice() has data */
    v->sync_q8 = 0;
    v->blank_q8 = 256;
    v->levels_valid = false;
    v->map_lo_q8 = 0;
    v->map_hi_q8 = 63 << 8;
    update_mapping(v);
    memset(v->hist, 0, sizeof v->hist);
    v->line_in_field = 0;
    v->broad_run = 0;
    v->std_votes = 0;
    v->v_locked = false;
}

void video_process(video_t *v, uint32_t write_pos)
{
    v->wr += (int32_t)(write_pos - v->wr32);
    v->wr32 = write_pos;
    for (;;) {
        const int64_t start = v->t_edge >> 8;
        if (start + ((v->period * 3 / 2) >> 8) + v->c_tail > v->wr) break;
        if (start - 4 < v->wr - (int64_t)v->ring_size + v->c_margin) {
            /* A ring behind the writer: those samples are being overwritten. */
            v->overruns++;
            restart_tracking(v, v->wr - (int64_t)v->ring_size / 4);
            continue;
        }
        const uint32_t t0 = NOW_US();
        const uint32_t fields = v->fields;
        const bool unlocked = !v->h_locked, rendering = v->render;
        process_line(v);
        const uint32_t dt = NOW_US() - t0;
        if (dt > v->max_line_us) {
            v->max_line_us = dt;
            v->max_line_kind = (rendering ? 1u : 0u) | (v->fields != fields ? 2u : 0u) | (unlocked ? 4u : 0u);
        }
    }
}

void video_get_stats(const video_t *v, video_stats_t *o)
{
    memset(o, 0, sizeof *o);
    o->h_locked = v->h_locked;
    o->v_locked = v->v_locked;
    o->std = v->std;
    o->lines_per_field = v->lines_per_field;
    o->line_us = (float)((double)v->period / 256.0 / v->fs_hz * 1e6);
    o->sync_level = v->sync_q8 / 256.0f;
    o->blank_level = v->blank_q8 / 256.0f;
    o->black_level = v->map_lo_q8 / 256.0f;
    o->white_level = v->map_hi_q8 / 256.0f;
    o->fields = v->fields;
    o->lines = v->lines;
    o->overruns = v->overruns;
    o->lock_losses = v->lock_losses;
    o->h_offset_tenths_us = v->h_offset_tenths;
    o->v_offset_lines = v->v_offset;
    o->levels = v->levels;
}

void video_shift_h(video_t *v, int tenths_us)
{
    int t = v->h_offset_tenths + tenths_us;
    v->h_offset_tenths = t < -50 ? -50 : t > 50 ? 50 : t;
    for (int s = 0; s < 2; s++) v->h_start[s] = v->h_start_base[s] + us_q8(v->fs_hz, v->h_offset_tenths / 10.0);
}

void video_shift_v(video_t *v, int lines)
{
    int t = v->v_offset + lines;
    v->v_offset = t < -20 ? -20 : t > 20 ? 20 : t;
}

void video_set_levels(video_t *v, video_levels_t mode)
{
    v->levels = mode;
}
