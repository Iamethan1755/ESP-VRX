/* C5VRX Wi-Fi bridge for a Seeed XIAO ESP32-S3.
 *
 * Reads the 6-bit digital video bus of a C5VRX receiver (XIAO ESP32-C5,
 * DAC pins D4..D9) with the S3 camera port, locks to the composite sync,
 * builds one 320x240 (NTSC) / 320x288 (PAL) grayscale frame per field and
 * streams it as MJPEG over the S3's own 2.4 GHz Wi-Fi network.
 *
 * Core 1 runs only the decoder; Wi-Fi, HTTP, JPEG and the console run on
 * core 0. C5VRX itself: https://github.com/Twotoz/C5VRX */
#include <stdio.h>
#include <string.h>
#include "bridge.h"
#include "capture.h"
#include "console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "stream.h"
#include "testpattern.h"
#include "video.h"

static const char *TAG = "bridge";
static const uint32_t s_fs_hz = CONFIG_BRIDGE_SAMPLE_MHZ * 1000000u;

static video_t s_video;
static testpattern_t s_tp;
static bool s_tp_ready;
static uint8_t *s_fb[2];
static bridge_source_t s_source;
static QueueHandle_t s_cmds;
static volatile uint32_t s_write_pos;
static uint32_t s_capture_gen;      /* capture restart the decoder is synchronised to */
static int64_t s_last_submit, s_tp_t0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static video_stats_t s_vstats;
static float s_msps, s_load;
static uint32_t s_max_pass, s_max_gap, s_max_line, s_max_line_kind;

const char *bridge_source_name(bridge_source_t s)
{
    return s == SOURCE_LIVE ? "c5" : s == SOURCE_TEST_NTSC ? "test-ntsc" : "test-pal";
}

/* Decoder task: a field is done. Hand it to the JPEG encoder at most
 * CONFIG_BRIDGE_MAX_FPS times a second (always the same field parity at
 * 30 fps, so the picture does not bob) and render into the other buffer.
 * Fields that would be dropped are not drawn at all: at 30 fps that is
 * every other field, half the decoder's work. */
static uint8_t *on_field(uint8_t *done, int w, int h, bool rendered, bool *render_next, void *ctx)
{
    (void)ctx;
    const int64_t interval = 1000000 / CONFIG_BRIDGE_MAX_FPS - 2000;
    const int64_t field_us = h > 240 ? 20000 : 16683;
    const int64_t now = esp_timer_get_time();
    uint8_t *next = done;
    if (rendered && now - s_last_submit >= interval && stream_submit(done, w, h)) {
        s_last_submit = now;
        next = done == s_fb[0] ? s_fb[1] : s_fb[0];
    }
    *render_next = now + field_us - s_last_submit >= interval;
    return next;
}

static void set_source(bridge_source_t src)
{
    if (src == SOURCE_LIVE) {
        capture_start();
        s_capture_gen = capture_generation();
        video_reset(&s_video, capture_write_pos());
    } else {
        capture_stop();
        if (s_tp_ready) testpattern_free(&s_tp);
        s_tp_ready = testpattern_init(&s_tp, capture_ring(), CAPTURE_RING_BYTES, s_fs_hz, src == SOURCE_TEST_PAL);
        testpattern_restart(&s_tp, 0);
        s_tp_t0 = esp_timer_get_time();
        video_reset(&s_video, 0);
    }
    s_source = src;
    ESP_LOGI(TAG, "source: %s", bridge_source_name(src));
}

static void apply_command(char c)
{
    switch (c) {
    case 't': set_source((bridge_source_t)((s_source + 1) % SOURCE_COUNT)); break;
    case 'c':
        video_set_levels(&s_video, s_video.levels == VIDEO_LEVELS_AUTO ? VIDEO_LEVELS_SYNC : VIDEO_LEVELS_AUTO);
        break;
    case 'j': video_shift_h(&s_video, 2); break;    /* start later: picture moves left */
    case 'l': video_shift_h(&s_video, -2); break;
    case 'i': video_shift_v(&s_video, 1); break;
    case 'k': video_shift_v(&s_video, -1); break;
    default: break;
    }
}

