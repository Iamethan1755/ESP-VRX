/* Shared state of the C5VRX Wi-Fi bridge: source selection, status and the
 * picture commands used by both the USB console and the web page. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video.h"

typedef enum { SOURCE_LIVE = 0, SOURCE_TEST_NTSC, SOURCE_TEST_PAL, SOURCE_COUNT } bridge_source_t;

typedef struct {
    bridge_source_t source;
    bool capture_ok;          /* camera port records samples */
    video_stats_t video;
    float sample_msps;        /* measured input rate */
    float decode_load;        /* share of core 1 spent decoding */
    uint32_t max_pass_us;     /* longest decode pass in the last 0.5 s */
    uint32_t max_gap_us;      /* longest wait between decode passes in the last 0.5 s */
    uint32_t max_line_us;     /* slowest single line in the last 0.5 s */
    uint32_t max_line_kind;   /* 1 rendered, 2 field start, 4 unlocked search */
    float fps;                /* JPEG frames per second */
    uint32_t jpeg_bytes;
    uint32_t jpeg_frames;
    int clients;              /* open video streams */
    int stations;             /* devices on the bridge's Wi-Fi */
    uint32_t heap_internal;
    uint32_t heap_psram;
    uint32_t uptime_s;
} bridge_status_t;

/* Shared commands, applied by the decoder task:
 *   t  next source (C5 input, NTSC test pattern, PAL test pattern)
 *   c  levels: auto contrast / standard sync-referenced levels
 *   j l  picture left / right (0.2 us)
 *   i k  picture up / down (1 line)
 * Returns false for anything else. */
bool bridge_command(char c);
void bridge_get_status(bridge_status_t *out);
int bridge_status_json(char *buf, size_t len);
/* Copies up to `max` of the most recent bus samples; returns the count. */
uint32_t bridge_snapshot_raw(uint8_t *dst, uint32_t max, uint32_t *fs_hz);
const char *bridge_source_name(bridge_source_t s);
