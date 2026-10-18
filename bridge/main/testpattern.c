/* Synthetic interlaced NTSC/PAL composite source, see testpattern.h. */
#include "testpattern.h"
#include <stdlib.h>
#include <string.h>

/* What each half of a line carries. */
typedef enum { H_PIC, H_BLANK, H_EQ, H_BROAD } half_t;

static int us_n(uint32_t fs_hz, double us) { return (int)(us * fs_hz / 1e6 + 0.5); }

static void fill(uint8_t *p, int from, int to, uint8_t level)
{
    if (to > from) memset(p + from, level, (size_t)(to - from));
}

/* Line structure of a frame (1-based line numbers, as in the standards).
 * Sets the half types and, for picture lines, the row within the field. */
static void line_kind(const testpattern_t *t, int L, half_t *l, half_t *r, int *row)
{
    *row = 0;
    if (!t->pal) {
        if (L <= 3 || (L >= 7 && L <= 9) || L == 264 || L == 265 || L == 270 || L == 271) { *l = *r = H_EQ; return; }
        if ((L >= 4 && L <= 6) || L == 267 || L == 268) { *l = *r = H_BROAD; return; }
        if (L == 266) { *l = H_EQ; *r = H_BROAD; return; }
        if (L == 269) { *l = H_BROAD; *r = H_EQ; return; }
        if (L == 272) { *l = H_EQ; *r = H_BLANK; return; }
        if (L == 263) { *l = H_PIC; *r = H_EQ; *row = 242; return; }
        if (L == 283) { *l = H_BLANK; *r = H_PIC; return; }
        if (L >= 21 && L <= 262) { *l = *r = H_PIC; *row = L - 21; return; }
        if (L >= 284) { *l = *r = H_PIC; *row = L - 284; return; }
    } else {
        if (L <= 2 || L == 314 || L == 315) { *l = *r = H_BROAD; return; }
        if (L == 3) { *l = H_BROAD; *r = H_EQ; return; }
        if (L == 313) { *l = H_EQ; *r = H_BROAD; return; }
        if (L == 4 || L == 5 || L == 311 || L == 312 || L == 316 || L == 317 || L >= 624) { *l = *r = H_EQ; return; }
        if (L == 318) { *l = H_EQ; *r = H_BLANK; return; }
        if (L == 623) { *l = H_PIC; *r = H_EQ; *row = 287; return; }
        if (L == 23) { *l = H_BLANK; *r = H_PIC; return; }
        if (L >= 24 && L <= 310) { *l = *r = H_PIC; *row = L - 24; return; }
        if (L >= 336) { *l = *r = H_PIC; *row = L - 336; return; }
    }
    *l = *r = H_BLANK;
}

bool testpattern_init(testpattern_t *t, uint8_t *ring, uint32_t ring_size, uint32_t fs_hz, bool pal)
{
    return testpattern_init_levels(t, ring, ring_size, fs_hz, pal, TP_SYNC, TP_BLANK, TP_WHITE, false);
}

bool testpattern_init_levels(testpattern_t *t, uint8_t *ring, uint32_t ring_size, uint32_t fs_hz, bool pal,
                             int sync, int blank, int white, bool dark_left)
{
    memset(t, 0, sizeof *t);
    t->lv_sync = sync;
    t->lv_blank = blank;
    t->lv_white = white;
    t->ring = ring;
    t->mask = ring_size - 1;
    t->fs_hz = fs_hz;
    t->pal = pal;
    const double line_us = pal ? 64.0 : 63.5556;
    t->line_q16 = (uint32_t)(line_us * fs_hz / 1e6 * 65536.0 + 0.5);
    t->len_max = (int)(t->line_q16 >> 16) + 2;
    t->half = (int)(t->line_q16 >> 17);
    t->lines = pal ? 625 : 525;
    t->x0 = us_n(fs_hz, pal ? 10.5 : 9.4);
    t->x_len = us_n(fs_hz, pal ? 52.0 : 52.6);
    t->rng = 0x12345678u;

    const int n = t->len_max;
    uint8_t **tpl[] = {&t->t_eq, &t->t_broad, &t->t_blank, &t->t_bars, &t->t_ramp, &t->pic, &t->buf};
    for (size_t k = 0; k < sizeof tpl / sizeof tpl[0]; k++) {
        *tpl[k] = malloc((size_t)n);
        if (!*tpl[k]) {
            testpattern_free(t);
            return false;
        }
        memset(*tpl[k], (uint8_t)blank, (size_t)n);
    }
    const int half = t->half;
    const int hs = us_n(fs_hz, 4.7), eq = us_n(fs_hz, pal ? 2.35 : 2.3);

    fill(t->t_eq, 0, eq, (uint8_t)sync);
    fill(t->t_eq, half, half + eq, (uint8_t)sync);
    fill(t->t_broad, 0, half - hs, (uint8_t)sync);
    fill(t->t_broad, half, 2 * half - hs, (uint8_t)sync);
    fill(t->t_blank, 0, hs, (uint8_t)sync);

    memcpy(t->t_bars, t->t_blank, (size_t)n);
    memcpy(t->t_ramp, t->t_blank, (size_t)n);
    for (int x = 0; x < t->x_len; x++) {
        const int xx = dark_left ? t->x_len - 1 - x : x;
        int bar = xx * 8 / t->x_len;                      /* white .. black in 8 steps */
        t->t_bars[t->x0 + x] = (uint8_t)(white - (white - blank) * bar / 7);
        t->t_ramp[t->x0 + x] = (uint8_t)(blank + (white - blank) * xx / (t->x_len - 1));
    }
    return true;
}

