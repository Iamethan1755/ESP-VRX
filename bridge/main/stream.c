/* SPDX-License-Identifier: GPL-3.0-only */
/* Wi-Fi access point, web page and MJPEG stream, see stream.h.
 *
 * Port 80: the page, /status (JSON), /cmd?c=X (picture commands) and
 * /snapshot.jpg. Port 81: /stream, multipart MJPEG. Each viewer gets its
 * own task (async request), wakes on a notification when a JPEG is ready
 * and sends it as one write on a TCP_NODELAY socket: no polling delay, no
 * chunk framing, no Nagle hold-back of a frame's last bytes. */
#include "stream.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "denoise.h"
#include "mbox.h"
#include "mdns.h"
#include "bridge.h"
#include "c5link.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define JPEG_MAX (96 * 1024)
#define BOUNDARY "vrxframe"
#define MAX_VIEWERS 3

static const char *TAG = "stream";
extern const char index_html[] asm("_binary_index_html_start");

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_encoder;
static uint8_t *s_jpeg, *s_spare;      /* published JPEG and the encoder's next output */
static size_t s_jpeg_len;
static volatile uint32_t s_seq;
static volatile int s_clients;
static float s_fps;
static uint32_t s_frames;
static float s_ms_nr, s_ms_conv, s_ms_jpeg;   /* encoder task per frame: denoise, YCbYCr bands, JPEG */
static esp_netif_t *s_ap_netif;

/* Per-viewer delivery for the console's w: new frames that went out,
 * frames skipped because the previous send was still running, and how long
 * one frame's send took (the Wi-Fi link's share), per 2 s window. */
typedef struct { uint32_t sent, skipped, bytes, send_us_sum, send_us_max, window_us; } viewer_stats_t;
static viewer_stats_t s_vstat_last[MAX_VIEWERS];

/* Viewer slots: the request copy and task of each open /stream. */
static portMUX_TYPE s_vmux = portMUX_INITIALIZER_UNLOCKED;
static bool s_viewer_used[MAX_VIEWERS];
static httpd_req_t *s_viewer_req[MAX_VIEWERS];
static TaskHandle_t s_viewer_task[MAX_VIEWERS];
static int64_t s_viewer_since[MAX_VIEWERS];
static volatile bool s_viewer_kick[MAX_VIEWERS];   /* make room: this viewer closes */
/* Held while notifying and while a viewer task removes its handle before
 * deleting itself: a copied handle could otherwise be notified after the
 * task was gone (the idle task on core 1 may free it at any time). */
static SemaphoreHandle_t s_notify_lock;

static void notify_viewers(void)
{
    xSemaphoreTake(s_notify_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_VIEWERS; i++) {
        TaskHandle_t t = s_viewer_task[i];
        if (t) xTaskNotifyGive(t);
    }
    xSemaphoreGive(s_notify_lock);
}

