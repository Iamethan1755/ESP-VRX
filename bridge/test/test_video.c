/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test: decode the synthetic NTSC/PAL source with the bridge's video
 * decoder and check lock, standard, field rate, alignment and levels;
 * NTSC colour bars (hue, saturation) also through noise, 5 bits and clock
 * slips; also the frame mailbox between the decoder and its readers.
 * Build and run: python bridge/test/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "denoise.h"
#include "mbox.h"
#include "testpattern.h"
#include "video.h"

#define RING (1u << 16)
#define CHUNK 2048u

static uint8_t s_ring[RING], s_src[RING];
static uint8_t s_fb[2][VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX];
static uint8_t s_last[VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX];
static uint8_t s_acc[VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX];   /* denoise running picture, as in the encoder */
static video_fmt_t s_last_fmt = VIDEO_FMT_GRAY;
static bool s_denoise, s_acc_first;
static bool s_weak;   /* C5-like levels: sync 15, blanking 21, white 40, dark on the left */
static bool s_five_bit;   /* bit 0 always 0 (keys on the bit 0 wire) */
static int s_last_h, s_failures;
static int s_rolled = -1;   /* >= 0: count drawn fields whose first row is not picture */

/* Like the firmware: draw every other field only, keep the drawn ones. */
static uint8_t *on_field(uint8_t *done, int w, int h, video_fmt_t fmt, bool rendered, bool *render_next, void *ctx)
{
    (void)ctx;
    if (rendered) {
        /* Bars on row 0, the ramp on the second-last row: a field that
         * starts a few lines early or late (vertical roll) misses one.
         * Colour frames start with the same luma rows (video.h). */
        if (s_rolled >= 0)
            s_rolled += !(done[20] > 200 && done[(h - 2) * w + w - 20] > done[(h - 2) * w + 20] + 150);
        const size_t n = fmt == VIDEO_FMT_COLOUR ? VIDEO_CHROMA_OFF + (size_t)(h / 2) * VIDEO_CHROMA_ROW : (size_t)(w * h);
        if (s_denoise) {
            const bool first = s_acc_first || fmt != s_last_fmt;
            denoise_frame(s_acc, done, w, h, first);
            if (fmt == VIDEO_FMT_COLOUR)
                denoise_frame(s_acc + VIDEO_CHROMA_OFF, done + VIDEO_CHROMA_OFF, VIDEO_CHROMA_ROW, h / 2, first);
            s_acc_first = false;
            memcpy(s_last, s_acc, n);
        } else {
            memcpy(s_last, done, n);
        }
        s_last_fmt = fmt;
        s_last_h = h;
    }
    *render_next = !rendered;
    return rendered ? (done == s_fb[0] ? s_fb[1] : s_fb[0]) : done;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static int px(int x, int y) { return s_last[y * VIDEO_W + x]; }
/* Cb (k = 1) or Cr (k = 3) of pixel x in a colour frame (video.h). */
static int pxc(int x, int y, int k)
{
    return s_last[VIDEO_CHROMA_OFF + (y / 2) * VIDEO_CHROMA_ROW + (k == 3 ? VIDEO_CW : 0) + x * VIDEO_CW / VIDEO_W];
}
/* Mean of three neighbours on a row: the geometry checks below must not
 * trip over single noisy pixels (noise is measured by quality()). */
static int pxa(int x, int y)
{
    const int l = x > 0 ? x - 1 : x, r = x < VIDEO_W - 1 ? x + 1 : x;
    return (px(l, y) + px(x, y) + px(r, y) + 1) / 3;
}

static uint8_t clamp255(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5)); }

