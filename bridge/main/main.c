/* SPDX-License-Identifier: GPL-3.0-only */
/* VRX Bridge: Wi-Fi video for the C5VRX receiver, on a Seeed XIAO ESP32-S3.
 *
 * Reads the 6-bit digital video bus of a C5VRX receiver (XIAO ESP32-C5,
 * DAC pins D4..D9) with the S3 camera port, locks to the composite sync,
 * builds one 320x240 (NTSC, colour or gray) / 320x288 (PAL, gray) frame per
 * field and streams it as MJPEG over the S3's own 2.4 GHz Wi-Fi network.
 *
 * Core 1 runs only the decoder; Wi-Fi, HTTP, JPEG and the console run on
 * core 0. C5VRX itself: https://github.com/Twotoz/C5VRX */
#include <stdio.h>
#include <string.h>
#include "bridge.h"
#include "c5link.h"
#include "capture.h"
#include "console.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
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
static uint8_t *s_fb[STREAM_FRAMES];   /* the decoder renders into one, see stream_submit */
static bridge_source_t s_source;
static QueueHandle_t s_cmds;
static volatile uint32_t s_write_pos;
static uint32_t s_capture_gen;      /* capture restart the decoder is synchronised to */
static int64_t s_last_submit, s_last_field, s_tp_t0, s_retry_at;

/* Long raw recording (console 'W'): the decoder task copies the ring to
 * s_rec instead of decoding until it is full, then wakes s_rec_waiter.
 * The console task (core 0) fills in s_rec* and then raises s_rec_active
 * for the decoder (core 1): fences keep those stores in that order. */
static uint8_t *s_rec;
static uint32_t s_rec_len, s_rec_max, s_rec_rp;
static volatile bool s_rec_active;
static TaskHandle_t s_rec_waiter;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static video_stats_t s_vstats;
static float s_msps, s_load, s_slip_rate;
static uint32_t s_max_pass, s_max_gap, s_max_line, s_max_line_kind;
static float s_drawn_us, s_skipped_us;

const char *bridge_source_name(bridge_source_t s)
{
    return s == SOURCE_LIVE ? "c5" : s == SOURCE_TEST_NTSC ? "test-ntsc" : "test-pal";
}

const char *bridge_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

/* Decoder task: a field is done. Hand it to the JPEG encoder at most
 * CONFIG_BRIDGE_MAX_FPS times a second (always the same field parity at
 * 30 fps, so the picture does not bob) and render into the buffer the
 * mailbox returns. Fields that would be dropped are not drawn at all: at
 * 30 fps that is every other field, half the decoder's work. */
static uint8_t *on_field(uint8_t *done, int w, int h, video_fmt_t fmt, bool rendered, bool *render_next, void *ctx)
{
    (void)ctx;
    const int64_t interval = 1000000 / CONFIG_BRIDGE_MAX_FPS - 2000;
    const int64_t now = esp_timer_get_time();
    /* The field period as it comes: free-running without a signal, fields
     * are longer than the standard's, and a fixed value then skipped every
     * other submit. */
    int64_t field_us = now - s_last_field;
    if (field_us < 12000 || field_us > 40000) field_us = h > 240 ? 20000 : 16683;
    s_last_field = now;
    uint8_t *next = done;
    if (rendered && now - s_last_submit >= interval) {
        next = stream_submit(done, w, h, fmt);
        s_last_submit = now;
    }
    *render_next = now + field_us - s_last_submit >= interval;
    return next;
}

static void set_source(bridge_source_t src)
{
    if (src == SOURCE_LIVE) {
        if (s_tp_ready) testpattern_free(&s_tp);   /* its colour lines take ~20 KiB of internal RAM */
        s_tp_ready = false;
        capture_start();
        s_capture_gen = capture_generation();
        video_reset(&s_video, capture_write_pos());
    } else {
        capture_stop();
        if (s_tp_ready) testpattern_free(&s_tp);
        s_tp_ready = testpattern_init(&s_tp, capture_ring(), CAPTURE_RING_BYTES, s_fs_hz, src == SOURCE_TEST_PAL);
        if (s_tp_ready && src == SOURCE_TEST_NTSC) testpattern_set_colour(&s_tp, true);   /* 75 % colour bars */
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
    case 'f':
        video_set_filter(&s_video, s_video.filter == VIDEO_FILTER_MEDIAN ? VIDEO_FILTER_SMOOTH : VIDEO_FILTER_MEDIAN);
        break;
    case 'd': stream_set_denoise(!stream_denoise()); break;
    case 'm': video_set_colour(&s_video, !s_video.colour); break;
    default: break;
    }
}

bool bridge_command(char c)
{
    if (!c || !strchr("tcjlikfdm", c)) return false;
    return xQueueSend(s_cmds, &c, 0) == pdTRUE;
}