static void *alloc_big(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

/* Frame mailbox between the decoder (core 1) and its readers here, the
 * encoder and the screen (mbox.h): the encoder always starts on the newest
 * picture and the decoder never waits or skips a submit. */
enum { READER_JPEG, READER_DISPLAY };
static portMUX_TYPE s_mbox_mux = portMUX_INITIALIZER_UNLOCKED;
static mbox_t s_mbox;
#ifdef CONFIG_BRIDGE_SPI_DISPLAY
static volatile TaskHandle_t s_display;   /* set by the display task once its panel is up */
#endif

uint8_t *stream_submit(uint8_t *fb, int w, int h, video_fmt_t fmt)
{
    portENTER_CRITICAL(&s_mbox_mux);
    uint8_t *next = mbox_submit(&s_mbox, fb, w, h, (int)fmt);
    portEXIT_CRITICAL(&s_mbox_mux);
    if (next != fb && s_encoder) xTaskNotifyGive(s_encoder);
#ifdef CONFIG_BRIDGE_SPI_DISPLAY
    if (next != fb && s_display) xTaskNotifyGive(s_display);
#endif
    return next;
}

static uint8_t *frame_take(int reader, int *w, int *h, video_fmt_t *fmt)
{
    int f = VIDEO_FMT_GRAY;
    portENTER_CRITICAL(&s_mbox_mux);
    uint8_t *fb = mbox_take(&s_mbox, reader, w, h, &f);
    portEXIT_CRITICAL(&s_mbox_mux);
    *fmt = f == VIDEO_FMT_COLOUR ? VIDEO_FMT_COLOUR : VIDEO_FMT_GRAY;
    return fb;
}

static void frame_release(const uint8_t *fb)
{
    portENTER_CRITICAL(&s_mbox_mux);
    mbox_release(&s_mbox, fb);
    portEXIT_CRITICAL(&s_mbox_mux);
}

#ifdef CONFIG_BRIDGE_SPI_DISPLAY
void stream_display_attach(void) { s_display = xTaskGetCurrentTaskHandle(); }

const uint8_t *stream_display_take(int *w, int *h, video_fmt_t *fmt) { return frame_take(READER_DISPLAY, w, h, fmt); }

void stream_display_release(const uint8_t *fb) { frame_release(fb); }
#endif

/* Temporal denoise state (encoder task only, plus the on/off switch). */
static uint8_t *s_nr;
static volatile bool s_nr_on;
static bool s_nr_valid;
static int s_nr_h;
static video_fmt_t s_nr_fmt;
/* Colour: the encoder takes YCbYCr one block (16 rows) at a time from an
 * internal-RAM band built from the frame's planes, so no YCbYCr frame is
 * written to and read back from PSRAM (the decoder's own PSRAM writes slow
 * down with every byte moved on the PSRAM bus). */
static uint8_t *s_band;
static int s_band_bytes;

/* Rows y0 .. y0 + rows - 1 of a colour frame's planes (video.h) as YCbYCr:
 * chroma sample k covers pixels 5k .. 5k + 4, one chroma row two rows.
 * With `nr`, each row is denoised on the way (nr: the running picture), so
 * every row is read from PSRAM once (a separate denoise pass and a band
 * pass over its result read each row twice: ~3 ms a frame). */
static void planes_band(uint8_t *dst, const uint8_t *src, uint8_t *nr, bool first, int y0, int rows)
{
    _Static_assert(VIDEO_W == 5 * VIDEO_CW, "5 pixels per chroma sample");
    uint8_t y[VIDEO_W], c[VIDEO_CHROMA_ROW];
    uint32_t *row = (uint32_t *)dst;
    for (int r = y0; r < y0 + rows; r++, row += VIDEO_W / 2) {
        memcpy(y, src + (size_t)r * VIDEO_W, VIDEO_W);
        if (nr) denoise_row(nr + (size_t)r * VIDEO_W, y, VIDEO_W, first);
        if (r == y0 || !(r & 1)) {   /* bands start on even rows */
            const size_t off = VIDEO_CHROMA_OFF + (size_t)(r / 2) * VIDEO_CHROMA_ROW;
            memcpy(c, src + off, sizeof c);
            if (nr) denoise_row(nr + off, c, VIDEO_CHROMA_ROW, first);
        }
        /* Pair p takes chroma sample (2p + 1) / 5: in every ten pixels (five
         * pairs) the first two pairs take one sample, the other three the
         * next. Unrolled per ten pixels (a table lookup per pair cost ~25
         * cycles a pair). */
        const uint8_t *yy = y, *cb = c, *cr = c + VIDEO_CW;
        uint32_t *o = row;
        for (int m = 0; m < VIDEO_CW / 2; m++, yy += 10, cb += 2, cr += 2, o += 5) {
            const uint32_t c0 = (uint32_t)cb[0] << 8 | (uint32_t)cr[0] << 24, c1 = (uint32_t)cb[1] << 8 | (uint32_t)cr[1] << 24;
            o[0] = yy[0] | (uint32_t)yy[1] << 16 | c0;
            o[1] = yy[2] | (uint32_t)yy[3] << 16 | c0;
            o[2] = yy[4] | (uint32_t)yy[5] << 16 | c1;
            o[3] = yy[6] | (uint32_t)yy[7] << 16 | c1;
            o[4] = yy[8] | (uint32_t)yy[9] << 16 | c1;
        }
    }
}

void stream_set_denoise(bool on)
{
    s_nr_valid = false;
    s_nr_on = on && s_nr;
}

bool stream_denoise(void) { return s_nr_on; }

static void encoder_task(void *arg)
{
    (void)arg;
    jpeg_enc_handle_t enc = NULL;
    int enc_w = 0, enc_h = 0;
    video_fmt_t enc_fmt = VIDEO_FMT_GRAY;
    int64_t window = esp_timer_get_time(), us_nr = 0, us_conv = 0, us_jpeg = 0;
    uint32_t count = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int w, h;
        video_fmt_t fmt;
        uint8_t *fb = frame_take(READER_JPEG, &w, &h, &fmt);
        if (!fb) continue;
        const int64_t t0 = esp_timer_get_time();
        if (!enc || enc_w != w || enc_h != h || enc_fmt != fmt) {
            if (enc) jpeg_enc_close(enc);
            enc = NULL;
            jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
            cfg.width = w;
            cfg.height = h;
            /* Colour: YCbYCr in, 4:2:0 out (one chroma row per two rows anyway). */
            const bool colour = fmt == VIDEO_FMT_COLOUR;
            cfg.src_type = colour ? JPEG_PIXEL_FORMAT_YCbYCr : JPEG_PIXEL_FORMAT_GRAY;
            cfg.subsampling = colour ? JPEG_SUBSAMPLE_420 : JPEG_SUBSAMPLE_GRAY;
            cfg.quality = CONFIG_BRIDGE_JPEG_QUALITY;
            if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG encoder %dx%d failed to open", w, h);
                enc = NULL;
                frame_release(fb);
                continue;
            }
            enc_w = w;
            enc_h = h;
            enc_fmt = fmt;
            const int block = colour ? jpeg_enc_get_block_size(enc) : 0;
            if (block > s_band_bytes) {
                heap_caps_free(s_band);
                s_band = heap_caps_aligned_alloc(16, (size_t)block, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!s_band) s_band = heap_caps_aligned_alloc(16, (size_t)block, MALLOC_CAP_SPIRAM);
                s_band_bytes = s_band ? block : 0;
            }
            if (colour && (!s_band || block % (2 * VIDEO_W))) {
                ESP_LOGE(TAG, "JPEG block of %d bytes: no colour encoding", block);
                jpeg_enc_close(enc);
                enc = NULL;
                frame_release(fb);
                continue;
            }
        }
        const uint8_t *src = fb;
        const bool nr = s_nr && s_nr_on, nr_first = !s_nr_valid || s_nr_h != h || s_nr_fmt != fmt;
        if (nr) {
            s_nr_valid = true;
            s_nr_h = h;
            s_nr_fmt = fmt;
        }
        if (nr && fmt == VIDEO_FMT_GRAY) {
            /* Temporal denoise here, on core 0: the result (s_nr) is what
             * gets encoded, and the decoder's frame is free right after.
             * Colour frames are denoised band by band below. */
            denoise_frame(s_nr, fb, w, h, nr_first);
            src = s_nr;
            frame_release(fb);
            fb = NULL;
        }
        int out_len = 0;
        jpeg_error_t err = JPEG_ERR_FAIL;
        const int64_t t1 = esp_timer_get_time();
        us_nr += t1 - t0;
        if (fmt == VIDEO_FMT_COLOUR) {
            const int block = jpeg_enc_get_block_size(enc), rows = block / (2 * VIDEO_W);
            for (int y0 = 0; y0 < h; y0 += rows) {
                const int64_t b0 = esp_timer_get_time();
                planes_band(s_band, src, nr ? s_nr : NULL, nr_first, y0, rows);
                us_conv += esp_timer_get_time() - b0;
                err = jpeg_enc_process_with_block(enc, s_band, block, s_spare, JPEG_MAX, &out_len);
                if (err < JPEG_ERR_OK) break;
            }
        } else {
            err = jpeg_enc_process(enc, src, w * h, s_spare, JPEG_MAX, &out_len);
        }
        us_jpeg += esp_timer_get_time() - t1;
        if (fb) frame_release(fb);
        if (err != JPEG_ERR_OK || out_len <= 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t *t = s_jpeg;
        s_jpeg = s_spare;
        s_spare = t;
        s_jpeg_len = (size_t)out_len;
        s_seq++;
        xSemaphoreGive(s_lock);
        notify_viewers();
        /* One tick for the idle task (task watchdog) and the low-priority
         * mDNS task: with a noisy picture the encoder runs back to back.
         * After the publish, so the frame does not wait for it. */
        vTaskDelay(1);

        count++;
        int64_t now = esp_timer_get_time();
        if (now - window >= 1000000) {
            s_fps = count * 1e6f / (float)(now - window);
            s_ms_nr = count ? us_nr / 1000.0f / count : 0;
            s_ms_conv = count ? us_conv / 1000.0f / count : 0;
            s_ms_jpeg = count ? (us_jpeg - us_conv) / 1000.0f / count : 0;
            us_nr = us_conv = us_jpeg = 0;
            s_frames += count;
            count = 0;
            window = now;
        }
    }
}

