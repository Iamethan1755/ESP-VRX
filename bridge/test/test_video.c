/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test: decode the synthetic NTSC/PAL source with the bridge's video
 * decoder and check lock, standard, field rate, alignment and levels.
 * Build and run: python bridge/test/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "denoise.h"
#include "testpattern.h"
#include "video.h"

#define RING (1u << 16)
#define CHUNK 2048u

static uint8_t s_ring[RING];
static uint8_t s_fb[2][VIDEO_W * VIDEO_H_MAX];
static uint8_t s_last[VIDEO_W * VIDEO_H_MAX];
static uint8_t s_acc[VIDEO_W * VIDEO_H_MAX];   /* denoise running picture, as in the encoder */
static bool s_denoise, s_acc_first;
static bool s_weak;   /* C5-like levels: sync 15, blanking 21, white 40, dark on the left */
static bool s_five_bit;   /* bit 0 always 0 (keys on the bit 0 wire) */
static int s_last_h, s_failures;
static int s_rolled = -1;   /* >= 0: count drawn fields whose first row is not picture */

/* Like the firmware: draw every other field only, keep the drawn ones. */
static uint8_t *on_field(uint8_t *done, int w, int h, bool rendered, bool *render_next, void *ctx)
{
    (void)ctx;
    if (rendered) {
        /* Bars on row 0, the ramp on the second-last row: a field that
         * starts a few lines early or late (vertical roll) misses one. */
        if (s_rolled >= 0)
            s_rolled += !(done[20] > 200 && done[(h - 2) * w + w - 20] > done[(h - 2) * w + 20] + 150);
        if (s_denoise) {
            denoise_frame(s_acc, done, w, h, s_acc_first);
            s_acc_first = false;
            memcpy(s_last, s_acc, (size_t)(w * h));
        } else {
            memcpy(s_last, done, (size_t)(w * h));
        }
        s_last_h = h;
    }
    *render_next = !rendered;
    return rendered ? (done == s_fb[0] ? s_fb[1] : s_fb[0]) : done;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static int px(int x, int y) { return s_last[y * VIDEO_W + x]; }
/* Mean of three neighbours on a row: the geometry checks below must not
 * trip over single noisy pixels (noise is measured by quality()). */
static int pxa(int x, int y)
{
    const int l = x > 0 ? x - 1 : x, r = x < VIDEO_W - 1 ? x + 1 : x;
    return (px(l, y) + px(x, y) + px(r, y) + 1) / 3;
}

static void save_pgm(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "test_out_%s.pgm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", VIDEO_W, s_last_h);
    fwrite(s_last, 1, (size_t)(VIDEO_W * s_last_h), f);
    fclose(f);
}

/* Feed `seconds` of signal in DMA-sized chunks, decoding after each chunk. */
static void run(testpattern_t *tp, video_t *v, double seconds, bool signal)
{
    uint32_t end = tp->pos + (uint32_t)(seconds * tp->fs_hz);
    while ((int32_t)(end - tp->pos) > 0) {
        if (signal) {
            testpattern_run_until(tp, tp->pos + CHUNK);
        } else {
            for (uint32_t k = 0; k < CHUNK; k++) s_ring[(tp->pos + k) & (RING - 1)] = 0;
            tp->pos += CHUNK;
        }
        video_process(v, tp->pos);
    }
}

static void check_picture(const testpattern_t *tp, const char *name)
{
    const double us0 = tp->pal ? 10.7 : 9.6, width = tp->pal ? 51.6 : 52.2;
    const double x0 = tp->pal ? 10.5 : 9.4, xl = tp->pal ? 52.0 : 52.6;
    const int tol = 24 + 4 * tp->noise;
    int prev = 999;
    for (int bar = 0; bar < 8; bar++) {
        double t = x0 + xl * (bar + 0.5) / 8;
        int x = (int)((t - us0) / width * VIDEO_W);
        int want = 248 - bar * 248 / 7;
        int got = pxa(x, 20);
        CHECK(abs(got - want) <= tol, "%s: bar %d at x=%d is %d, want ~%d", name, bar, x, got, want);
        CHECK(got < prev, "%s: bars not decreasing at bar %d", name, bar);
        prev = got;
    }
    /* first and last picture rows must be picture, not blanking (PAL's last
     * row is a half line in one of the two fields) */
    CHECK(px(20, 0) > 200, "%s: row 0 is not the first picture line (%d)", name, px(20, 0));
    int last = s_last_h - 2;
    CHECK(px(VIDEO_W - 20, last) > px(20, last) + 150,
          "%s: last row %d is not the ramp (%d .. %d)", name, last, px(20, last), px(VIDEO_W - 20, last));
    /* The moving box: white inside (rows 41..78), black top edge on row 40,
     * bars above it. Box columns are white on row 60 but not on row 100. */
    int cols = 0, bad = 0;
    for (int x = 0; x < VIDEO_W; x++) {
        if (pxa(x, 60) < 220 || pxa(x, 100) > 160) continue;
        cols++;
        bad += !(pxa(x, 41) > 200 && pxa(x, 78) > 200 && pxa(x, 40) < 60 + 4 * tp->noise && pxa(x, 39) < 200);
    }
    CHECK(bad <= 2 + 2 * tp->noise, "%s: box misplaced (%d columns, %d wrong)", name, cols, bad);
}

