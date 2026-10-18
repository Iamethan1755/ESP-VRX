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
#include "homewifi.h"
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
#define BOUNDARY "c5vrxframe"
#define MAX_VIEWERS 3

static const char *TAG = "stream";
extern const char index_html[] asm("_binary_index_html_start");

static SemaphoreHandle_t s_job, s_lock;
static volatile bool s_busy;
static const uint8_t *s_job_fb;
static int s_job_w, s_job_h;
static uint8_t *s_jpeg, *s_spare;      /* published JPEG and the encoder's next output */
static size_t s_jpeg_len;
static volatile uint32_t s_seq;
static volatile int s_clients;
static float s_fps;
static uint32_t s_frames;
static esp_netif_t *s_ap_netif;

/* Viewer slots: the request copy and task of each open /stream. */
static portMUX_TYPE s_vmux = portMUX_INITIALIZER_UNLOCKED;
static bool s_viewer_used[MAX_VIEWERS];
static httpd_req_t *s_viewer_req[MAX_VIEWERS];
static TaskHandle_t s_viewer_task[MAX_VIEWERS];

static void notify_viewers(void)
{
    TaskHandle_t tasks[MAX_VIEWERS];
    portENTER_CRITICAL(&s_vmux);
    memcpy(tasks, s_viewer_task, sizeof tasks);
    portEXIT_CRITICAL(&s_vmux);
    for (int i = 0; i < MAX_VIEWERS; i++)
        if (tasks[i]) xTaskNotifyGive(tasks[i]);
}