size_t stream_copy_jpeg(uint8_t *dst, size_t max)
{
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_jpeg_len && s_jpeg_len <= max) {
        memcpy(dst, s_jpeg, s_jpeg_len);
        n = s_jpeg_len;
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t stream_copy_jpeg_new(uint8_t *dst, size_t max, uint32_t *seq)
{
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_seq != *seq && s_jpeg_len && s_jpeg_len <= max) {
        memcpy(dst, s_jpeg, s_jpeg_len);
        n = s_jpeg_len;
        *seq = s_seq;
    }
    xSemaphoreGive(s_lock);
    return n;
}

void stream_timing(float *nr_ms, float *conv_ms, float *jpeg_ms)
{
    *nr_ms = s_ms_nr;
    *conv_ms = s_ms_conv;
    *jpeg_ms = s_ms_jpeg;
}

void stream_stats(float *fps, uint32_t *jpeg_bytes, uint32_t *frames, int *clients)
{
    *fps = s_fps;
    *jpeg_bytes = (uint32_t)s_jpeg_len;
    *frames = s_frames;
    *clients = s_clients;
}

int stream_wifi_stations(void)
{
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : -1;
}

/* ---- HTTP ---- */

static esp_err_t index_get(httpd_req_t *req)
{
    printf("HTTP page requested\n");
    httpd_resp_set_type(req, "text/html");
    /* Always the page of the firmware that runs: Safari kept an old one. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char buf[1536];
    int n = bridge_status_json(buf, sizeof buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t cmd_get(httpd_req_t *req)
{
    char query[32], val[4];
    bool ok = httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
              httpd_query_key_value(query, "c", val, sizeof val) == ESP_OK &&
              val[0] && !val[1] && bridge_command(val[0]);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown command");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
}

/* /c5?k=X: one receiver key over the UART link; /c5?ch=N: tune straight
 * to channel N (0..47, see c5link.h). */
static esp_err_t c5_get(httpd_req_t *req)
{
    char query[32], val[4];
    const bool q = httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK;
    /* 503 when the key line is not free (no receiver listening, see
     * c5link.c): the page then shows the pick as not applied. */
    if (q && httpd_query_key_value(query, "ch", val, sizeof val) == ESP_OK) {
        char *end = NULL;
        const long ch = strtol(val, &end, 10);
        if (!val[0] || *end || ch < 0 || ch >= C5LINK_CHANNELS)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel 0..47");
        if (!c5link_set_channel((int)ch))
            return httpd_resp_send_custom_err(req, "503 Service Unavailable", "receiver not listening on the key line");
        printf("C5 <- channel %ld\n", ch);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "ok");
    }
    const bool valid = q && httpd_query_key_value(query, "k", val, sizeof val) == ESP_OK &&
                       val[0] && !val[1] && strchr(C5LINK_KEYS, val[0]);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown receiver key");
    if (!c5link_send(val[0]))
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "receiver not listening on the key line");
    printf("C5 <- %c\n", val[0]);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    uint8_t *buf = alloc_big(JPEG_MAX);
    if (!buf) return httpd_resp_send_500(req);
    size_t n = stream_copy_jpeg(buf, JPEG_MAX);
    esp_err_t err;
    if (!n) {
        err = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame yet");
    } else {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=esp-vrx.jpg");
        err = httpd_resp_send(req, (const char *)buf, (ssize_t)n);
    }
    free(buf);
    return err;
}