bool bridge_command(char c)
{
    if (!c || !strchr("tcjlik", c)) return false;
    return xQueueSend(s_cmds, &c, 0) == pdTRUE;
}

static void decode_task(void *arg)
{
    (void)arg;
    int64_t window = esp_timer_get_time(), busy = 0, last_end = window, max_pass = 0, max_gap = 0;
    uint32_t window_wp = 0;
    for (;;) {
        char c;
        while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) {
            apply_command(c);
            window_wp = s_source == SOURCE_LIVE ? capture_write_pos() : 0;
        }
        uint32_t wp = 0;
        if (s_source == SOURCE_LIVE) {
            /* Generation first: a restart between the two reads then shows
             * up as a changed generation on the next pass, never as a
             * stale position paired with the new run. */
            const uint32_t gen = capture_generation();
            wp = capture_write_pos();
            if (gen != s_capture_gen) {
                /* restarted elsewhere (console self-test): positions begin at 0 again */
                s_capture_gen = gen;
                video_reset(&s_video, wp);
                window_wp = wp;
            }
        } else if (s_tp_ready) {
            testpattern_run_until(&s_tp, (uint32_t)((esp_timer_get_time() - s_tp_t0) * (int64_t)s_fs_hz / 1000000));
            wp = s_tp.pos;
        }
        s_write_pos = wp;

        const int64_t a = esp_timer_get_time();
        video_process(&s_video, wp);
        const int64_t b = esp_timer_get_time();
        busy += b - a;
        if (b - a > max_pass) max_pass = b - a;
        if (a - last_end > max_gap) max_gap = a - last_end;
        last_end = b;
        if (b - window >= 500000) {
            video_stats_t st;
            video_get_stats(&s_video, &st);
            portENTER_CRITICAL(&s_mux);
            s_vstats = st;
            s_msps = (float)(uint32_t)(wp - window_wp) / (float)(b - window);
            s_load = (float)busy / (float)(b - window);
            s_max_pass = (uint32_t)max_pass;
            s_max_gap = (uint32_t)max_gap;
            s_max_line = s_video.max_line_us;
            s_max_line_kind = s_video.max_line_kind;
            portEXIT_CRITICAL(&s_mux);
            s_video.max_line_us = 0;
            max_pass = max_gap = 0;
            window = b;
            window_wp = wp;
            busy = 0;
        }
        /* Woken by every completed DMA node (205 us at 10 MS/s); the 1 ms
         * timeout paces the test pattern, which has no DMA. */
        ulTaskNotifyTake(pdTRUE, 1);
    }
}