static void *alloc_big(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

bool stream_submit(const uint8_t *fb, int w, int h)
{
    if (s_busy || !s_job) return false;
    s_job_fb = fb;
    s_job_w = w;
    s_job_h = h;
    s_busy = true;
    xSemaphoreGive(s_job);
    return true;
}

/* Temporal denoise state (encoder task only, plus the on/off switch). */
static uint8_t *s_nr;
static volatile bool s_nr_on;
static bool s_nr_valid;
static int s_nr_h;

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
    int64_t window = esp_timer_get_time();
    uint32_t count = 0;
    for (;;) {
        xSemaphoreTake(s_job, portMAX_DELAY);
        if (!enc || enc_w != s_job_w || enc_h != s_job_h) {
            if (enc) jpeg_enc_close(enc);
            enc = NULL;
            jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
            cfg.width = s_job_w;
            cfg.height = s_job_h;
            cfg.src_type = JPEG_PIXEL_FORMAT_GRAY;
            cfg.subsampling = JPEG_SUBSAMPLE_GRAY;
            cfg.quality = CONFIG_BRIDGE_JPEG_QUALITY;
            if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG encoder %dx%d failed to open", s_job_w, s_job_h);
                enc = NULL;
                s_busy = false;
                continue;
            }
            enc_w = s_job_w;
            enc_h = s_job_h;
        }
        const uint8_t *src = s_job_fb;
        if (s_nr && s_nr_on) {
            /* Temporal denoise here, on core 0: the result (s_nr) is what
             * gets encoded, and the decoder's frame is free right after. */
            const bool first = !s_nr_valid || s_nr_h != s_job_h;
            denoise_frame(s_nr, s_job_fb, s_job_w, s_job_h, first);
            s_nr_valid = true;
            s_nr_h = s_job_h;
            src = s_nr;
            s_busy = false;
        }
        int out_len = 0;
        jpeg_error_t err = jpeg_enc_process(enc, src, s_job_w * s_job_h, s_spare, JPEG_MAX, &out_len);
        s_busy = false;   /* the decoder may render into that frame again */
        if (err != JPEG_ERR_OK || out_len <= 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t *t = s_jpeg;
        s_jpeg = s_spare;
        s_spare = t;
        s_jpeg_len = (size_t)out_len;
        s_seq++;
        xSemaphoreGive(s_lock);
        notify_viewers();

        count++;
        int64_t now = esp_timer_get_time();
        if (now - window >= 1000000) {
            s_fps = count * 1e6f / (float)(now - window);
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

/* /c5?k=X: one receiver key over the UART link (see c5link.h). */
static esp_err_t c5_get(httpd_req_t *req)
{
    char query[32], val[4];
    bool ok = httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
              httpd_query_key_value(query, "k", val, sizeof val) == ESP_OK &&
              val[0] && !val[1] && c5link_send(val[0]);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown receiver key");
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
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=c5vrx.jpg");
        err = httpd_resp_send(req, (const char *)buf, (ssize_t)n);
    }
    free(buf);
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
    httpd_req_t *req = s_viewer_req[slot];
    s_clients++;
    portEXIT_CRITICAL(&s_vmux);
    const int fd = httpd_req_to_sockfd(req);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("HTTP video stream opened (viewer %d)\n", slot);

    /* The response is written raw: one multipart part per send, no chunking. */
    static const char head[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Cache-Control: no-store\r\n"
                               "Connection: close\r\n\r\n";
    uint8_t *buf = alloc_big(JPEG_MAX + 128);
    bool ok = buf && send_all(req, (const uint8_t *)head, sizeof head - 1);
    while (ok) {
        /* Woken by the encoder for each new JPEG; resend the last one after
         * 2 s without a frame so the connection stays alive. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        size_t len = 0;
        int h = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_jpeg_len) {
            h = snprintf((char *)buf, 128, "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)s_jpeg_len);
            memcpy(buf + h, s_jpeg, s_jpeg_len);
            len = s_jpeg_len;
        }
        xSemaphoreGive(s_lock);
        if (!len) continue;
        memcpy(buf + h + len, "\r\n", 2);
        ok = send_all(req, buf, (size_t)h + len + 2);
    }

    printf("HTTP video stream closed (viewer %d)\n", slot);
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
    portEXIT_CRITICAL(&s_vmux);
    if (slot < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too many viewers");

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
    homewifi_print();
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
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* The driver logs dozens of lines per weak-link drop; the WIFI lines
     * from wifi_event() already report joins and leaves. */
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    homewifi_init();

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(homewifi_configured() ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    homewifi_apply();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power((int8_t)(CONFIG_BRIDGE_WIFI_TX_POWER_DBM * 4));
    ESP_LOGI(TAG, "Wi-Fi \"%s\" on channel %d: open http://192.168.4.1/", ssid, CONFIG_BRIDGE_WIFI_CHANNEL);

    /* c5vrx-bridge.local on both networks. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("c5vrx-bridge");
        mdns_instance_name_set("C5VRX Wi-Fi bridge");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS unavailable: use the IP address");
    }
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            const char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* POST /wifi (ssid, pass): join a home network, or forget it with an
 * empty ssid. The password is stored in NVS only, never printed or sent. */
static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[256];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof body)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    char ssid[100] = "", pass[200] = "";
    const bool ok = httpd_query_key_value(body, "ssid", ssid, sizeof ssid) == ESP_OK &&
                    httpd_query_key_value(body, "pass", pass, sizeof pass) == ESP_OK;
    url_decode(ssid);
    url_decode(pass);
    esp_err_t err = ok ? homewifi_set(ssid, pass) : ESP_ERR_INVALID_ARG;
    memset(pass, 0, sizeof pass);
    memset(body, 0, sizeof body);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "network name up to 32 characters, password empty or 8-63 characters");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, err == ESP_OK ? "ok" : "joining, but saving it failed");
}

esp_err_t stream_start(void)
{
    s_job = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    s_jpeg = alloc_big(JPEG_MAX);
    s_spare = alloc_big(JPEG_MAX);
    /* The denoised picture is the JPEG input: 16-byte aligned for the encoder. */
    s_nr = heap_caps_aligned_calloc(16, 1, DENOISE_W_MAX * 288, MALLOC_CAP_SPIRAM);
    if (!s_nr) s_nr = heap_caps_aligned_calloc(16, 1, DENOISE_W_MAX * 288, MALLOC_CAP_8BIT);
    s_nr_on = s_nr != NULL;   /* on by default */
    if (!s_job || !s_lock || !s_jpeg || !s_spare) return ESP_ERR_NO_MEM;
    /* Lowest priority on core 0: encoding a noisy picture takes longer than a
     * frame, so the encoder runs nonstop and must only get what the web
     * server, viewers and USB console leave (at 5 it starved the console). */
    xTaskCreatePinnedToCore(encoder_task, "jpeg", 6144, NULL, 2, NULL, 0);

    ESP_ERROR_CHECK(wifi_start());
    static const httpd_uri_t page_uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/cmd", .method = HTTP_GET, .handler = cmd_get},
        {.uri = "/c5", .method = HTTP_GET, .handler = c5_get},
        {.uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
    };
    static const httpd_uri_t stream_uris[] = {
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_get},
    };
    if (!start_server(80, 32768, page_uris, sizeof page_uris / sizeof page_uris[0])) return ESP_FAIL;
    if (!start_server(81, 32769, stream_uris, 1)) return ESP_FAIL;
    return ESP_OK;
}