/* GET /bench: 1 MiB of filler, timed here (console line BENCH): the Wi-Fi
 * link's own speed to that device, without the browser's MJPEG handling. */
static esp_err_t bench_get(httpd_req_t *req)
{
    enum { TOTAL = 1024 * 1024, CHUNK = 4096 };
    char *buf = malloc(CHUNK);
    if (!buf) return httpd_resp_send_500(req);
    memset(buf, '.', CHUNK);
    buf[CHUNK - 1] = '\n';
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    for (int sent = 0; sent < TOTAL && err == ESP_OK; sent += CHUNK) err = httpd_resp_send_chunk(req, buf, CHUNK);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    const int64_t dt = esp_timer_get_time() - t0;
    free(buf);
    if (err == ESP_OK) printf("BENCH %d KB in %lld ms: %.0f kB/s\n", TOTAL / 1024, dt / 1000, TOTAL / 1024.0 / (dt / 1e6));
    else printf("BENCH aborted after %lld ms\n", dt / 1000);
    return err;
}

static bool send_all(httpd_req_t *req, const uint8_t *p, size_t n)
{
    while (n) {
        int sent = httpd_send(req, (const char *)p, n);
        if (sent <= 0) return false;
        p += sent;
        n -= (size_t)sent;
    }
    return true;
}

