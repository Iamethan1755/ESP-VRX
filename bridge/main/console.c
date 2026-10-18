/* USB serial console: one key per command, no Enter needed.
 * Binary data (raw samples, JPEG) goes out base64-encoded between BEGIN/END
 * lines with a CRC-32, read by tools/bridge_tool.py. USB video ('V') sends
 * JPEG frames raw instead, for bridge_tool.py live. */
#include "console.h"
#include <stdio.h>
#include <string.h>
#include "bridge.h"
#include "c5link.h"
#include "capture.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stream.h"

static const char *HELP =
    "C5VRX Wi-Fi bridge keys:\n"
    "  s  status            t  next source (C5 / NTSC test / PAL test)\n"
    "  c  levels auto/std   j l  picture left/right   i k  picture up/down\n"
    "  f  despeckle median/smooth                     d  denoise on/off\n"
    "  R  raw sample dump   F  current JPEG frame     L  loopback self-test\n"
    "  w  Wi-Fi details     G  capture diagnostics    h  this help\n"
    "  C<k>  send receiver key k to the C5 (c channel, b band, s scan, o menu,\n"
    "        n/x menu next/select, e close menu, v video standard)\n"
    "  u  check the C5 key link pin\n"
    "  V v  USB video on/off (binary JPEG frames for bridge_tool.py live)\n"
    "  W  record ~6 fields of raw C5 samples, sent binary (bridge_tool.py rawlong)\n";

static void print_base64(const uint8_t *p, size_t n)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80];
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        line[o++] = tbl[(v >> 18) & 63];
        line[o++] = tbl[(v >> 12) & 63];
        line[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        line[o++] = i + 2 < n ? tbl[v & 63] : '=';
        if (o >= 76 || i + 3 >= n) {
            line[o] = 0;
            puts(line);
            o = 0;
        }
    }
}

static void print_status(void)
{
    bridge_status_t s;
    bridge_get_status(&s);
    const video_stats_t *v = &s.video;
    printf("STATUS source=%s capture=%s h_lock=%d v_lock=%d std=%s lines/field=%d line_us=%.3f "
           "sync=%.1f blank=%.1f map=%.1f..%.1f levels=%s filter=%s denoise=%d fields=%lu overruns=%lu lock_losses=%lu "
           "h_offset=%.1fus v_offset=%d rate=%.2fMS/s load=%.0f%% pass_max=%luus gap_max=%luus line_max=%luus(kind %lu) fps=%.1f jpeg=%lu clients=%d wifi=" CONFIG_BRIDGE_WIFI_SSID " stations=%d "
           "heap_int=%lu heap_psram=%lu up=%lus reset=%s\n",
           bridge_source_name(s.source), s.capture_ok ? "ok" : "FAIL", v->h_locked, v->v_locked,
           v->std == VIDEO_STD_PAL ? "PAL" : "NTSC", v->lines_per_field, v->line_us, v->sync_level,
           v->blank_level, v->black_level, v->white_level, v->levels == VIDEO_LEVELS_AUTO ? "auto" : "std",
           v->filter == VIDEO_FILTER_MEDIAN ? "median" : "smooth", (int)stream_denoise(), (unsigned long)v->fields, (unsigned long)v->overruns, (unsigned long)v->lock_losses,
           v->h_offset_tenths_us / 10.0, v->v_offset_lines, s.sample_msps, s.decode_load * 100,
           (unsigned long)s.max_pass_us, (unsigned long)s.max_gap_us, (unsigned long)s.max_line_us,
           (unsigned long)s.max_line_kind, s.fps,
           (unsigned long)s.jpeg_bytes, s.clients, s.stations, (unsigned long)s.heap_internal, (unsigned long)s.heap_psram,
           (unsigned long)s.uptime_s, bridge_reset_reason());
}

static void dump_raw(void)
{
    const uint32_t max = CAPTURE_RING_BYTES / 2;
    uint8_t *buf = heap_caps_malloc(max, MALLOC_CAP_8BIT);
    if (!buf) {
        puts("RAW ERROR no memory");
        return;
    }
    uint32_t fs = 0;
    uint32_t n = bridge_snapshot_raw(buf, max, &fs);
    printf("RAW BEGIN fs=%lu n=%lu\n", (unsigned long)fs, (unsigned long)n);
    print_base64(buf, n);
    printf("RAW END crc=%08lx\n", (unsigned long)esp_rom_crc32_le(0, buf, n));
    free(buf);
}

static void dump_jpeg(void)
{
    const size_t max = 96 * 1024;
    uint8_t *buf = heap_caps_malloc(max, MALLOC_CAP_8BIT);
    if (!buf) {
        puts("JPEG ERROR no memory");
        return;
    }
    size_t n = stream_copy_jpeg(buf, max);
    if (!n) {
        puts("JPEG ERROR no frame yet");
    } else {
        printf("JPEG BEGIN n=%u\n", (unsigned)n);
        print_base64(buf, n);
        printf("JPEG END crc=%08lx\n", (unsigned long)esp_rom_crc32_le(0, buf, n));
    }
    free(buf);
}

