/* SPDX-License-Identifier: GPL-3.0-only */
/* Replay a raw recording of the C5 bus (bridge_tool.py rawlong) through the
 * bridge's decoder: prints lock and field events with their time and saves
 * the drawn fields (colour as on the bridge: NTSC with a burst; REPLAY_GRAY=1
 * for gray). Build and run: python bridge/test/replay.py FILE.bin
 * usage: replay FILE.bin FS_HZ [median|smooth] [OUTPREFIX] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "video.h"

#define RING (1u << 16)
#define CHUNK 2048u

static uint8_t s_ring[RING];
static uint8_t s_fb[2][VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX];
static const char *s_prefix = "replay";
static int s_saved;

static uint8_t clamp255(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5)); }

/* A gray frame as PGM, a colour one (video.h) as PPM. */
static void save_pgm(const uint8_t *img, int w, int h, video_fmt_t fmt)
{
    char path[512];
    const bool colour = fmt == VIDEO_FMT_COLOUR;
    snprintf(path, sizeof path, "%s_%02d.%s", s_prefix, s_saved++, colour ? "ppm" : "pgm");
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s\n%d %d\n255\n", colour ? "P6" : "P5", w, h);
    if (!colour) {
        fwrite(img, 1, (size_t)(w * h), f);
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *c = img + VIDEO_CHROMA_OFF + (y / 2) * VIDEO_CHROMA_ROW + x * VIDEO_CW / VIDEO_W;
                const double l = img[y * w + x], cb = c[0] - 128, cr = c[VIDEO_CW] - 128;
                const uint8_t rgb[3] = {clamp255(l + 1.402 * cr), clamp255(l - 0.344136 * cb - 0.714136 * cr),
                                        clamp255(l + 1.772 * cb)};
                fwrite(rgb, 1, 3, f);
            }
    }
    fclose(f);
}

static uint8_t *on_field(uint8_t *done, int w, int h, video_fmt_t fmt, bool rendered, bool *render_next, void *ctx)
{
    (void)ctx;
    if (rendered) save_pgm(done, w, h, fmt);
    *render_next = !rendered;
    return rendered ? (done == s_fb[0] ? s_fb[1] : s_fb[0]) : done;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: replay FILE.bin FS_HZ [median|smooth] [OUTPREFIX]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);
    const uint32_t fs = (uint32_t)strtoul(argv[2], NULL, 10);
    if (argc > 4) s_prefix = argv[4];

    static video_t v;
    video_init(&v, s_ring, RING, fs, s_fb[0], on_field, NULL);
    video_reset(&v, 0);
    if (argc > 3 && strcmp(argv[3], "smooth") == 0) video_set_filter(&v, VIDEO_FILTER_SMOOTH);
    const bool colour = !getenv("REPLAY_GRAY");
    video_set_colour(&v, colour);

    const double us = 1e6 / fs;
    /* REPLAY_TRACE=from_ms,to_ms: one line per decoded line in that window */
    double tr_from = -1, tr_to = -1;
    const char *tr = getenv("REPLAY_TRACE");
    if (tr) sscanf(tr, "%lf,%lf", &tr_from, &tr_to);
    const uint32_t step = tr ? 128u : CHUNK;
    video_stats_t prev, st;
    video_get_stats(&v, &prev);
    bool h_prev = false, v_prev = false;
    uint32_t lines_prev = 0;
    int64_t edge_prev = 0;
    for (uint32_t pos = 0; pos + step <= (uint32_t)size; pos += step) {
        memcpy(s_ring + (pos & (RING - 1)), data + pos, step);
        video_process(&v, pos + step);
        video_get_stats(&v, &st);
        const double ms = pos * us / 1000.0;
        if (ms >= tr_from && ms <= tr_to && v.lines != lines_prev) {
            const double dt = (v.t_edge - edge_prev) / 256.0 * us;
            printf("    %8.3f ms line %3d edge %+8.2f us (period %.3f) run %3d miss %2d h%d broad_run %d slice %.1f\n",
                   (v.t_edge >> 8) * us / 1000.0, v.line_in_field, dt, v.period / 256.0 * us, v.cur_run, v.miss,
                   v.h_locked, v.broad_run, v.slice_q8 / 256.0);
        }
        lines_prev = v.lines;
        edge_prev = v.t_edge;
        if (st.h_locked != h_prev)
            printf("%8.3f ms  h_lock %d (line %d of field, slicer %.1f, sync %.1f, blank %.1f)\n", ms, st.h_locked,
                   v.line_in_field, v.slice_q8 / 256.0, st.sync_level, st.blank_level);
        if (st.fields != prev.fields)
            printf("%8.3f ms  field %u: %d lines, %s, v_lock %d, colour %s burst %.2f noise %.1f deg sat %.2f slips %u\n",
                   ms, st.fields, st.lines_per_field, v.v_miss ? "forced (no vsync)" : "vsync", st.v_locked,
                   st.colour_shown ? "shown" : st.colour_noisy ? "noisy" : "no", st.burst_level, st.burst_noise_deg,
                   st.colour_sat, (unsigned)st.slips);
        else if (st.v_locked != v_prev)
            printf("%8.3f ms  v_lock %d\n", ms, st.v_locked);
        h_prev = st.h_locked;
        v_prev = st.v_locked;
        prev = st;
    }
    printf("total: %u fields, %u lock losses, %u overruns, line %.3f us, sync %.1f blank %.1f, %d frames saved\n",
           st.fields, st.lock_losses, st.overruns, st.line_us, st.sync_level, st.blank_level, s_saved);
    if (!tr) {
        /* Decoder cost on this input: the firmware has ~2x less time than
         * the signal lasts on core 1 at 20 MS/s; compare runs, not absolutes. */
        video_init(&v, s_ring, RING, fs, s_fb[0], on_field, NULL);
        video_reset(&v, 0);
        video_set_colour(&v, colour);
        const clock_t c0 = clock();
        for (int rep = 0; rep < 5; rep++)
            for (uint32_t pos = 0; pos + CHUNK <= (uint32_t)size; pos += CHUNK) {
                const uint32_t p = rep * (uint32_t)size + pos;
                memcpy(s_ring + (p & (RING - 1)), data + pos, CHUNK);
                video_process(&v, p + CHUNK);
            }
        const double cpu_ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
        printf("host decode time: %.1f ms for %.1f ms of signal (%.2f %%)\n", cpu_ms, 5 * size * us / 1000.0,
               100.0 * cpu_ms / (5 * size * us / 1000.0));
    }
    free(data);
    return 0;
}