/* Slot bookkeeping, all under s_vmux. */
static void release_slot(int slot)
{
    portENTER_CRITICAL(&s_vmux);
    s_viewer_task[slot] = NULL;
    s_viewer_req[slot] = NULL;
    s_viewer_used[slot] = false;
    portEXIT_CRITICAL(&s_vmux);
}

static void viewer_task(void *arg)
{
    const int slot = (int)(intptr_t)arg;
    /* The task registers itself: it outranks the HTTP task and may finish
     * (client gone at once) before xTaskCreate even returns there, which
     * then stored the handle of a deleted task for notify_viewers(). */
    portENTER_CRITICAL(&s_vmux);
    s_viewer_task[slot] = xTaskGetCurrentTaskHandle();
    s_viewer_since[slot] = esp_timer_get_time();
    s_viewer_kick[slot] = false;
    httpd_req_t *req = s_viewer_req[slot];
    s_clients++;
    portEXIT_CRITICAL(&s_vmux);
    const int fd = httpd_req_to_sockfd(req);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* ?raw=1: the same bytes as plain binary for the page's own reader.
     * WebKit (iPhone Safari) handles multipart/x-mixed-replace itself and a
     * fetch() of it closed at once (board, 2026-10-18). */
    char query[32], val[4];
    const bool raw = httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
                     httpd_query_key_value(query, "raw", val, sizeof val) == ESP_OK && val[0] == '1';
    printf("HTTP video stream opened (viewer %d%s)\n", slot, raw ? ", raw" : "");

    /* The response is written raw: one multipart part per send, no chunking. */
    static const char head_mjpeg[] = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n"
                                     "Access-Control-Allow-Origin: *\r\n"
                                     "Cache-Control: no-store\r\n"
                                     "Connection: close\r\n\r\n";
    static const char head_raw[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/octet-stream\r\n"
                                   "X-Content-Type-Options: nosniff\r\n"
                                   "Access-Control-Allow-Origin: *\r\n"
                                   "Cache-Control: no-store\r\n"
                                   "Connection: close\r\n\r\n";
    uint8_t *buf = alloc_big(JPEG_MAX + 128);
    bool ok = buf && (raw ? send_all(req, (const uint8_t *)head_raw, sizeof head_raw - 1)
                          : send_all(req, (const uint8_t *)head_mjpeg, sizeof head_mjpeg - 1));
    viewer_stats_t st = {0};
    int64_t window_t0 = esp_timer_get_time();
    uint32_t last_seq = 0;
    while (ok) {
        /* Woken by the encoder for each new JPEG; resend the last one after
         * 2 s without a frame so the connection stays alive. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        if (s_viewer_kick[slot]) break;
        /* A phone that sleeps with the page open keeps the connection but
         * reads nothing: the send then times out after the server's
         * send_wait_timeout (5 s) and the viewer closes, freeing the slot.
         * (A select() for writability before picking the frame let only
         * ~1 frame/s through on the board, 2026-10-19.) */
        size_t len = 0;
        int h = 0;
        uint32_t seq = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        seq = s_seq;
        if (s_jpeg_len) {
            h = snprintf((char *)buf, 128, "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)s_jpeg_len);
            memcpy(buf + h, s_jpeg, s_jpeg_len);
            len = s_jpeg_len;
        }
        xSemaphoreGive(s_lock);
        if (!len) continue;
        memcpy(buf + h + len, "\r\n", 2);
        const int64_t t0 = esp_timer_get_time();
        ok = send_all(req, buf, (size_t)h + len + 2);
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        if (seq != last_seq) {   /* not the keep-alive resend */
            if (last_seq && seq - last_seq > 1) st.skipped += seq - last_seq - 1;
            last_seq = seq;
            st.sent++;
            st.bytes += (uint32_t)len;
            st.send_us_sum += dt;
            if (dt > st.send_us_max) st.send_us_max = dt;
        }
        const int64_t now = esp_timer_get_time();
        if (now - window_t0 >= 2000000) {
            st.window_us = (uint32_t)(now - window_t0);
            s_vstat_last[slot] = st;
            st = (viewer_stats_t){0};
            window_t0 = now;
        }
    }
    s_vstat_last[slot] = (viewer_stats_t){0};

    printf("HTTP video stream closed (viewer %d)\n", slot);
    xSemaphoreTake(s_notify_lock, portMAX_DELAY);
    s_viewer_task[slot] = NULL;   /* no notification reaches this task from here on */
    xSemaphoreGive(s_notify_lock);
    free(buf);
    httpd_handle_t server = req->handle;
    httpd_req_async_handler_complete(req);
    httpd_sess_trigger_close(server, fd);
    portENTER_CRITICAL(&s_vmux);
    s_clients--;
    portEXIT_CRITICAL(&s_vmux);
    release_slot(slot);
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    int slot = -1;
    portENTER_CRITICAL(&s_vmux);
    for (int i = 0; i < MAX_VIEWERS && slot < 0; i++) {
        if (!s_viewer_used[i]) {
            s_viewer_used[i] = true;
            slot = i;
        }
    }
    if (slot < 0) {
        /* All slots taken (often by connections a phone left behind): the
         * oldest viewer makes room, this one retries (the page does). */
        int oldest = 0;
        for (int i = 1; i < MAX_VIEWERS; i++)
            if (s_viewer_since[i] < s_viewer_since[oldest]) oldest = i;
        s_viewer_kick[oldest] = true;
        portEXIT_CRITICAL(&s_vmux);
        notify_viewers();
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "too many viewers: the oldest one closes, retry");
    }
    portEXIT_CRITICAL(&s_vmux);

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        release_slot(slot);
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_vmux);
    s_viewer_req[slot] = copy;
    portEXIT_CRITICAL(&s_vmux);
    if (xTaskCreatePinnedToCore(viewer_task, "viewer", 4096, (void *)(intptr_t)slot, 6, NULL, 0) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        release_slot(slot);
        return ESP_FAIL;
    }
    return ESP_OK;   /* the viewer task owns the connection (and its slot) now */
}

