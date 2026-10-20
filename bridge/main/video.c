/* SPDX-License-Identifier: GPL-3.0-only */
/* Composite video decoder for VRX Bridge: line PLL on the sync
 * edges, vertical sync from broad pulses, one grayscale or (NTSC) colour
 * frame per field. See video.h. */
#include "video.h"
#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
/* Per-line code in internal RAM: never waits on the flash cache that core 0
 * (Wi-Fi, JPEG) shares. noinline: a static function inlined into a caller
 * in flash ends up in flash (process_line once did, inside video_process). */
#define VIDEO_HOT IRAM_ATTR __attribute__((noinline))
#define NOW_US() ((uint32_t)esp_timer_get_time())
#else
#define VIDEO_HOT
#define NOW_US() 0u
#endif

#define FREE_RUN_LINES 400    /* no vertical sync after this many lines: emit the field anyway */
#define FSC_HZ (315e6 / 88)   /* NTSC colour subcarrier, 3.579545 MHz */
#define TWO_PI 6.283185307179586
/* Subcarrier table amplitude: with samples of 0..63, blocks of up to 16
 * samples and a zero sum per block, both 16-bit halves of a packed sum
 * stay in range. */
#define CTAB_PEAK 31
#define COS2_STEP 0.671f      /* cos^2(35 deg): a burst further off the reference is a phase step */
#define WEAK_BURSTS 200       /* lines in a row without a burst that drop the reference */
#define LOCK_GOOD      5      /* consecutive line-spaced syncs needed to lock */
#define LOCK_MISS      30     /* consecutive missing syncs that drop the lock */

static int32_t us_q8(uint32_t fs_hz, double us) { return (int32_t)(us * fs_hz / 1e6 * 256.0 + 0.5); }
static int us_n(uint32_t fs_hz, double us) { return (int)(us * fs_hz / 1e6 + 0.5); }

/* Colour burst sums (colour_burst) per DAC code of burst amplitude. */
static inline __attribute__((always_inline)) float burst_unit(const video_t *v)
{
    return CTAB_PEAK / 2.0f * (float)(v->c_bblk * v->c_bblocks);
}

/* Sync detection works on the signal averaged over c_box samples (0.4 us),
 * centred on the position. On the C5 the sync tip is only ~6 codes below
 * blanking with ~3 codes of noise per sample: 7 % of sync-tip samples lie
 * above the slicer (board recording), so single samples chopped every line
 * sync into short runs and the line lock kept dropping. The average has a
 * third of the noise and still follows a 4.7 us sync edge to the sample. */
static inline __attribute__((always_inline)) int32_t box_sum(const video_t *v, uint32_t centre)
{
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    uint32_t i = centre - (uint32_t)(v->c_box / 2);
    int32_t s = 0;
    for (int j = 0; j < v->c_box; j++, i++) s += r[i & m] & 63;
    return s;
}

