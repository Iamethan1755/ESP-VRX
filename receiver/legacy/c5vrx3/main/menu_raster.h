#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Standalone CVBS menu raster at 40 MHz.
 *
 * The modern UI is 384 x 56 logical pixels. Horizontal coordinates use a
 * 208/50 sample scale (40.0 us). Each logical Y row is repeated on four NTSC
 * or five PAL video lines (224 / 280 lines per field, ~93% / ~98% of the
 * active picture height); repetition reuses the same SRAM row, so the taller
 * menu costs no UI memory.
 *
 * The UI is centred in the active picture on both axes. Horizontal centring
 * lengthens the shared sync/burst prefix to the UI start, so a UI scanline is
 * still exactly prefix + UI row + blank tail (no extra DMA node per line).
 * NTSC active video is 9.4..62.06 us after 0H, PAL 10.35..62.35 us.
 */
#define MENU_FONT_HEIGHT 8u
#define MENU_UI_WIDTH 384u
#define MENU_UI_LINES 56u
#define MENU_UI_X_SCALE_NUM 208u
#define MENU_UI_X_SCALE_DEN 50u
#define MENU_UI_BYTES 1600u
#define MENU_NTSC_UI_Y_REPEAT 4u
#define MENU_PAL_UI_Y_REPEAT 5u
/* First UI line as a field-line index of menu_raster_emit (NTSC index k is
 * field line k+1, PAL index k is field line k-1). Active picture: NTSC
 * k=21..261, PAL k=25..311 in both fields. */
#define MENU_NTSC_UI_FIRST_LINE 29u
#define MENU_PAL_UI_FIRST_LINE 28u

/* Line start (sync leading edge) to UI start; DMA word aligned. */
#define MENU_NTSC_PREFIX_BYTES 628u
#define MENU_PAL_PREFIX_BYTES 652u
#define MENU_PREFIX_BYTES MENU_PAL_PREFIX_BYTES
/* Longest line remainder after the shortest prefix (NTSC lines are <= 2544). */
#define MENU_TAIL_BYTES (2560u - MENU_NTSC_PREFIX_BYTES)
#define MENU_PHASES 8u
/* One complete two-field frame is enough for the monochrome menu.  Closing
 * the burst phase over that frame keeps the waveform cyclic without the old
 * eight-field descriptor chain (which needed ~76 KiB and could not be
 * allocated after Wi-Fi/PHY startup on the C5). */
#define MENU_FIELDS 2u
#define MENU_MAX_NODES 1840u

typedef enum { VIDEO_STD_NTSC, VIDEO_STD_PAL } video_standard_t;

typedef struct {
    uint8_t prefix[MENU_PHASES][MENU_PREFIX_BYTES];
    uint8_t no_burst[MENU_PREFIX_BYTES];
    uint8_t equalizing[1280];
    uint8_t broad[1280];
    uint8_t blank[MENU_TAIL_BYTES];
    uint8_t ui[MENU_UI_LINES][MENU_UI_BYTES];
} menu_raster_t;

typedef bool (*menu_segment_fn)(void *ctx, const uint8_t *data, unsigned length);
void menu_raster_init(menu_raster_t *raster, video_standard_t standard);
bool menu_raster_emit(const menu_raster_t *raster, video_standard_t standard,
                      menu_segment_fn emit, void *ctx);
uint32_t menu_half_sample(video_standard_t standard, unsigned half);

static inline unsigned menu_prefix_bytes(video_standard_t standard)
{
    return standard == VIDEO_STD_PAL ? MENU_PAL_PREFIX_BYTES : MENU_NTSC_PREFIX_BYTES;
}

static inline unsigned menu_ui_y_repeat(video_standard_t standard)
{
    return standard == VIDEO_STD_PAL ? MENU_PAL_UI_Y_REPEAT : MENU_NTSC_UI_Y_REPEAT;
}

static inline unsigned menu_ui_first_line(video_standard_t standard)
{
    return standard == VIDEO_STD_PAL ? MENU_PAL_UI_FIRST_LINE : MENU_NTSC_UI_FIRST_LINE;
}
