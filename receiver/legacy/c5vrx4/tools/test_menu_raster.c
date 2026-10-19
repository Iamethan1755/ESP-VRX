/* Host waveform test: compile with main/menu_raster.c and -lm. */
#include "menu_raster.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static menu_raster_t raster;
static uint8_t waveform[2000000];
static unsigned used, nodes;

static bool capture(void *ctx, const uint8_t *data, unsigned length)
{
    (void)ctx;
    assert(((uintptr_t)data & 3u) == 0);
    assert(length && length <= 4092 && !(length & 3u));
    assert(used + length <= sizeof(waveform));
    assert(++nodes <= MENU_MAX_NODES);
    memcpy(waveform + used, data, length);
    used += length;
    return true;
}

static bool reject(void *ctx, const uint8_t *data, unsigned length)
{
    (void)ctx; (void)data; (void)length;
    return false;
}

static void test(video_standard_t standard)
{
    bool pal = standard == VIDEO_STD_PAL;
    unsigned field = pal ? 625 : 525;
    unsigned fields = MENU_FIELDS;
    unsigned eq = pal ? 5 : 6;
    unsigned ui_lines = 0, ui_min = ~0u, ui_max = 0;
    used = nodes = 0;
    menu_raster_init(&raster, standard);
    memset(raster.ui, 60, sizeof(raster.ui));
    assert(menu_raster_emit(&raster, standard, capture, NULL));
    assert(!menu_raster_emit(&raster, standard, reject, NULL));
    assert(used == (pal ? 1600000u : 1334668u));
    for (unsigned h = 0; h < field * fields; ++h) {
        unsigned at = menu_half_sample(standard, h);
        unsigned next = menu_half_sample(standard, h + 1);
        unsigned pos = (h + (pal ? 5 : 0)) % field;
        double ideal = h * (pal ? 1280.0 : 11440.0 / 9.0);
        assert(fabs(at - ideal) <= 1.778);
        /* Both fields must have equalizing/broad/equalizing pulses, with
         * the second train starting halfway between horizontal sync edges. */
        unsigned width = pos < 3 * eq ?
            (pos >= eq && pos < 2 * eq ? next - at - 188 : (pal ? 94 : 92)) :
            (h % 2 == 0 ? 188 : 0);
        for (unsigned x = 0; x < next - at; ++x) {
            assert((waveform[at + x] == 0) == (x < width));
            assert(waveform[at + x] <= 60);
        }
        if (pos >= 3 * eq && !(h & 1)) {
            unsigned burst = pal ? 224 : 212;
            unsigned count = pal ? 90 : 100;
            unsigned line = h / 2 % 625 + 1;
            bool burst_enabled = !pal || ((h / 1250 % 2) ?
                (line > 5 && line < 622 && (line < 310 || line > 318)) :
                (line > 6 && line < 623 && (line < 311 || line > 319)));
            bool varies = false;
            for (unsigned x = 188; x < menu_prefix_bytes(standard); ++x) {
                unsigned value = waveform[at + x];
                if (burst_enabled && x >= burst && x < burst + count) {
                    assert(value >= 12 && value <= 28);
                    varies |= value != 20;
                    double carrier = pal ? 177345.0 / 1600000 : 119438.0 / 1334668;
                    double swing = pal ? (h / 2 % 2 ? -0.375 : 0.375) : 0.5;
                    double ideal = 20 + 8 * sin(6.283185307179586 * ((at + x) * carrier + swing));
                    /* Quantized start phase plus integer DAC amplitude. */
                    assert(fabs(value - ideal) < 3.7);
                } else assert(value == 20);
            }
            assert(varies == burst_enabled);

            /* UI placement: rows occupy exactly [prefix, prefix + row) on
             * the repeated, vertically centred lines and nowhere else. */
            unsigned k = pos / 2;
            unsigned halves = pos + 1 < field ? 2 : 1;
            unsigned line_len = menu_half_sample(standard, h + halves) - at;
            unsigned prefix = menu_prefix_bytes(standard);
            unsigned first = menu_ui_first_line(standard);
            bool ui_line = k >= first &&
                           k < first + menu_ui_y_repeat(standard) * MENU_UI_LINES;
            for (unsigned x = 0; x < line_len; ++x) {
                bool in_ui = ui_line && x >= prefix && x < prefix + MENU_UI_BYTES;
                assert((waveform[at + x] == 60) == in_ui);
            }
            if (ui_line) {
                ++ui_lines;
                assert(line_len - prefix - MENU_UI_BYTES >= 60); /* >= 1.5 us front porch */
                if (k < ui_min) ui_min = k;
                if (k > ui_max) ui_max = k;
            }
        }
    }
    /* Both fields carry the full UI; it stays inside the active picture and
     * is centred vertically (in lines) and horizontally (in samples). */
    unsigned active_first = pal ? 25 : 21, active_last = pal ? 311 : 261;
    assert(ui_lines == fields * menu_ui_y_repeat(standard) * MENU_UI_LINES);
    assert(ui_min >= active_first && ui_max <= active_last);
    assert(abs((int)(ui_min - active_first) - (int)(active_last - ui_max)) <= 1);
    double active_mid = pal ? (10.35 + 62.35) / 2 * 40 : (9.4 + 63.5556 - 1.5) / 2 * 40;
    assert(fabs(menu_prefix_bytes(standard) + MENU_UI_BYTES / 2.0 - active_mid) <= 4.0);
    /* Burst samples must correlate with the selected carrier, not a luma
     * rectangle or an IQ pattern. Check all starting phases. */
    double omega = 6.283185307179586 *
                   (pal ? 177345.0 / 1600000 : 119438.0 / 1334668);
    for (unsigned p = 0; p < MENU_PHASES; ++p) {
        unsigned begin = pal ? 224 : 212;
        unsigned count = pal ? 90 : 100;
        for (unsigned x = begin; x < begin + count; ++x) {
            double ideal = 20 + 8 * sin(6.283185307179586 * p / MENU_PHASES + omega * x);
            assert(fabs(raster.prefix[p][x] - ideal) <= 0.501);
        }
    }
    double cycles = used * (pal ? 177345.0 / 1600000 : 119438.0 / 1334668);
    assert(fabs(cycles - round(cycles)) < 1e-8);
    if (!pal) {
        assert(fabs(119438.0 * 40000000 / used - 315000000.0 / 88) < 12.0);
        assert(fabs(used / (40000000.0 * 1001 / 30000) - 1) < 0.000001);
    }
    printf("%s: %u samples, %u fields, %u DMA nodes; UI lines %u..%u (%u per field) at %.2f..%.2f us; "
           "sync, burst, alignment and colour-loop closure OK\n",
           pal ? "PAL" : "NTSC", used, fields, nodes, ui_min, ui_max, ui_lines / fields,
           menu_prefix_bytes(standard) / 40.0,
           (menu_prefix_bytes(standard) + MENU_UI_BYTES) / 40.0);
}

int main(void)
{
    test(VIDEO_STD_NTSC);
    test(VIDEO_STD_PAL);
    return 0;
}