static inline __attribute__((always_inline)) bool box_below(const video_t *v, int64_t pos)
{
    return (box_sum(v, (uint32_t)pos) << 8) < v->slice_q8 * v->c_box;
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

/* First falling slicer crossing of the averaged signal in [from, from + n)
 * whose low run lasts at least min_low samples; highs of up to c_box + 2
 * samples inside the run (FM clicks) are bridged. The edge is interpolated
 * to 1/256 sample. */
static VIDEO_HOT bool find_edge(const video_t *v, int64_t from, int n, int min_low,
                                int64_t *edge_q8, int *run_out)
{
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    const int w = v->c_box;
    const int32_t thr = v->slice_q8 * w;
    const int gap_max = w + 2;
    /* Running box sum: lo is the oldest sample in the window centred on
     * position from + k - 1, hi the next sample to enter. */
    uint32_t lo = (uint32_t)from - 1 - (uint32_t)(w / 2), hi = lo + (uint32_t)w;
    int32_t sum = box_sum(v, (uint32_t)from - 1);
    int32_t prev = sum << 8;
    for (int k = 0; k < n; k++, lo++, hi++) {
        sum += (r[hi & m] & 63) - (r[lo & m] & 63);
        const int32_t cur = sum << 8;
        if (prev >= thr && cur < thr) {
            int run = 1, len = 1, gap = 0;
            int32_t s = sum;
            for (uint32_t a = lo + 1, b = hi + 1; len < v->c_max_run; a++, b++) {
                s += (r[b & m] & 63) - (r[a & m] & 63);
                len++;
                if ((s << 8) < thr) {
                    run = len;
                    gap = 0;
                } else if (++gap > gap_max) {
                    break;
                }
            }
            if (run >= min_low) {
                /* an even-length window centred on c spans c - w/2 .. c + w/2 - 1 */
                *edge_q8 = ((from + k - 1) << 8) + ((prev - thr) << 8) / (prev - cur) + (w % 2 ? 0 : 128);
                *run_out = run;
                return true;
            }
            /* Short dip (noise, equalising pulse while unlocked): go on. */
        }
        prev = cur;
    }
    return false;
}

/* Same result as find_edge() for syncs of at least min_low samples, but
 * probes only every min_low/2-th position: a low run that long always covers
 * a probe whose predecessor probe lies before the run. Used while unlocked,
 * where most samples are picture or noise. find_edge() then pins down the
 * crossing between the two probes. */
static VIDEO_HOT bool find_edge_coarse(const video_t *v, int64_t from, int n, int min_low,
                                       int64_t *edge_q8, int *run_out)
{
    const int st = min_low / 2 > 1 ? min_low / 2 : 1;
    /* Crossings only after `from` (k - st >= 0): when called just past a
     * sync edge, `from` lies inside that pulse and must not find it again. */
    bool prev_low = box_below(v, from);
    for (int k = st; k < n; k += st) {
        const bool low = box_below(v, from + k);
        if (low && !prev_low && find_edge(v, from + k - st + 1, st, min_low, edge_q8, run_out)) return true;
        prev_low = low;
    }
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
    for (int sum = 0; sum < 256; sum++) {   /* line[] holds 0..252 */
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
    uint8_t *next = v->cb(v->fb, VIDEO_W, v->std == VIDEO_STD_PAL ? 288 : 240,
                          v->colour_field ? VIDEO_FMT_COLOUR : VIDEO_FMT_GRAY, v->render, &render_next, v->cb_ctx);
    if (next) v->fb = next;
    v->render = render_next;
}

static void field_start(video_t *v, bool vsync)
{
    const int n = v->line_in_field;
    /* A field forced while vertically locked (missed sync) keeps the line
     * phase: it gets the length of the field two back (interlaced fields
     * alternate 262/263 or 312/313 lines) and the next field continues
     * from the lines past it. Restarting at 0 put every later field 8
     * lines late, the real sync was then rejected as early and the picture
     * rolled for 4-5 fields after any fade or channel change. */
    const int expect = v->std == VIDEO_STD_PAL ? 312 : 262;
    int len = n, carry = 0;
    if (!vsync && v->v_locked) {
        const int two_back = v->field_len[1];
        len = two_back >= expect && two_back <= expect + 1 ? two_back : expect;
        carry = n > len ? n - len : 0;
    }
    v->field_len[1] = v->field_len[0];
    v->field_len[0] = len;
    v->lines_per_field = len;
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
        /* Locked only once the field length matches the standard in use:
         * the flywheel below expects that standard's field length. */
        v->v_locked = s != VIDEO_STD_UNKNOWN && s == v->std;
        v->v_miss = 0;
    } else if (++v->v_miss > 3) {
        v->v_locked = false;   /* flywheel: a few missed vertical syncs keep the lock */
    }
    update_mapping(v);
    memset(v->hist, 0, sizeof v->hist);
    v->fields++;
    deliver(v);   /* a short or free-running field that never reached its last row */
    v->delivered = false;
    v->line_in_field = carry;
    /* Colour for the coming field: on, NTSC, a burst (on above 0.8 codes,
     * off below 0.5) and levels to scale the chroma by. */
    const float lim = (v->burst_ok ? 0.5f : 0.8f) * burst_unit(v);
    const float rp = v->ref_re * v->ref_re + v->ref_im * v->ref_im;
    v->burst_ok = v->have_ref && rp > lim * lim;
    /* Burst samples within blanking +- (2 bursts + 4 codes): the burst and
     * its noise pass, a click is cut down. */
    v->burst_clip = v->have_ref ? (int)(2.0f * sqrtf(rp) / burst_unit(v) + 4.5f) : 12;
    v->colour_field = v->colour && v->std == VIDEO_STD_NTSC && v->burst_ok && v->blank_q8 - v->sync_q8 >= 2 * 256;
    if (v->colour_field) {
        /* Chroma sums / reference = chroma / burst amplitude * (burst samples
         * / block samples); the burst is 20 IRE and blanking - sync 40 IRE,
         * so in luma 0..1 units: * (blank - sync) / 2 / (white - black). */
        v->chroma_gain = (float)(v->blank_q8 - v->sync_q8) / (2.0f * (float)(v->map_hi_q8 - v->map_lo_q8)) *
                         (float)(v->c_bblk * v->c_bblocks) / (float)v->c_cblk;
    }
}

/* ---- Colour (NTSC), docs/colour-study.md ---------------------------------
 * The C5's output has only 24 levels (~2.7 codes a step) and ~2 codes of
 * noise, and its 20 MS/s samples drift against the S3's clock (one sample
 * duplicated or dropped every few tens of ms on the board: the subcarrier
 * phase steps by 64 degrees). Per line: the burst (3 blocks from 5.5 us,
 * 8 cycles at 20 MS/s) against the subcarrier at the absolute sample index;
 * an average of the bursts (1/8 per line) as the phase and amplitude
 * reference, with a burst more than 35 degrees off used for its own line
 * only and a second one in a row restarting the average (the slip);
 * chroma sums over VIDEO_CW blocks of the even picture rows (16 samples at
 * 20 MS/s, SIMD), rotated by the reference into the frame's Cb/Cr plane. */

/* Packed cos/sin tables for `blocks` blocks of n samples from phase 0, with
 * a zero sum per block: the rounding residue goes to the entries rounded
 * furthest the other way. */
static void fill_blocks(int32_t *tab, int n, int blocks, double w0)
{
    for (int b = 0; b < blocks; b++) {
        double f[2][16], mean[2] = {0, 0};
        int q[2][16];
        for (int k = 0; k < n; k++) {
            const double ph = w0 * (b * n + k);
            f[0][k] = CTAB_PEAK * cos(ph);
            f[1][k] = CTAB_PEAK * sin(ph);
            mean[0] += f[0][k] / n;
            mean[1] += f[1][k] / n;
        }
        for (int c = 0; c < 2; c++) {
            int sum = 0;
            for (int k = 0; k < n; k++) {
                f[c][k] -= mean[c];
                q[c][k] = (int)lround(f[c][k]);
                sum += q[c][k];
            }
            while (sum) {
                int best = 0;
                double best_e = -1e9;
                for (int k = 0; k < n; k++) {
                    const double e = sum > 0 ? q[c][k] - f[c][k] : f[c][k] - q[c][k];
                    if (e > best_e) {
                        best_e = e;
                        best = k;
                    }
                }
                q[c][best] += sum > 0 ? -1 : 1;
                sum += sum > 0 ? -1 : 1;
            }
        }
        for (int k = 0; k < n; k++) tab[b * n + k] = q[0][k] * 65536 + q[1][k];
    }
}

/* The chroma tables: int8 cos and sin per sample of the chroma span, from
 * the packed blocks (zero sum per block). */
static void fill_chroma(video_t *v, double w0)
{
    static int32_t packed[VIDEO_CSPAN];   /* 4 KiB: not on the caller's stack */
    fill_blocks(packed, v->c_cblk, VIDEO_CW, w0);
    for (int j = 0; j < v->c_cblk * VIDEO_CW; j++) {
        const int32_t sn = (int16_t)packed[j];
        v->csin[j] = (int8_t)sn;
        v->ccos[j] = (int8_t)((packed[j] - sn) >> 16);
    }
}

/* Chroma sums of the copied span: out[2k] = sum of x * cos, out[2k + 1] =
 * sum of x * sin over block k (n samples). */
static VIDEO_HOT void chroma_sums_c(const uint8_t *x, const int8_t *c, const int8_t *s, int n, int blocks, int32_t *out)
{
    for (int b = 0; b < blocks; b++, x += n, c += n, s += n) {
        int32_t a = 0, q = 0;
        for (int k = 0; k < n; k++) {
            a += x[k] * c[k];
            q += x[k] * s[k];
        }
        out[2 * b] = a;
        out[2 * b + 1] = q;
    }
}

#ifdef ESP_PLATFORM
/* chroma_pie.S: the same for 16-sample blocks with the S3's 16-lane
 * multiply-accumulate; x, c and s 16-byte aligned. */
void chroma_sums_pie(const uint8_t *x, const int8_t *c, const int8_t *s, int blocks, int32_t *out);

/* SIMD against C on a pseudo-random line; false: use C. */
static bool chroma_selftest(video_t *v)
{
    if (v->c_cblk != 16) return false;
    uint32_t r = 12345;
    for (int j = 0; j < VIDEO_CSPAN + 16; j++) {
        r = r * 1664525u + 1013904223u;
        v->cline[j] = (uint8_t)(r >> 26);   /* 0..63 */
    }
    int32_t a[2 * VIDEO_CW], b[2 * VIDEO_CW];
    chroma_sums_c(v->cline, v->ccos, v->csin, 16, VIDEO_CW, a);
    chroma_sums_pie(v->cline, v->ccos, v->csin, VIDEO_CW, b);
    return memcmp(a, b, sizeof a) == 0;
}
#endif

static void colour_init(video_t *v)
{
    const double w0 = TWO_PI * FSC_HZ / v->fs_hz;
    v->sc_inc = (uint32_t)llround(FSC_HZ / v->fs_hz * 4294967296.0);
    for (int k = 0; k < 256; k++) v->sc_cos[k] = (float)cos(TWO_PI * k / 256);
    v->c_burst = us_n(v->fs_hz, 5.5);
    v->c_bblk = us_n(v->fs_hz, 0.75);
    if (v->c_bblk > 16) v->c_bblk = 16;
    v->c_bblocks = 3;
    if (v->c_bblk * v->c_bblocks > VIDEO_BTAB) v->c_bblk = VIDEO_BTAB / v->c_bblocks;
    fill_blocks(v->btab, v->c_bblk, v->c_bblocks, w0);
    /* Chroma: VIDEO_CW blocks over the middle of the picture (16 samples,
     * 0.8 us at 20 MS/s: -3 dB at about 0.55 MHz). */
    const int active = (int)(us_q8(v->fs_hz, 52.2) >> 8);
    v->c_cblk = active / VIDEO_CW;
    if (v->c_cblk > VIDEO_CSPAN / VIDEO_CW) v->c_cblk = VIDEO_CSPAN / VIDEO_CW;
    v->c_coff = (active - v->c_cblk * VIDEO_CW) / 2;
    fill_chroma(v, w0);
#ifdef ESP_PLATFORM
    v->pie = chroma_selftest(v);
#endif
}

/* cos and sin of the subcarrier phase at absolute sample `pos`. */
static inline __attribute__((always_inline)) void sc_phase(const video_t *v, int64_t pos, float *c, float *s)
{
    const uint32_t i = ((uint32_t)pos * v->sc_inc) >> 24;
    *c = v->sc_cos[i];
    *s = v->sc_cos[(i - 64u) & 255u];
}

/* The burst of the line starting at t0 (Q8): updates the reference and
 * sets the one this line uses (line_re/im, line_ok). */
static VIDEO_HOT void colour_burst(video_t *v, int64_t t0)
{
    const int64_t o = (t0 >> 8) + v->c_burst;
    const uint32_t m = v->mask, first = (uint32_t)o & m;
    const int n = v->c_bblk, total = n * v->c_bblocks;
    const bool contiguous = first + (uint32_t)total <= v->ring_size;
    const int32_t *tab = v->btab;
    /* An FM click (a sample at 0 or 63) in the window would turn the burst
     * by tens of degrees: samples are limited to blanking +- burst_clip. */
    const int mid = (v->blank_q8 + 128) >> 8, lo = mid - v->burst_clip, hi = mid + v->burst_clip;
    int32_t c = 0, s = 0;
    for (int b = 0; b < v->c_bblocks; b++, tab += n) {
        int32_t acc = 0;
        const uint32_t at = (uint32_t)o + (uint32_t)(b * n);
        for (int k = 0; k < n; k++) {
            int x = contiguous ? v->ring[(at & m) + (uint32_t)k] : v->ring[(at + (uint32_t)k) & m];
            x = x < lo ? lo : x > hi ? hi : x;
            acc += x * tab[k];
        }
        const int32_t bs = (int16_t)acc;
        c += (acc - bs) >> 16;
        s += bs;
    }
    /* The burst is c - js at its window start; at the absolute index it is
     * that times e^(-j phase(o)). */
    float pc, ps;
    sc_phase(v, o, &pc, &ps);
    const float br = (float)c, bi = (float)-s;
    const float ar = br * pc + bi * ps, ai = bi * pc - br * ps;
    const float p = ar * ar + ai * ai;
    const float unit = burst_unit(v);
    v->line_ok = false;
    if (!v->have_ref) {
        if (p < 0.25f * unit * unit) return;   /* no burst (yet): a 0.5 code one starts the reference */
        v->ref_re = ar;
        v->ref_im = ai;
        v->have_ref = true;
        v->have_pend = false;
        v->weak_bursts = 0;
    }
    const float rp = v->ref_re * v->ref_re + v->ref_im * v->ref_im;
    v->line_re = v->ref_re;
    v->line_im = v->ref_im;
    v->line_ok = true;
    if (p < 0.25f * rp) {
        /* No burst on this line (vertical interval, a dropout): the
         * reference stays, and goes after many such lines (no colour). */
        if (++v->weak_bursts > WEAK_BURSTS) v->have_ref = v->line_ok = false;
        return;
    }
    v->weak_bursts = 0;
    const float dr = ar * v->ref_re + ai * v->ref_im, di = ai * v->ref_re - ar * v->ref_im;
    if (dr > 0 && dr * dr > COS2_STEP * (dr * dr + di * di)) {
        v->have_pend = false;
        v->ref_re += (ar - v->ref_re) * 0.125f;
        v->ref_im += (ai - v->ref_im) * 0.125f;
        v->line_re = v->ref_re;
        v->line_im = v->ref_im;
        return;
    }
    /* Far off the reference: this line uses its own burst; a second one in
     * a row, off to the same side and agreeing with it, is a step (a slip):
     * the average restarts. Noise outliers fall either way. */
    v->line_re = ar;
    v->line_im = ai;
    if (v->have_pend && (di < 0) == v->pend_back) {
        const float er = ar * v->pend_re + ai * v->pend_im, ei = ai * v->pend_re - ar * v->pend_im;
        if (er > 0 && er * er > COS2_STEP * (er * er + ei * ei)) {
            v->slips++;
            v->slip_sum += di < 0 ? 1 : -1;   /* phase back: a sample taken twice */
            v->ref_re = (ar + v->pend_re) * 0.5f;
            v->ref_im = (ai + v->pend_im) * 0.5f;
            v->have_pend = false;
            return;
        }
    }
    v->pend_re = ar;
    v->pend_im = ai;
    v->pend_back = di < 0;
    v->have_pend = true;
}

/* The chroma row `crow` (from picture row 2 * crow) into the frame's chroma
 * plane; the chroma blocks start at sample `start`. */
static VIDEO_HOT void render_chroma(video_t *v, uint32_t start, int crow)
{
    uint8_t out[VIDEO_CHROMA_ROW] __attribute__((aligned(4)));
    const int n = v->c_cblk, span = n * VIDEO_CW;
    if (v->line_ok && v->burst_ok) {
        /* The reference at this line's chroma origin, and the matrix from
         * the block sums (c, s) to Cb/Cr (Q16). With r the reference,
         * U = (s r_im - c r_re) / |r|^2 and V = (c r_im + s r_re) / |r|^2
         * (the NTSC burst is at 180 degrees); in luma 0..1 units,
         * Cb = 128 + 292.3 U and Cr = 128 + 207.3 V (JFIF Cb/Cr of NTSC U/V). */
        float pc, ps;
        sc_phase(v, start, &pc, &ps);
        const float rr = v->line_re * pc - v->line_im * ps, ri = v->line_im * pc + v->line_re * ps;
        const float q = v->chroma_gain * 65536.0f / (rr * rr + ri * ri);
        const float k[4] = {-292.3f * q * rr, 292.3f * q * ri, 207.3f * q * ri, 207.3f * q * rr};
        int32_t kq[4];
        for (int i = 0; i < 4; i++) kq[i] = (int32_t)(k[i] > 30000.0f ? 30000.0f : k[i] < -30000.0f ? -30000.0f : k[i]);
        /* The samples into an aligned buffer: no ring wrap, aligned SIMD loads. */
        const uint32_t m = v->mask, first = start & m, tail = v->ring_size - first;
        if ((uint32_t)span <= tail) {
            memcpy(v->cline, v->ring + first, (size_t)span);
        } else {
            memcpy(v->cline, v->ring + first, tail);
            memcpy(v->cline + tail, v->ring, (size_t)span - tail);
        }
        int32_t sums[2 * VIDEO_CW];
#ifdef ESP_PLATFORM
        if (v->pie) chroma_sums_pie(v->cline, v->ccos, v->csin, VIDEO_CW, sums);
        else
#endif
            chroma_sums_c(v->cline, v->ccos, v->csin, n, VIDEO_CW, sums);
        for (int b = 0; b < VIDEO_CW; b++) {
            const int32_t c = sums[2 * b], s = sums[2 * b + 1];
            const int32_t u = 128 + ((kq[0] * c + kq[1] * s) >> 16), w = 128 + ((kq[2] * c + kq[3] * s) >> 16);
            out[b] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
            out[VIDEO_CW + b] = (uint8_t)(w < 0 ? 0 : w > 255 ? 255 : w);
        }
    } else {
        memset(out, 128, sizeof out);
    }
    memcpy(v->fb + VIDEO_CHROMA_OFF + crow * VIDEO_CHROMA_ROW, out, VIDEO_CHROMA_ROW);
}

static VIDEO_HOT void render_line(video_t *v, int row)
{
    /* Built in internal RAM, then copied in one go: the frame is in PSRAM.
     * Each pixel's filtered level (4x the DAC code, 0..252) goes through the
     * level LUT in the same loop, except on every fourth line, whose levels
     * also feed the auto-contrast statistics: there a second pass maps them
     * (a separate pass on every line cost ~4 us of the 64 on the board). */
    uint8_t line[VIDEO_W] __attribute__((aligned(4)));
    const uint8_t *lut = v->lut;
    const bool stats = (row & 3) == 0;
    const int s = v->std == VIDEO_STD_PAL;
    const int64_t start = v->t_edge + v->h_start[s];
    const uint32_t base = (uint32_t)(start >> 8);
    const uint32_t step = v->h_step[s];
    const uint32_t pos0 = (uint32_t)(start & 255) + (step >> 1);
    uint32_t pos = pos0;
    const uint8_t *r = v->ring;
    const uint32_t m = v->mask;
    const bool median = v->filter == VIDEO_FILTER_MEDIAN;
    const int d = v->c_tap;                       /* smoothing taps 0.1 us apart */
    const uint32_t reach = d > 2 ? (uint32_t)d : 2u;   /* farthest tap of either filter */
    const uint32_t first = base & m, last = first + ((pos0 + step * (VIDEO_W - 1)) >> 8) + reach;
    if (first >= reach && last < v->ring_size) {
        const uint8_t *q = r + first;   /* the whole line is contiguous in the ring */
        if (median && !stats) {
            /* Median of three samples two apart: an FM click (1-2 samples)
             * reaches at most one of them, so it never wins. Half the cost
             * of a 5-tap median (board: overruns at 20 MS/s with it). */
            for (int x = 0; x < VIDEO_W; x++, pos += step) {
                const uint8_t *p = q + (pos >> 8);
                line[x] = lut[video_med3(p[-2], p[0], p[2]) << 2];
            }
        } else if (median) {
            for (int x = 0; x < VIDEO_W; x++, pos += step) {
                const uint8_t *p = q + (pos >> 8);
                line[x] = (uint8_t)(video_med3(p[-2], p[0], p[2]) << 2);
            }
        } else if (!stats) {
            /* [1 2 1]/4 low-pass with taps 0.1 us apart: -15 dB at the NTSC and
             * -30 dB at the PAL colour subcarrier at 10 and 20 MS/s alike
             * (neighbouring samples at 20 MS/s gave only -3 dB). */
            for (int x = 0; x < VIDEO_W; x++, pos += step) {
                const uint8_t *p = q + (pos >> 8);
                line[x] = lut[p[-d] + 2 * p[0] + p[d]];
            }
        } else {
            for (int x = 0; x < VIDEO_W; x++, pos += step) {
                const uint8_t *p = q + (pos >> 8);
                line[x] = (uint8_t)(p[-d] + 2 * p[0] + p[d]);
            }
        }
    } else {
        for (int x = 0; x < VIDEO_W; x++, pos += step) {
            const uint32_t i = base + (pos >> 8);
#define S(k) (r[(i + (uint32_t)(k)) & m] & 63)
            const int level = median ? video_med3(S(-2), S(0), S(2)) << 2 : S(-d) + 2 * S(0) + S(d);
            line[x] = stats ? (uint8_t)level : lut[level];
#undef S
        }
    }
    if (stats) {
        /* Auto-contrast statistics from every fourth line, on the same
         * filtered values the pixels show (raw samples would widen the
         * percentiles by the noise and wash the picture out). */
        uint32_t *hist = v->hist;
        for (int x = 0; x < VIDEO_W; x += 2) hist[line[x] >> 2]++;
        for (int x = 0; x < VIDEO_W; x++) line[x] = lut[line[x]];
    }
    memcpy(v->fb + row * VIDEO_W, line, VIDEO_W);
    if (v->colour_field && !(row & 1)) render_chroma(v, base + (uint32_t)v->c_coff, row / 2);
}

static void restart_tracking(video_t *v, int64_t pos)
{
    /* The vertical lock stays: the flywheel counted the missed lines, and
     * the C5's weak sync loses the line lock every few seconds on the board
     * (2026-10-19), where dropping it let a false vertical sync split the
     * frame. A field forced after a real loss takes the next sync (v_miss). */
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
    /* Broad (vertical sync) pulse: low through most of the first half line,
     * at least 5 of 7 probes from 6 to 24 us. Two probes mistook dark picture
     * for vsync when sync and blanking are only ~5 codes apart (board); a
     * measured low run would be cut short by a single FM click. */
    int probes = 0;
    for (int k = 0; k < 7; k++) probes += box_below(v, base + v->c_broad[k]);
    const bool broad = probes >= 5;

    /* The first regular line after a run of broad pulses starts a field.
     * Once locked, a vertical sync far too early is noise (vertical flywheel),
     * and a missing one is replaced after the expected field length. */
    const int expect = v->std == VIDEO_STD_PAL ? 312 : 262;
    if (broad) {
        v->broad_run++;
    } else {
        /* After a forced field (v_miss) any vertical sync is taken: the
         * flywheel only guesses the field start. */
        if (v->broad_run >= 2 && (!v->v_locked || v->v_miss || v->line_in_field >= expect - 8)) field_start(v, true);
        v->broad_run = 0;
    }
    if (v->line_in_field >= (v->v_locked ? expect + 8 : FREE_RUN_LINES)) field_start(v, false);

    const bool pal = v->std == VIDEO_STD_PAL;
    const int rows = pal ? 288 : 240;
    const int row = v->line_in_field - (pal ? v->active_first_pal : v->active_first_ntsc) - v->v_offset;
    /* The burst of every other line (the chroma rows'): the reference is an
     * average anyway, and every line cost 4 us of the decoder's 64 (board). */
    if (!broad && v->colour && !pal && v->h_locked && !(row & 1)) colour_burst(v, t0);
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
    v->c_box = us_n(fs_hz, 0.4) > 2 ? us_n(fs_hz, 0.4) : 2;
    v->c_tap = us_n(fs_hz, 0.1) > 1 ? us_n(fs_hz, 0.1) : 1;
    for (int k = 0; k < 7; k++) v->c_broad[k] = us_n(fs_hz, 6.0 + 3.0 * k);   /* 6..24 us */
    v->c_sync_a = us_n(fs_hz, 1.0);
    v->c_sync_b = us_n(fs_hz, 3.5);
    /* Blanking level after the colour burst (NTSC 5.3..7.8 us, PAL
     * 5.6..7.85 us) and before the picture (NTSC 9.4 us, PAL 10.5 us). */
    v->c_porch_a = us_n(fs_hz, 8.0);
    v->c_porch_b = us_n(fs_hz, 9.2);
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
    v->filter = VIDEO_FILTER_MEDIAN;
    v->render = true;
    colour_init(v);
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
    v->v_miss = 0;
    /* Sample positions start anew: the colour reference with them. */
    v->have_ref = v->have_pend = v->line_ok = v->burst_ok = v->colour_field = false;
    v->weak_bursts = 0;
    v->burst_clip = 12;
}

VIDEO_HOT void video_process(video_t *v, uint32_t write_pos)
{
    v->wr += (int32_t)(write_pos - v->wr32);
    v->wr32 = write_pos;
    for (;;) {
        const int64_t start = v->t_edge >> 8;
        if (start + ((v->period * 3 / 2) >> 8) + v->c_tail > v->wr) break;
        if (start - 4 < v->wr - (int64_t)v->ring_size + v->c_margin) {
            /* A ring behind the writer: those samples are being overwritten.
             * Locked, skip whole lines and keep the lock and the line count
             * (the picture misses those rows once); unlocked, search anew. */
            v->overruns++;
            const int64_t target = (v->wr - (int64_t)v->ring_size / 4) << 8;
            if (v->h_locked && v->period > 0) {
                const int64_t k = (target - v->t_edge + v->period - 1) / v->period;
                v->t_edge += k * v->period;
                v->line_in_field += (int)k;
                v->lines += (uint32_t)k;
            } else {
                restart_tracking(v, target >> 8);
            }
            continue;
        }
        const uint32_t t0 = NOW_US();
        const uint32_t fields = v->fields;
        const bool unlocked = !v->h_locked, rendering = v->render;
        process_line(v);
        const uint32_t dt = NOW_US() - t0;
        if (rendering) {
            v->drawn_us += dt;
            v->drawn_lines++;
        } else {
            v->skipped_us += dt;
            v->skipped_lines++;
        }
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
    o->filter = v->filter;
    o->colour = v->colour;
    o->colour_shown = v->colour_field;
    o->colour_simd = v->pie;
    o->burst_level = v->have_ref ? sqrtf(v->ref_re * v->ref_re + v->ref_im * v->ref_im) / burst_unit(v) : 0.0f;
    o->slips = v->slips;
    o->slip_sum = v->slip_sum;
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

void video_set_filter(video_t *v, video_filter_t filter)
{
    v->filter = filter;
}

void video_set_colour(video_t *v, bool on)
{
    v->colour = on;
}