/* USB video: every new JPEG as a line "USBJPEG n=<bytes> crc=<crc32>" and
 * the raw bytes, a STATUS line every second, all from this one task so they
 * never interleave (other tasks' log lines can; the PC drops such a frame by
 * its CRC). Stops by itself when the PC stops reading. */
#define USBVID_CHUNK 1024
static volatile bool s_usbvid_on;
static TaskHandle_t s_usbvid_task;

static bool usb_write_all(const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        const size_t k = n < USBVID_CHUNK ? n : USBVID_CHUNK;
        if (usb_serial_jtag_write_bytes(p, k, pdMS_TO_TICKS(200)) != (int)k) return false;
        p += k;
        n -= k;
    }
    return true;
}

static void usbvid_task(void *arg)
{
    (void)arg;
    const size_t max = 96 * 1024;
    uint8_t *buf = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(max, MALLOC_CAP_8BIT);
    uint32_t seq = 0;
    int64_t next_status = 0;
    for (;;) {
        if (!s_usbvid_on || !buf) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const size_t n = stream_copy_jpeg_new(buf, max, &seq);
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        char head[48];
        const int h = snprintf(head, sizeof head, "USBJPEG n=%u crc=%08lx\n", (unsigned)n,
                               (unsigned long)esp_rom_crc32_le(0, buf, n));
        if (!usb_write_all(head, (size_t)h) || !usb_write_all(buf, n)) {
            s_usbvid_on = false;   /* nobody reading */
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_status) {
            print_status();
            fflush(stdout);
            next_status = now + 1000000;
        }
    }
}

static void usbvid_set(bool on)
{
    if (on && !s_usbvid_task &&
        xTaskCreatePinnedToCore(usbvid_task, "usbvid", 4096, NULL, 4, &s_usbvid_task, 0) != pdPASS) {
        s_usbvid_task = NULL;
        puts("USBVIDEO ERROR no task");
        return;
    }
    printf("USBVIDEO %s\n", on ? "on" : "off");
    fflush(stdout);
    s_usbvid_on = on;
    if (on) xTaskNotifyGive(s_usbvid_task);
}

/* 'W': about six fields of contiguous raw samples for replaying the real
 * signal through the decoder on the PC (bridge_tool.py rawlong), sent like
 * USB video: a line "RAWLONG n=<bytes> fs=<Hz> crc=<crc32>", then the bytes. */
static void dump_raw_long(void)
{
    const uint32_t n = 2u << 20;
    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf) {
        puts("RAWLONG ERROR no memory");
        return;
    }
    uint32_t fs = 0;
    if (bridge_record_raw(buf, n, &fs) != n) {
        puts("RAWLONG ERROR recording failed (source must be the C5)");
    } else {
        char head[64];
        const int h = snprintf(head, sizeof head, "RAWLONG n=%lu fs=%lu crc=%08lx\n", (unsigned long)n,
                               (unsigned long)fs, (unsigned long)esp_rom_crc32_le(0, buf, n));
        fflush(stdout);
        if (!usb_write_all(head, (size_t)h) || !usb_write_all(buf, n)) puts("RAWLONG ERROR nobody reading");
    }
    free(buf);
}

static char read_key(void)
{
    char c = 0;
    while (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
    }
    return c;
}

static void selftest(void)
{
    puts("SELFTEST drives D0..D5 from the S3. Only run it with NOTHING (no C5) connected to them.");
    puts("SELFTEST press y to run, any other key to cancel");
    if (read_key() != 'y') {
        puts("SELFTEST cancelled");
        return;
    }
    char report[256];
    capture_selftest(report, sizeof report);
    puts(report);
}

static void console_task(void *arg)
{
    (void)arg;
    puts(HELP);
    for (;;) {
        char c = read_key();
        switch (c) {
        case 's': print_status(); break;
        case 'R': dump_raw(); break;
        case 'F': dump_jpeg(); break;
        case 'W': dump_raw_long(); break;
        case 'V': usbvid_set(true); break;
        case 'v': usbvid_set(false); break;
        case 'w': stream_print_wifi(); break;
        case 'L': selftest(); break;
        case 'G': {
            static char report[1024];
            capture_debug(report, sizeof report);
            puts(report);
            break;
        }
        case 'C': {
            const char k = read_key();
            printf(c5link_send(k) ? "C5 <- %c\n" : "C5 key %c not accepted (keys: " C5LINK_KEYS ")\n", k);
            break;
        }
        case 'u': {
            char report[160];
            c5link_selftest(report, sizeof report);
            puts(report);
            break;
        }
        case 'h': case '?': puts(HELP); break;
        case '\r': case '\n': case ' ': break;
        default:
            if (bridge_command(c)) printf("COMMAND %c ok\n", c);
            else printf("COMMAND %c unknown ('h' for help)\n", c);
        }
    }
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IOLBF, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 3, NULL, 0);
}
