#include "menu_raster.h"
#include <math.h>
#include <string.h>

/* ITU-R BT.470: PAL 625/50, NTSC 525/59.94. Round cumulative NTSC
 * half-line time (11440/9 samples) to a DMA word, never each line independently.
 * Within a cycle the longest edge error is 44.45 ns. The four-frame NTSC
 * loop adds only 0.25 ppm period error (see the carrier adjustment below). */
uint32_t menu_half_sample(video_standard_t standard, unsigned half)
{
    return standard == VIDEO_STD_PAL ? half * 1280u :
           ((half * 2860u + 4u) / 9u) * 4u;
}

void menu_raster_init(menu_raster_t *r, video_standard_t standard)
{
    const double tau = 6.2831853071795864769;
    /* Close the colour-reference carrier over one complete two-field frame.
     * The menu itself is monochrome.  The resulting offsets are only
     * +6.25 Hz for PAL and about +10.7 Hz for NTSC, while reducing the cyclic
     * descriptor allocation from ~76 KiB to less than 20 KiB. */
    const double frequency = standard == VIDEO_STD_PAL ?
                             177345.0 * 40000000.0 / 1600000.0 :
                             119438.0 * 40000000.0 / 1334668.0;
    const unsigned burst_start = standard == VIDEO_STD_PAL ? 224u : 212u;
    const unsigned burst_samples = standard == VIDEO_STD_PAL ? 90u : 100u;
    memset(r, 20, sizeof(*r));
    memset(r->no_burst, 0, 188); /* 4.7 us H sync */
    memset(r->equalizing, 0, standard == VIDEO_STD_PAL ? 94 : 92);
    /* Broad pulse ends 4.7 us before the following half-line edge.
     * NTSC short/long half-lines select a shifted start below. */
    memset(r->broad, 0, 1280 - 188);
    for (unsigned phase = 0; phase < MENU_PHASES; ++phase) {
        memcpy(r->prefix[phase], r->no_burst, MENU_PREFIX_BYTES);
        for (unsigned x = burst_start; x < burst_start + burst_samples; ++x) {
            double angle = tau * ((double)phase / MENU_PHASES + x * frequency / 40000000.0);
            r->prefix[phase][x] = (uint8_t)lround(20.0 + 8.0 * sin(angle));
        }
    }
}

bool menu_raster_emit(const menu_raster_t *r, video_standard_t standard,
                      menu_segment_fn emit, void *ctx)
{
    const bool pal = standard == VIDEO_STD_PAL;
    const unsigned field_halves = pal ? 625 : 525;
    const unsigned eq = pal ? 5 : 6;
    /* A complete interlaced frame.  Burst phase is closed at this loop. */
    const unsigned total_halves = field_halves * MENU_FIELDS;
    /* Centre the UI in the active picture, clear of vertical blanking. */
    const unsigned text_start = menu_ui_first_line(standard);
    const unsigned y_repeat = menu_ui_y_repeat(standard);
    const unsigned prefix_bytes = menu_prefix_bytes(standard);
    for (unsigned h = 0; h < total_halves;) {
        /* PAL line 1 starts with broad sync; its five pre-equalizing
         * half-lines belong to the end of the preceding field. NTSC line 1
         * starts with pre-equalization (BT.470 figures 2-1 and 2-2). */
        unsigned pos = (h + (pal ? eq : 0)) % field_halves;
        unsigned start = menu_half_sample(standard, h);
        unsigned length = menu_half_sample(standard, h + 1) - start;
        if (pos < 3 * eq) {
            const uint8_t *data = (pos >= eq && pos < 2 * eq) ?
                r->broad + 1280 - length : r->equalizing;
            if (!emit(ctx, data, length)) return false;
            ++h;
            continue;
        }
        /* Horizontal sync stays on the global full-line grid. The second
         * field's vertical train starts on the intervening half-line. */
        if (h & 1u) {
            if (!emit(ctx, r->blank, length)) return false;
            ++h;
            continue;
        }
        unsigned halves = pos + 1 < field_halves ? 2 : 1;
        length = menu_half_sample(standard, h + halves) - start;
        /* Carrier phase from absolute sample time, including at loop wrap.
         * Phase bins quantize only the burst start phase; samples within each
         * burst retain the exact PAL/NTSC carrier frequency. */
        double cycles = pal ? (double)start * 177345.0 / 1600000.0 :
                              (double)start * 119438.0 / 1334668.0;
        cycles += pal ? ((h / 2) & 1u ? -0.375 : 0.375) : 0.5;
        int phase = (int)floor(cycles * MENU_PHASES + 0.5);
        unsigned frame = h / 1250;
        unsigned pal_line = (h / 2) % 625 + 1;
        /* BT.470 figure 5a: alternate nine-line PAL burst blanking windows
         * over four fields; NTSC suppresses burst only in its pulse train. */
        bool burst = !pal || ((frame & 1u) ?
            (pal_line > 5 && pal_line < 622 && !(pal_line >= 310 && pal_line <= 318)) :
            (pal_line > 6 && pal_line < 623 && !(pal_line >= 311 && pal_line <= 319)));
        const uint8_t *prefix = burst ? r->prefix[(unsigned)phase % MENU_PHASES] : r->no_burst;
        if (!emit(ctx, prefix, prefix_bytes)) return false;
        unsigned line = pos / 2;
        unsigned remaining = length - prefix_bytes;
        if (line >= text_start && line < text_start + y_repeat * MENU_UI_LINES) {
            if (!emit(ctx, r->ui[(line - text_start) / y_repeat], MENU_UI_BYTES)) return false;
            remaining -= MENU_UI_BYTES;
        }
        if (!emit(ctx, r->blank, remaining)) return false;
        h += halves;
    }
    return true;
}