/* Picture quality on the flat bars (rows 5..35, middle half of each bar):
 * speckles = pixels more than 40 off the bar's level, rms = error. */
static void quality(const testpattern_t *tp, int *speckles, double *rms)
{
    const double us0 = tp->pal ? 10.7 : 9.6, width = tp->pal ? 51.6 : 52.2;
    const double x0 = tp->pal ? 10.5 : 9.4, xl = tp->pal ? 52.0 : 52.6;
    double se = 0;
    int n = 0, sp = 0;
    for (int bar = 0; bar < 8; bar++) {
        const double c = (x0 + xl * (bar + 0.5) / 8 - us0) / width * VIDEO_W, hw = VIDEO_W / 8.0 / 4;
        int sum = 0, cnt = 0;   /* the bar's true level: median-free mean of the clean rows is unknown, use row means */
        for (int y = 5; y < 36; y++)
            for (int x = (int)(c - hw); x <= (int)(c + hw); x++) { sum += px(x, y); cnt++; }
        const double want = (double)sum / cnt;
        for (int y = 5; y < 36; y++)
            for (int x = (int)(c - hw); x <= (int)(c + hw); x++) {
                const double e = px(x, y) - want;
                se += e * e;
                n++;
                sp += e > 40 || e < -40;
            }
    }
    *speckles = sp;
    *rms = n ? sqrt(se / n) : 0;
}