static void decode_task(void *arg)
{
    (void)arg;
    int64_t window = esp_timer_get_time(), busy = 0, last_end = window, max_pass = 0, max_gap = 0;
    uint32_t window_wp = 0;
    int32_t slip_sum = 0;
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
            /* A capture that failed to start (at boot, or after a test) is
             * retried every 2 s instead of leaving the stream dead. */
            if (!capture_running() && !s_rec_active) {
                const int64_t now = esp_timer_get_time();
                if (now - s_retry_at >= 2000000) {
                    s_retry_at = now;
                    if (capture_retry()) ESP_LOGW(TAG, "camera port capture started on retry");
                }
            }
        } else if (s_tp_ready) {
            /* At most 3/4 of a ring per pass: all the time since a long pass in
             * one go (a drawn colour field) overran the ring. Time beyond
             * that is dropped, the pattern then runs a little slow. */
            const int64_t now = esp_timer_get_time();
            uint32_t target = (uint32_t)((now - s_tp_t0) * (int64_t)s_fs_hz / 1000000);
            if ((int32_t)(target - s_tp.pos) > (int32_t)(CAPTURE_RING_BYTES * 3 / 4)) {
                s_tp_t0 += (int64_t)(target - s_tp.pos - CAPTURE_RING_BYTES * 3 / 4) * 1000000 / (int64_t)s_fs_hz;
                target = s_tp.pos + CAPTURE_RING_BYTES * 3 / 4;
            }
            testpattern_run_until(&s_tp, target);
            wp = s_tp.pos;
        }
        s_write_pos = wp;

        if (s_rec_active) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* s_rec* as set before s_rec_active */
            bool done = s_source != SOURCE_LIVE;
            uint32_t avail = wp - s_rec_rp;
            if (avail > CAPTURE_RING_BYTES - 4096) {
                s_rec_len = 0;   /* the DMA overtook us: data lost */
                done = true;
            }
            if (!done) {
                if (avail > s_rec_max - s_rec_len) avail = s_rec_max - s_rec_len;
                const uint8_t *ring = capture_ring();
                const uint32_t start = s_rec_rp & (CAPTURE_RING_BYTES - 1);
                const uint32_t first = CAPTURE_RING_BYTES - start < avail ? CAPTURE_RING_BYTES - start : avail;
                memcpy(s_rec + s_rec_len, ring + start, first);
                memcpy(s_rec + s_rec_len + first, ring, avail - first);
                s_rec_len += avail;
                s_rec_rp += avail;
                done = s_rec_len >= s_rec_max;
            }
            if (done) {
                s_rec_active = false;
                video_reset(&s_video, wp);   /* the decoder skipped this stretch */
                window_wp = wp;
                xTaskNotifyGive(s_rec_waiter);
            }
            ulTaskNotifyTake(pdTRUE, 1);
            continue;
        }

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
            /* Clock slips per second, averaged over ~10 s while colour is shown. */
            const float slips = (float)(st.slip_sum - slip_sum) * 1e6f / (float)(b - window);
            slip_sum = st.slip_sum;
            portENTER_CRITICAL(&s_mux);
            if (st.colour_shown) s_slip_rate += (slips - s_slip_rate) * 0.05f;
            s_vstats = st;
            s_msps = (float)(uint32_t)(wp - window_wp) / (float)(b - window);
            s_load = (float)busy / (float)(b - window);
            s_max_pass = (uint32_t)max_pass;
            s_max_gap = (uint32_t)max_gap;
            s_max_line = s_video.max_line_us;
            s_max_line_kind = s_video.max_line_kind;
            if (s_video.drawn_lines) s_drawn_us = (float)s_video.drawn_us / (float)s_video.drawn_lines;
            if (s_video.skipped_lines) s_skipped_us = (float)s_video.skipped_us / (float)s_video.skipped_lines;
            portEXIT_CRITICAL(&s_mux);
            s_video.max_line_us = 0;
            s_video.drawn_us = s_video.drawn_lines = s_video.skipped_us = s_video.skipped_lines = 0;
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

bool bridge_locked(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool locked = s_vstats.h_locked && s_vstats.v_locked;
    portEXIT_CRITICAL(&s_mux);
    return locked;
}