static httpd_handle_t start_server(uint16_t port, uint16_t ctrl_port, const httpd_uri_t *uris, size_t n)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.ctrl_port = ctrl_port;
    cfg.core_id = 0;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server on port %u failed", port);
        return NULL;
    }
    for (size_t k = 0; k < n; k++) httpd_register_uri_handler(h, &uris[k]);
    return h;
}

/* Console lines for every join, leave and address, to debug phone connections. */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        printf("WIFI access point up\n");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        printf("WIFI device " MACSTR " joined\n", MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        printf("WIFI device " MACSTR " left (reason %d)\n", MAC2STR(e->mac), e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        printf("WIFI device " MACSTR " got " IPSTR "\n", MAC2STR(e->mac), IP2STR(&e->ip));
    }
}

void stream_print_wifi(void)
{
    wifi_config_t cfg = {0};
    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    esp_netif_ip_info_t ip = {0};
    if (s_ap_netif) esp_netif_get_ip_info(s_ap_netif, &ip);
    int8_t power = 0;
    esp_wifi_get_max_tx_power(&power);
    uint8_t primary = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    printf("WIFI ssid=%s channel=%u auth=%s ip=" IPSTR " tx_power=%.1fdBm\n", (char *)cfg.ap.ssid, primary,
           cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2", IP2STR(&ip.ip), power / 4.0);
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        for (int i = 0; i < list.num; i++)
            printf("WIFI device " MACSTR " rssi=%d\n", MAC2STR(list.sta[i].mac), list.sta[i].rssi);
        if (!list.num) printf("WIFI no devices joined\n");
    }
    for (int i = 0; i < MAX_VIEWERS; i++) {
        const viewer_stats_t v = s_vstat_last[i];
        if (!v.window_us) continue;
        const float sec = v.window_us / 1e6f;
        printf("VIEWER %d fps=%.1f skipped/s=%.1f kB/s=%.0f send_ms avg=%.1f max=%.1f\n", i, v.sent / sec,
               v.skipped / sec, v.bytes / 1024.0f / sec, v.sent ? v.send_us_sum / 1000.0f / v.sent : 0.0f,
               v.send_us_max / 1000.0f);
    }
}