static void scenario_full(const char *name, uint32_t fs, bool pal, int noise, int clicks,
                          video_filter_t filter, bool denoise, int *speckles_out, double *rms_out)
{
    printf("%s\n", name);
    testpattern_t tp;
    video_t v;
    const bool ok = s_weak ? testpattern_init_levels(&tp, s_ring, RING, fs, pal, 15, 21, 40, true)
                           : testpattern_init(&tp, s_ring, RING, fs, pal);
    if (!ok) {
        printf("  FAIL: no memory\n");
        s_failures++;
        return;
    }
    tp.noise = noise;
    tp.clicks = clicks;
    tp.five_bit = s_five_bit;
    testpattern_restart(&tp, 123456u);   /* arbitrary start, mid-field for the decoder */
    video_init(&v, s_ring, RING, fs, s_fb[0], on_field, NULL);
    video_reset(&v, tp.pos);
    video_set_filter(&v, filter);
    s_denoise = denoise;
    s_acc_first = true;
    tp.line = 100;

    run(&tp, &v, 2.0, true);
    video_stats_t st;
    video_get_stats(&v, &st);
    double rate = pal ? 50.0 : 59.94;
    printf("  locked h=%d v=%d std=%s lines/field=%d line=%.3f us fields=%u overruns=%u lock_losses=%u "
           "sync=%.1f blank=%.1f map=%.1f..%.1f\n",
           st.h_locked, st.v_locked, st.std == VIDEO_STD_PAL ? "PAL" : st.std == VIDEO_STD_NTSC ? "NTSC" : "?",
           st.lines_per_field, st.line_us, st.fields, st.overruns, st.lock_losses, st.sync_level, st.blank_level,
           st.black_level, st.white_level);
    CHECK(st.h_locked && st.v_locked, "%s: not locked", name);
    CHECK(st.std == (pal ? VIDEO_STD_PAL : VIDEO_STD_NTSC), "%s: wrong standard", name);
    /* interlaced: fields alternate 262/263 (NTSC) and 312/313 (PAL) lines */
    int lpf = pal ? 312 : 262;
    CHECK(st.lines_per_field == lpf || st.lines_per_field == lpf + 1, "%s: %d lines per field", name, st.lines_per_field);
    CHECK(st.fields >= (uint32_t)(2.0 * rate) - 6 && st.fields <= (uint32_t)(2.0 * rate) + 2,
          "%s: %u fields in 2 s", name, st.fields);
    CHECK(st.overruns == 0, "%s: %u overruns", name, st.overruns);
    CHECK(st.lock_losses == 0, "%s: %u line-lock losses", name, st.lock_losses);
    const float want_sync = s_weak ? 15.0f : TP_SYNC, want_blank = s_weak ? 21.0f : TP_BLANK;
    CHECK(st.sync_level > want_sync - 1.0f && st.sync_level < want_sync + 1.0f, "%s: sync level %.2f", name, st.sync_level);
    CHECK(st.blank_level > want_blank - 1.0f && st.blank_level < want_blank + 1.0f, "%s: blank level %.2f", name, st.blank_level);
    CHECK(s_last_h == (pal ? 288 : 240), "%s: frame height %d", name, s_last_h);
    /* The [1 2 1] filter with FM clicks is the reference for the speckle
     * comparison: its picture is expected to fail the exact-pixel checks. */
    const bool strict = !(clicks && filter == VIDEO_FILTER_SMOOTH) && !s_weak;
    if (strict) check_picture(&tp, name);
    save_pgm(name);
    int speckles;
    double rms;
    if (s_weak) speckles = 0, rms = 0;
    else quality(&tp, &speckles, &rms);
    printf("  quality: speckles=%d rms=%.2f (filter=%s denoise=%d)\n", speckles, rms,
           filter == VIDEO_FILTER_MEDIAN ? "median" : "smooth", denoise);
    if (speckles_out) *speckles_out = speckles;
    if (rms_out) *rms_out = rms;

    /* Missed vertical syncs (a fade, a channel change): the flywheel keeps
     * the field phase, no field may come out rolled, also on the ones after.
     * Restarting the line count after a forced field once rolled the picture
     * for 4-5 fields. */
    if (strict) {
        for (int drops = 1; drops <= 3; drops++) {
            tp.drop_vsync = drops;
            s_rolled = 0;
            run(&tp, &v, 0.2, true);
            const int rolled = s_rolled;
            s_rolled = -1;
            video_get_stats(&v, &st);
            printf("  %d vertical sync(s) dropped: rolled fields=%d locked h=%d v=%d\n", drops, rolled, st.h_locked, st.v_locked);
            CHECK(rolled == 0, "%s: %d rolled fields after %d missed vertical syncs", name, rolled, drops);
            CHECK(st.h_locked && st.v_locked, "%s: lock lost after %d missed vertical syncs", name, drops);
        }
    }

    /* Signal loss: fields keep coming (free-run), lock drops. */
    uint32_t f0 = v.fields;
    run(&tp, &v, 0.5, false);
    video_get_stats(&v, &st);
    printf("  no signal: locked h=%d fields +%u\n", st.h_locked, st.fields - f0);
    CHECK(!st.h_locked, "%s: still locked without signal", name);
    CHECK(st.fields - f0 >= 5, "%s: only %u fields without signal", name, st.fields - f0);

    /* Signal back: relock within half a second. */
    run(&tp, &v, 0.5, true);
    video_get_stats(&v, &st);
    printf("  signal back: locked h=%d v=%d\n", st.h_locked, st.v_locked);
    CHECK(st.h_locked && st.v_locked, "%s: no relock", name);
    if (strict) check_picture(&tp, name);

    /* Decoder starved for two rings: overrun detected, then relock. */
    testpattern_run_until(&tp, tp.pos + 2 * RING);
    run(&tp, &v, 0.5, true);
    video_get_stats(&v, &st);
    printf("  after starvation: overruns=%u locked h=%d v=%d\n", st.overruns, st.h_locked, st.v_locked);
    CHECK(st.overruns >= 1, "%s: overrun not detected", name);
    CHECK(st.h_locked && st.v_locked, "%s: no relock after overrun", name);

    /* A short stall (the decoder a bit under one ring behind, as when Wi-Fi
     * holds core 1 off): lines are skipped but the line lock stays. */
    const uint32_t losses = st.lock_losses, over = st.overruns;
    testpattern_run_until(&tp, tp.pos + RING * 7 / 8 + 2048);
    run(&tp, &v, 0.2, true);
    video_get_stats(&v, &st);
    printf("  short stall: overruns +%u lock losses +%u locked h=%d v=%d\n", st.overruns - over,
           st.lock_losses - losses, st.h_locked, st.v_locked);
    CHECK(st.overruns > over, "%s: short stall not detected", name);
    CHECK(st.lock_losses == losses, "%s: short stall dropped the line lock", name);
    CHECK(st.h_locked && st.v_locked, "%s: not locked after a short stall", name);
    testpattern_free(&tp);
}

static void scenario(const char *name, uint32_t fs, bool pal, int noise)
{
    scenario_full(name, fs, pal, noise, 0, VIDEO_FILTER_MEDIAN, false, NULL, NULL);
}