void bridge_get_status(bridge_status_t *out)
{
    memset(out, 0, sizeof *out);
    portENTER_CRITICAL(&s_mux);
    out->video = s_vstats;
    out->sample_msps = s_msps;
    out->decode_load = s_load;
    out->slip_rate = s_slip_rate;
    out->clock_ppm = s_slip_rate / (float)s_fs_hz * 1e6f;   /* one sample per slip */
    out->max_pass_us = s_max_pass;
    out->max_gap_us = s_max_gap;
    out->max_line_us = s_max_line;
    out->max_line_kind = s_max_line_kind;
    out->drawn_line_us = s_drawn_us;
    out->skipped_line_us = s_skipped_us;
    portEXIT_CRITICAL(&s_mux);
    out->source = s_source;
    out->capture_ok = s_source != SOURCE_LIVE || capture_running();
    stream_stats(&out->fps, &out->jpeg_bytes, &out->jpeg_frames, &out->clients);
    out->stations = stream_wifi_stations();
    out->c5_keys = c5link_sent();
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
        "\"levels\":\"%s\",\"filter\":\"%s\",\"denoise\":%s,\"colour\":%s,\"colour_shown\":%s,\"burst\":%.2f,"
        "\"slips\":%lu,\"clock_ppm\":%.2f,"
        "\"fields\":%lu,\"overruns\":%lu,\"lock_losses\":%lu,\"h_offset_us\":%.1f,"
        "\"v_offset\":%d,\"msps\":%.2f,\"load\":%.2f,\"fps\":%.1f,\"jpeg_bytes\":%lu,\"frames\":%lu,"
        "\"clients\":%d,\"stations\":%d,\"c5_keys\":%lu,\"c5_channel\":%d,\"heap_int\":%lu,\"heap_psram\":%lu,\"uptime\":%lu,"
        "\"reset\":\"%s\"}",
        bridge_source_name(s.source), s.capture_ok ? "true" : "false", v->h_locked ? "true" : "false",
        v->v_locked ? "true" : "false", v->std == VIDEO_STD_PAL ? "PAL" : "NTSC", v->lines_per_field, v->line_us,
        v->sync_level, v->blank_level, v->black_level, v->white_level,
        v->levels == VIDEO_LEVELS_AUTO ? "auto" : "std", v->filter == VIDEO_FILTER_MEDIAN ? "median" : "smooth",
        stream_denoise() ? "true" : "false", v->colour ? "true" : "false", v->colour_shown ? "true" : "false",
        v->burst_level, (unsigned long)v->slips, s.clock_ppm, (unsigned long)v->fields, (unsigned long)v->overruns,
        (unsigned long)v->lock_losses, v->h_offset_tenths_us / 10.0, v->v_offset_lines, s.sample_msps,
        s.decode_load, s.fps, (unsigned long)s.jpeg_bytes, (unsigned long)s.jpeg_frames, s.clients, s.stations, (unsigned long)s.c5_keys, c5link_channel(),
        (unsigned long)s.heap_internal, (unsigned long)s.heap_psram, (unsigned long)s.uptime_s,
        bridge_reset_reason());
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

uint32_t bridge_record_raw(uint8_t *dst, uint32_t n, uint32_t *fs_hz)
{
    if (s_source != SOURCE_LIVE || s_rec_active) return 0;
    s_rec = dst;
    s_rec_max = n;
    s_rec_len = 0;
    s_rec_rp = s_write_pos;
    s_rec_waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0);
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the decoder sees all of the above first */
    s_rec_active = true;
    /* 2 MiB at 20 MS/s is 105 ms */
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))) {
        s_rec_active = false;
        vTaskDelay(pdMS_TO_TICKS(20));   /* let a copy in progress finish */
        return 0;
    }
    *fs_hz = s_fs_hz;
    return s_rec_len;
}

static uint8_t *alloc_frame(void)
{
    const size_t n = VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX;   /* room for a colour (YCbYCr) frame */
    void *p = heap_caps_aligned_calloc(16, 1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_aligned_calloc(16, 1, n, MALLOC_CAP_8BIT);
    return p;
}

void app_main(void)
{
    s_cmds = xQueueCreate(16, sizeof(char));   /* before the console: its keys land here */
    console_start();
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
        ESP_LOGE(TAG, "no PSRAM found: frames use internal RAM, Wi-Fi may run short");
    bool frames_ok = true;
    for (int i = 0; i < STREAM_FRAMES; i++) {
        s_fb[i] = alloc_frame();
        frames_ok = frames_ok && s_fb[i];
    }
    ESP_ERROR_CHECK(s_cmds && frames_ok ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(capture_init(s_fs_hz));
    video_init(&s_video, capture_ring(), CAPTURE_RING_BYTES, s_fs_hz, s_fb[0], on_field, NULL);
#ifdef CONFIG_BRIDGE_COLOUR
    video_set_colour(&s_video, true);
#endif
    if (c5link_init() != ESP_OK) ESP_LOGE(TAG, "receiver key link unavailable");
#ifdef CONFIG_BRIDGE_SPI_DISPLAY
    /* Before Wi-Fi takes the internal RAM the screen's DMA strips need. The
     * screen is optional: without it the stream runs as usual. */
    if (display_start() != ESP_OK) ESP_LOGE(TAG, "SPI screen unavailable");
#endif
    ESP_ERROR_CHECK(stream_start(s_fb));
#ifdef CONFIG_BRIDGE_TEST_PATTERN_AT_BOOT
    set_source(SOURCE_TEST_NTSC);
#else
    set_source(SOURCE_LIVE);
#endif
    if (s_source == SOURCE_LIVE && !capture_running()) ESP_LOGE(TAG, "camera port capture failed to start");
    TaskHandle_t decoder = NULL;
    xTaskCreatePinnedToCore(decode_task, "decode", 6144, NULL, configMAX_PRIORITIES - 3, &decoder, 1);
    capture_set_notify(decoder);
    ESP_LOGI(TAG, "ready: join Wi-Fi \"%s\", open http://192.168.4.1/  ('h' here for keys)", CONFIG_BRIDGE_WIFI_SSID);
}