/* The bridge once had a home Wi-Fi option (AP+STA): erase a network it
 * stored, ours ("bridge" sta_ssid/sta_pass) and the driver's copy, once. */
static void forget_old_home_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = 0;
    const bool stored = nvs_get_str(h, "sta_ssid", NULL, &n) == ESP_OK;
    if (stored) {
        nvs_erase_key(h, "sta_ssid");
        nvs_erase_key(h, "sta_pass");
        nvs_commit(h);
    }
    nvs_close(h);
    if (stored && nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        printf("WIFI erased the stored home network (feature removed)\n");
    }
}

static esp_err_t wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, wifi_event, NULL);
    forget_old_home_wifi();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;   /* the AP is configured on every boot: no flash writes for it */
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* The driver logs dozens of lines per weak-link drop; the WIFI lines
     * from wifi_event() already report joins and leaves. */
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    wifi_config_t ap = {0};
    const char *ssid = CONFIG_BRIDGE_WIFI_SSID, *pass = CONFIG_BRIDGE_WIFI_PASSWORD;
    strlcpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = CONFIG_BRIDGE_WIFI_CHANNEL;
    ap.ap.max_connection = 4;
    if (strlen(pass) >= 8) {
        strlcpy((char *)ap.ap.password, pass, sizeof ap.ap.password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (pass[0]) ESP_LOGW(TAG, "Wi-Fi password shorter than 8 characters: network is OPEN");
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power((int8_t)(CONFIG_BRIDGE_WIFI_TX_POWER_DBM * 4));
    ESP_LOGI(TAG, "Wi-Fi \"%s\" on channel %d: open http://192.168.4.1/", ssid, CONFIG_BRIDGE_WIFI_CHANNEL);

    /* http://esp-vrx.local/ besides 192.168.4.1 */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("esp-vrx");
        mdns_instance_name_set("ESP-VRX");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS unavailable: use the IP address");
    }
    return ESP_OK;
}

esp_err_t stream_start(uint8_t *const *frames)
{
    bool frames_ok = true;
    for (int i = 0; i < STREAM_FRAMES; i++) frames_ok = frames_ok && frames[i];
    if (!frames_ok) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_mbox_mux);
    mbox_init(&s_mbox, frames, STREAM_FRAMES - 2);
    portEXIT_CRITICAL(&s_mbox_mux);
    s_lock = xSemaphoreCreateMutex();
    s_notify_lock = xSemaphoreCreateMutex();
    s_jpeg = alloc_big(JPEG_MAX);
    s_spare = alloc_big(JPEG_MAX);
    /* The denoised picture is the JPEG input: 16-byte aligned for the encoder. */
    const size_t frame = VIDEO_W * VIDEO_H_MAX * VIDEO_BPP_MAX;   /* gray, or colour planes */
    s_nr = heap_caps_aligned_calloc(16, 1, frame, MALLOC_CAP_SPIRAM);
    if (!s_nr) s_nr = heap_caps_aligned_calloc(16, 1, frame, MALLOC_CAP_8BIT);
    s_nr_on = s_nr != NULL;   /* on by default */
    if (!s_lock || !s_notify_lock || !s_jpeg || !s_spare) return ESP_ERR_NO_MEM;
    /* Low priority on core 0: encoding a noisy picture takes longer than a
     * frame, so the encoder runs nonstop and must only get what the web
     * server, viewers and USB console leave (at 5 it starved the console). */
    xTaskCreatePinnedToCore(encoder_task, "jpeg", 6144, NULL, 2, &s_encoder, 0);

    ESP_ERROR_CHECK(wifi_start());
    static const httpd_uri_t page_uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/cmd", .method = HTTP_GET, .handler = cmd_get},
        {.uri = "/c5", .method = HTTP_GET, .handler = c5_get},
        {.uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/bench", .method = HTTP_GET, .handler = bench_get},
    };
    static const httpd_uri_t stream_uris[] = {
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_get},
    };
    if (!start_server(80, 32768, page_uris, sizeof page_uris / sizeof page_uris[0])) return ESP_FAIL;
    if (!start_server(81, 32769, stream_uris, 1)) return ESP_FAIL;
    return ESP_OK;
}