void bridge_get_status(bridge_status_t *out)
{
    memset(out, 0, sizeof *out);
    portENTER_CRITICAL(&s_mux);
    out->video = s_vstats;
    out->sample_msps = s_msps;
    out->decode_load = s_load;
    out->max_pass_us = s_max_pass;
    out->max_gap_us = s_max_gap;
    out->max_line_us = s_max_line;
    out->max_line_kind = s_max_line_kind;
    portEXIT_CRITICAL(&s_mux);
    out->source = s_source;
    out->capture_ok = s_source != SOURCE_LIVE || capture_running();
    stream_stats(&out->fps, &out->jpeg_bytes, &out->jpeg_frames, &out->clients);
    out->stations = stream_wifi_stations();
    out->heap_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

int bridge_status_json(char *buf, size_t len)
{
    bridge_status_t s;
    bridge_get_status(&s);
    const video_stats_t *v = &s.video;
    int n = snprintf(buf, len,
        "{\"source\":\"%s\",\"capture_ok\":%s,\"h_locked\":%s,\"v_locked\":%s,\"std\":\"%s\","
        "\"lines_per_field\":%d,\"line_us\":%.3f,\"sync\":%.1f,\"blank\":%.1f,\"black\":%.1f,\"white\":%.1f,"
        "\"levels\":\"%s\",\"fields\":%lu,\"overruns\":%lu,\"lock_losses\":%lu,\"h_offset_us\":%.1f,"
        "\"v_offset\":%d,\"msps\":%.2f,\"load\":%.2f,\"fps\":%.1f,\"jpeg_bytes\":%lu,\"frames\":%lu,"
        "\"clients\":%d,\"stations\":%d,\"heap_int\":%lu,\"heap_psram\":%lu,\"uptime\":%lu}",
        bridge_source_name(s.source), s.capture_ok ? "true" : "false", v->h_locked ? "true" : "false",
        v->v_locked ? "true" : "false", v->std == VIDEO_STD_PAL ? "PAL" : "NTSC", v->lines_per_field, v->line_us,
        v->sync_level, v->blank_level, v->black_level, v->white_level,
        v->levels == VIDEO_LEVELS_AUTO ? "auto" : "std", (unsigned long)v->fields, (unsigned long)v->overruns,
        (unsigned long)v->lock_losses, v->h_offset_tenths_us / 10.0, v->v_offset_lines, s.sample_msps,
        s.decode_load, s.fps, (unsigned long)s.jpeg_bytes, (unsigned long)s.jpeg_frames, s.clients, s.stations,
        (unsigned long)s.heap_internal, (unsigned long)s.heap_psram, (unsigned long)s.uptime_s);
    return n < (int)len ? n : (int)len - 1;
}

uint32_t bridge_snapshot_raw(uint8_t *dst, uint32_t max, uint32_t *fs_hz)
{
    /* Half the ring: far from the DMA (or test pattern) write head. */
    uint32_t n = max < CAPTURE_RING_BYTES / 2 ? max : CAPTURE_RING_BYTES / 2;
    const uint32_t wp = s_write_pos;
    if (wp < n) n = wp;
    const uint8_t *ring = capture_ring();
    const uint32_t start = (wp - n) & (CAPTURE_RING_BYTES - 1);
    const uint32_t first = CAPTURE_RING_BYTES - start < n ? CAPTURE_RING_BYTES - start : n;
    memcpy(dst, ring + start, first);
    memcpy(dst + first, ring, n - first);
    *fs_hz = s_fs_hz;
    return n;
}

static uint8_t *alloc_frame(void)
{
    void *p = heap_caps_aligned_calloc(16, 1, VIDEO_W * VIDEO_H_MAX, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_aligned_calloc(16, 1, VIDEO_W * VIDEO_H_MAX, MALLOC_CAP_8BIT);
    return p;
}

void app_main(void)
{
    console_start();
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
        ESP_LOGE(TAG, "no PSRAM found: frames use internal RAM, Wi-Fi may run short");
    s_cmds = xQueueCreate(16, sizeof(char));
    s_fb[0] = alloc_frame();
    s_fb[1] = alloc_frame();
    ESP_ERROR_CHECK(s_cmds && s_fb[0] && s_fb[1] ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(capture_init(s_fs_hz));
    video_init(&s_video, capture_ring(), CAPTURE_RING_BYTES, s_fs_hz, s_fb[0], on_field, NULL);
    ESP_ERROR_CHECK(stream_start());
#ifdef CONFIG_BRIDGE_TEST_PATTERN_AT_BOOT
    set_source(SOURCE_TEST_NTSC);
#else
    set_source(SOURCE_LIVE);
#endif
    if (s_source == SOURCE_LIVE && !capture_running()) ESP_LOGE(TAG, "camera port capture failed to start");
    TaskHandle_t decoder = NULL;
    xTaskCreatePinnedToCore(decode_task, "decode", 4096, NULL, configMAX_PRIORITIES - 3, &decoder, 1);
    capture_set_notify(decoder);
    ESP_LOGI(TAG, "ready: join Wi-Fi \"%s\", open http://192.168.4.1/  ('h' here for keys)", CONFIG_BRIDGE_WIFI_SSID);
}