/* The last frame as PGM, or PPM (JFIF YCbCr to RGB) for colour. */
static void save_pgm(const char *name)
{
    char path[256];
    const bool colour = s_last_fmt == VIDEO_FMT_COLOUR;
    snprintf(path, sizeof path, "test_out_%s.%s", name, colour ? "ppm" : "pgm");
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s\n%d %d\n255\n", colour ? "P6" : "P5", VIDEO_W, s_last_h);
    if (!colour) {
        fwrite(s_last, 1, (size_t)(VIDEO_W * s_last_h), f);
    } else {
        for (int y = 0; y < s_last_h; y++)
            for (int x = 0; x < VIDEO_W; x++) {
                const double l = px(x, y), cb = pxc(x, y, 1) - 128, cr = pxc(x, y, 3) - 128;
                const uint8_t rgb[3] = {clamp255(l + 1.402 * cr), clamp255(l - 0.344136 * cb - 0.714136 * cr),
                                        clamp255(l + 1.772 * cb)};
                fwrite(rgb, 1, 3, f);
            }
    }
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

/* ---- Colour --------------------------------------------------------------- */

/* Like run(), but the decoder's ring gets the source with one sample taken
 * twice every `every` samples (the S3 clock faster than the C5's): s_src is
 * the test pattern's ring, s_wr the decoder's write position. */
static uint32_t s_wr, s_rd;
static void run_slips(testpattern_t *tp, video_t *v, double seconds, int every)
{
    const uint32_t end = s_wr + (uint32_t)(seconds * tp->fs_hz);
    while ((int32_t)(end - s_wr) > 0) {
        testpattern_run_until(tp, s_rd + CHUNK + 1);
        for (uint32_t k = 0; k < CHUNK; k++) {
            s_ring[s_wr & (RING - 1)] = s_src[s_rd & (RING - 1)];
            s_wr++;
            if (s_wr % (uint32_t)every) s_rd++;
        }
        video_process(v, s_wr);
    }
}

/* Colour bars (testpattern.c: white, yellow, cyan, green, magenta, red,
 * blue, black at 75 %) against JFIF Cb/Cr of the same RGB, averaged over
 * rows 5..35 and the middle half of each bar: hue within max_deg, the
 * chroma amplitude within sat_tol of the expected one, gray bars gray. */
static void check_colour(const char *name, double max_deg, double sat_tol)
{
    static const int rgb[8] = {7, 6, 3, 2, 5, 4, 1, 0};
    CHECK(s_last_fmt == VIDEO_FMT_COLOUR, "%s: frame is not colour", name);
    if (s_last_fmt != VIDEO_FMT_COLOUR) return;
    const double us0 = 9.6, width = 52.2, x0 = 9.4, xl = 52.6;
    for (int bar = 0; bar < 8; bar++) {
        const double c = (x0 + xl * (bar + 0.5) / 8 - us0) / width * VIDEO_W, hw = VIDEO_W / 8.0 / 4;
        double cb = 0, cr = 0;
        int n = 0;
        for (int y = 5; y < 36; y++)
            for (int x = (int)(c - hw); x <= (int)(c + hw); x++, n++) {
                cb += pxc(x, y, 1) - 128;
                cr += pxc(x, y, 3) - 128;
            }
        cb /= n;
        cr /= n;
        const double r = rgb[bar] & 4 ? 0.75 : 0, g = rgb[bar] & 2 ? 0.75 : 0, b = rgb[bar] & 1 ? 0.75 : 0;
        const double wb = 255 * (-0.168736 * r - 0.331264 * g + 0.5 * b), wr = 255 * (0.5 * r - 0.418688 * g - 0.081312 * b);
        const double got_amp = hypot(cb, cr), want_amp = hypot(wb, wr);
        if (want_amp < 1) {
            printf("  bar %d: Cb %+.1f Cr %+.1f (gray)\n", bar, cb, cr);
            CHECK(got_amp < 12, "%s: gray bar %d has colour (Cb %+.1f, Cr %+.1f)", name, bar, cb, cr);
            continue;
        }
        double dh = (atan2(cr, cb) - atan2(wr, wb)) * 180 / 3.141592653589793;
        while (dh > 180) dh -= 360;
        while (dh < -180) dh += 360;
        printf("  bar %d: Cb %+.1f Cr %+.1f, want %+.1f %+.1f: hue %+.1f deg, saturation x%.2f\n", bar, cb, cr, wb, wr,
               dh, got_amp / want_amp);
        CHECK(fabs(dh) <= max_deg, "%s: bar %d hue off by %.1f deg", name, bar, dh);
        CHECK(fabs(got_amp / want_amp - 1) <= sat_tol, "%s: bar %d saturation x%.2f", name, bar, got_amp / want_amp);
    }
}

/* NTSC colour bars through the decoder with colour on, standard levels
 * (black at blanking, so the expected Cb/Cr are exact). slip_every > 0
 * duplicates a sample that often (the board: every ~530000 at 20 MS/s). */
static void scenario_colour(const char *name, uint32_t fs, int noise, int clicks, int slip_every, double max_deg,
                            double sat_tol)
{
    printf("%s\n", name);
    testpattern_t tp;
    video_t v;
    uint8_t *ring = slip_every ? s_src : s_ring;
    const bool ok = (s_weak ? testpattern_init_levels(&tp, ring, RING, fs, false, 15, 21, 40, false)
                            : testpattern_init(&tp, ring, RING, fs, false)) && testpattern_set_colour(&tp, true);
    if (!ok) {
        printf("  FAIL: no memory\n");
        s_failures++;
        return;
    }
    tp.noise = noise;
    tp.clicks = clicks;
    tp.five_bit = s_five_bit;
    testpattern_restart(&tp, 123456u);
    tp.line = 100;
    s_wr = s_rd = tp.pos;
    video_init(&v, s_ring, RING, fs, s_fb[0], on_field, NULL);
    video_reset(&v, tp.pos);
    video_set_levels(&v, VIDEO_LEVELS_SYNC);
    video_set_colour(&v, true);
    s_denoise = false;
    if (slip_every) run_slips(&tp, &v, 2.0, slip_every);
    else run(&tp, &v, 2.0, true);
    video_stats_t st;
    video_get_stats(&v, &st);
    const double want_burst = 0.2 * (s_weak ? 40 - 21 : TP_WHITE - TP_BLANK);
    const uint32_t want_slips = slip_every ? (uint32_t)(2.0 * fs / slip_every) : 0;
    printf("  locked h=%d v=%d colour=%d shown=%d burst=%.2f (want %.2f) slips=%u sum=%d (want ~%u) overruns=%u\n",
           st.h_locked, st.v_locked, st.colour, st.colour_shown, st.burst_level, want_burst, st.slips, (int)st.slip_sum,
           want_slips, st.overruns);
    CHECK(st.h_locked && st.v_locked, "%s: not locked", name);
    CHECK(st.colour_shown, "%s: no colour", name);
    CHECK(fabs(st.burst_level - want_burst) < 0.15 * want_burst, "%s: burst %.2f, want %.2f", name, st.burst_level, want_burst);
    CHECK(st.overruns == 0 && st.lock_losses == 0, "%s: %u overruns, %u lock losses", name, st.overruns, st.lock_losses);
    if (slip_every) {
        /* Each slip counted once (a few may hide in the vertical interval). */
        CHECK(st.slips * 100 >= want_slips * 97 && st.slips * 100 <= want_slips * 101 + 200, "%s: %u slips counted, %u made",
              name, st.slips, want_slips);
        CHECK(abs((int)st.slip_sum - (int)want_slips) * 100 <= (int)want_slips * 3 + 200,
              "%s: %d duplicated minus dropped samples counted, %u made", name, (int)st.slip_sum, want_slips);
    } else {
        CHECK(st.slips <= 1, "%s: %u slips without any", name, st.slips);
    }
    check_colour(name, max_deg, sat_tol);
    save_pgm(name);
    testpattern_free(&tp);
}

/* Colour on but nothing to show in colour: PAL, or NTSC without a burst. */
static void scenario_no_colour(const char *name, bool pal)
{
    printf("%s\n", name);
    testpattern_t tp;
    video_t v;
    if (!testpattern_init(&tp, s_ring, RING, 20000000, pal)) {
        printf("  FAIL: no memory\n");
        s_failures++;
        return;
    }
    testpattern_restart(&tp, 123456u);
    video_init(&v, s_ring, RING, 20000000, s_fb[0], on_field, NULL);
    video_reset(&v, tp.pos);
    video_set_colour(&v, true);
    run(&tp, &v, 1.0, true);
    video_stats_t st;
    video_get_stats(&v, &st);
    printf("  locked h=%d v=%d shown=%d burst=%.2f frame %s\n", st.h_locked, st.v_locked, st.colour_shown, st.burst_level,
           s_last_fmt == VIDEO_FMT_GRAY ? "gray" : "colour");
    CHECK(st.h_locked && st.v_locked && !st.colour_shown && s_last_fmt == VIDEO_FMT_GRAY, "%s: colour without a burst", name);
    if (s_last_fmt == VIDEO_FMT_GRAY) check_picture(&tp, name);
    testpattern_free(&tp);
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

/* The original encoder-only mailbox (stream.c before the SPI screen), for
 * comparison: a frame no one took comes back, else a free buffer. */
typedef struct { uint8_t *ready, *free_[2]; int nfree; } old_mbox_t;

static uint8_t *old_submit(old_mbox_t *o, uint8_t *fb)
{
    uint8_t *next = fb;
    if (o->ready) next = o->ready;
    else if (o->nfree > 0) next = o->free_[--o->nfree];
    if (next != fb) o->ready = fb;
    return next;
}

static uint8_t *old_take(old_mbox_t *o)
{
    uint8_t *fb = o->ready;
    o->ready = NULL;
    return fb;
}

static void old_release(old_mbox_t *o, uint8_t *fb)
{
    if (o->nfree < 2) o->free_[o->nfree++] = fb;
}

static uint32_t stamp(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }
static void set_stamp(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }

/* Frame mailbox (mbox.c) in random orders of submits, takes and releases.
 * Each buffer carries the number of the frame drawn into it; the decoder
 * scribbles over its next buffer at once. The decoder must always get a
 * buffer back, never the published one or one a reader holds (a reader's
 * frame must be unchanged when handed back); every take must return the
 * newest frame, each at most once per reader. With one reader the frames
 * must reach the encoder exactly as through the original mailbox. */
static void check_mailbox(void)
{
    static uint8_t bufs[MBOX_FRAMES_MAX][4], old_bufs[3][4];
    const uint32_t scribble = 0xdeadbeefu;
    uint32_t rng = 1;
    for (int readers = 1; readers <= MBOX_READERS_MAX; readers++) {
        uint8_t *frames[MBOX_FRAMES_MAX];
        for (int i = 0; i < MBOX_FRAMES_MAX; i++) frames[i] = bufs[i];
        mbox_t m;
        mbox_init(&m, frames, readers);
        old_mbox_t o = {NULL, {old_bufs[1], old_bufs[2]}, 2};
        uint8_t *dec = frames[0], *old_dec = old_bufs[0], *held[MBOX_READERS_MAX] = {0}, *old_held = NULL;
        uint32_t held_stamp[MBOX_READERS_MAX] = {0}, had[MBOX_READERS_MAX] = {0}, seq = 0;
        int bad = 0, reads = 0, differ = 0;
        for (int step = 0; step < 200000; step++) {
            rng = rng * 1103515245u + 12345u;
            const unsigned op = rng >> 16 & 7;
            if (op < 3) {
                set_stamp(dec, ++seq);
                uint8_t *next = mbox_submit(&m, dec, VIDEO_W, 240, VIDEO_FMT_GRAY);
                bad += next == dec || next == m.ready;
                for (int r = 0; r < readers; r++) bad += next == held[r];
                dec = next;
                set_stamp(dec, scribble);
                if (readers == 1) {
                    set_stamp(old_dec, seq);
                    uint8_t *old_next = old_submit(&o, old_dec);
                    differ += old_next == old_dec;
                    old_dec = old_next;
                    set_stamp(old_dec, scribble);
                }
                continue;
            }
            const int r = (int)(op - 3) % readers;
            if (held[r]) {
                bad += stamp(held[r]) != held_stamp[r];
                mbox_release(&m, held[r]);
                held[r] = NULL;
                if (readers == 1) {
                    differ += stamp(old_held) != held_stamp[r];
                    old_release(&o, old_held);
                    old_held = NULL;
                }
                continue;
            }
            int w = 0, h = 0, fmt = 0;
            held[r] = mbox_take(&m, r, &w, &h, &fmt);
            if (held[r]) {
                held_stamp[r] = stamp(held[r]);
                bad += held_stamp[r] != seq || seq == had[r] || w != VIDEO_W || h != 240 || fmt != VIDEO_FMT_GRAY;
                had[r] = seq;
                reads++;
            } else {
                bad += seq != had[r];   /* NULL only when this reader had the newest */
            }
            if (readers == 1) {
                old_held = old_take(&o);
                differ += !old_held != !held[r] || (old_held && stamp(old_held) != held_stamp[r]);
            }
        }
        printf("mailbox, %d reader(s): %u frames, %d read, %d wrong%s\n", readers, (unsigned)seq, reads, bad,
               readers == 1 ? (differ ? ", DIFFERS from the original" : ", same as the original") : "");
        CHECK(bad == 0, "mailbox with %d reader(s): %d wrong hand-offs", readers, bad);
        CHECK(differ == 0, "mailbox with one reader differs from the original in %d steps", differ);
        CHECK(reads > 10000, "mailbox with %d reader(s): only %d frames read", readers, reads);
    }
}

int main(void)
{
    check_med3();
    check_mailbox();
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

    /* Colour: clean, noisy, at 10 MS/s, with the board's weak levels, 5
     * bits and FM clicks, and with the board's clock slips (one sample
     * twice every 26.6 ms) and ten times more of them. */
    scenario_colour("ntsc_20mhz_colour", 20000000, 0, 0, 0, 5, 0.12);
    scenario_colour("ntsc_20mhz_colour_noise3", 20000000, 3, 0, 0, 6, 0.15);
    scenario_colour("ntsc_10mhz_colour", 10000000, 0, 0, 0, 6, 0.15);
    s_weak = true;
    s_five_bit = true;
    scenario_colour("ntsc_20mhz_colour_weak_5bit", 20000000, 3, 50, 0, 10, 0.25);
    scenario_colour("ntsc_20mhz_colour_weak_5bit_slips", 20000000, 3, 50, 532000, 10, 0.25);
    scenario_colour("ntsc_20mhz_colour_weak_5bit_slips10x", 20000000, 3, 50, 53200, 12, 0.25);
    s_weak = false;
    s_five_bit = false;
    scenario_no_colour("pal_20mhz_colour_on", true);
    scenario_no_colour("ntsc_20mhz_colour_on_no_burst", false);
    printf(s_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", s_failures);
    return s_failures ? 1 : 0;
}