static void check_med3(void)
{
    int bad = 0;
    for (int a = 0; a < 16; a++)
        for (int b = 0; b < 16; b++)
            for (int c = 0; c < 16; c++) {
                const int lo = a < b ? a : b, hi = a < b ? b : a;
                const int want = a + b + c - lo - hi - (c < lo ? c : c > hi ? c : 0) - (c < lo ? 0 : c > hi ? 0 : 0);
                int s[3] = {a, b, c};
                for (int i = 0; i < 3; i++)
                    for (int j = i + 1; j < 3; j++)
                        if (s[j] < s[i]) { int tmp = s[i]; s[i] = s[j]; s[j] = tmp; }
                (void)want;
                bad += video_med3(a, b, c) != s[1];
            }
    printf("med3: %d wrong of 4096\n", bad);
    CHECK(bad == 0, "video_med3 is not a median");
}

int main(void)
{
    check_med3();
    scenario("ntsc_10mhz", 10000000, false, 0);
    scenario("pal_10mhz", 10000000, true, 0);
    scenario("ntsc_20mhz", 20000000, false, 0);
    scenario("pal_20mhz", 20000000, true, 0);
    scenario("ntsc_10mhz_noise3", 10000000, false, 3);
    scenario("pal_10mhz_noise3", 10000000, true, 3);

    /* FM clicks (0.5 % of samples) plus noise: the median must remove most
     * speckles the [1 2 1] filter lets through; denoise must lower the rms. */
    int sp_smooth, sp_median, sp_nr;
    double rms_smooth, rms_median, rms_nr;
    scenario_full("ntsc_20mhz_clicks_smooth", 20000000, false, 2, 50, VIDEO_FILTER_SMOOTH, false, &sp_smooth, &rms_smooth);
    scenario_full("ntsc_20mhz_clicks_median", 20000000, false, 2, 50, VIDEO_FILTER_MEDIAN, false, &sp_median, &rms_median);
    scenario_full("ntsc_20mhz_clicks_median_denoise", 20000000, false, 2, 50, VIDEO_FILTER_MEDIAN, true, &sp_nr, &rms_nr);
    scenario_full("ntsc_10mhz_clicks_median", 10000000, false, 2, 50, VIDEO_FILTER_MEDIAN, false, NULL, NULL);
    scenario_full("pal_20mhz_clicks_median_denoise", 20000000, true, 2, 50, VIDEO_FILTER_MEDIAN, true, NULL, NULL);
    printf("\nspeckles smooth=%d median=%d median+denoise=%d | rms smooth=%.2f median=%.2f median+denoise=%.2f\n",
           sp_smooth, sp_median, sp_nr, rms_smooth, rms_median, rms_nr);
    CHECK(sp_median * 3 <= sp_smooth, "median removed too few speckles (%d vs %d)", sp_median, sp_smooth);
    CHECK(rms_nr < rms_median, "denoise did not lower the noise (%.2f vs %.2f)", rms_nr, rms_median);

    /* The board's real signal (recording 2026-10-18): sync tip and back
     * porch only ~6 codes apart with ~3 codes of noise per sample (here
     * uniform +-4, sd 2.6), a dark left edge (where vertical sync is
     * probed) and FM clicks. Two probes per line took dark picture for
     * broad pulses; single-sample slicing chopped every sync into short
     * runs and the line lock kept dropping. Lock, field count and length
     * must stay right. */
    s_weak = true;
    scenario_full("ntsc_20mhz_weak_dark", 20000000, false, 4, 50, VIDEO_FILTER_MEDIAN, true, NULL, NULL);
    scenario_full("ntsc_10mhz_weak_dark", 10000000, false, 4, 50, VIDEO_FILTER_MEDIAN, false, NULL, NULL);
    scenario_full("pal_20mhz_weak_dark", 20000000, true, 4, 50, VIDEO_FILTER_MEDIAN, true, NULL, NULL);
    /* Keys on the bit 0 wire: the same weak signal with 5 bits. */
    s_five_bit = true;
    scenario_full("ntsc_20mhz_weak_dark_5bit", 20000000, false, 4, 50, VIDEO_FILTER_MEDIAN, true, NULL, NULL);
    scenario_full("pal_20mhz_weak_dark_5bit", 20000000, true, 4, 50, VIDEO_FILTER_MEDIAN, true, NULL, NULL);
    s_weak = false;
    scenario("ntsc_20mhz_5bit", 20000000, false, 0);
    s_five_bit = false;
    s_weak = false;
    printf(s_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", s_failures);
    return s_failures ? 1 : 0;
}