void testpattern_free(testpattern_t *t)
{
    uint8_t **tpl[] = {&t->t_eq, &t->t_broad, &t->t_blank, &t->t_bars, &t->t_ramp, &t->pic, &t->buf};
    for (size_t k = 0; k < sizeof tpl / sizeof tpl[0]; k++) {
        free(*tpl[k]);
        *tpl[k] = NULL;
    }
}

void testpattern_restart(testpattern_t *t, uint32_t pos)
{
    t->pos = pos;
    t->acc_q16 = 0;
    t->line = 0;
}

static void ring_write(testpattern_t *t, const uint8_t *src, uint32_t n)
{
    uint32_t off = t->pos & t->mask;
    uint32_t first = t->mask + 1 - off;
    if (first > n) first = n;
    memcpy(t->ring + off, src, first);
    if (n > first) memcpy(t->ring, src + first, n - first);
    t->pos += n;
}

/* Picture line `row` of a field: bars in the top two thirds, a ramp below,
 * and a box bouncing left and right over rows 40..79. */
static const uint8_t *picture_line(testpattern_t *t, int row, uint32_t len)
{
    const int rows = t->pal ? 288 : 240;
    const uint8_t *tpl = row < rows * 2 / 3 ? t->t_bars : t->t_ramp;
    const int box_rows = 40, box_w = t->x_len / 8;
    if (row < 40 || row >= 40 + box_rows) return tpl;
    memcpy(t->pic, tpl, len);
    const int travel = t->x_len - box_w;
    int phase = (int)(t->field * 2u % (uint32_t)(2 * travel));
    int bx = t->x0 + (phase < travel ? phase : 2 * travel - phase);
    bool edge = row == 40 || row == 40 + box_rows - 1;
    fill(t->pic, bx, bx + box_w, (uint8_t)(edge ? t->lv_blank : t->lv_white));
    fill(t->pic, bx, bx + 2, (uint8_t)t->lv_blank);
    fill(t->pic, bx + box_w - 2, bx + box_w, (uint8_t)t->lv_blank);
    return t->pic;
}

static const uint8_t *half_src(testpattern_t *t, half_t k, int row, uint32_t len)
{
    switch (k) {
    case H_PIC:   return picture_line(t, row, len);
    case H_EQ:    return t->t_eq;
    case H_BROAD: return t->t_broad;
    default:      return t->t_blank;   /* left: line sync + blanking, right: blanking */
    }
}

static void emit_line(testpattern_t *t)
{
    const uint32_t sum = t->acc_q16 + t->line_q16;
    const uint32_t len = sum >> 16;
    t->acc_q16 = sum & 0xffffu;

    const int L = t->line + 1;
    if (L == 1 || L == (t->pal ? 313 : 263)) t->field++;
    half_t l, r;
    int row;
    line_kind(t, L, &l, &r, &row);
    const uint8_t *a = half_src(t, l, row, len);
    const uint8_t *b = r == l ? a : half_src(t, r, row, len);
    const uint8_t *src = a;
    if (a != b) {
        memcpy(t->buf, a, (size_t)t->half);
        memcpy(t->buf + t->half, b + t->half, len - (uint32_t)t->half);
        src = t->buf;
    }
    if (t->noise > 0 || t->clicks > 0) {
        if (src != t->buf) memcpy(t->buf, src, len);
        const int span = 2 * t->noise + 1;
        for (uint32_t k = 0; k < len; k++) {
            t->rng = t->rng * 1664525u + 1013904223u;
            int v = t->buf[k];
            if (t->noise > 0) v += (int)((t->rng >> 16) % (uint32_t)span) - t->noise;
            if (t->clicks > 0 && (t->rng >> 8) % 10000u < (uint32_t)t->clicks) {
                /* FM click: the phase slips, the output spikes for 1-2 samples */
                v = (t->rng & 1u) ? 63 : 0;
                if ((t->rng & 2u) && k + 1 < len) t->buf[++k] = (uint8_t)v;
            }
            t->buf[k] = (uint8_t)(v < 0 ? 0 : v > 63 ? 63 : v);
        }
        src = t->buf;
    }
    ring_write(t, src, len);
    if (++t->line >= t->lines) t->line = 0;
}

void testpattern_run_until(testpattern_t *t, uint32_t target)
{
    while ((int32_t)(target - t->pos) > 0) emit_line(t);
}
