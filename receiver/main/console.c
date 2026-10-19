/* C5VRX-5 by Twotoz and the C5VRX contributors: USB serial console.
 * Polled, never paces the realtime path. */
#include "receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "hal/usb_serial_jtag_ll.h"
#include "rf.h"
#include "snapshot.h"

extern volatile const char *s_boot_stage;

static void status(void)
{
    printf("C5VRX5 version=%s demod=U85Q_1a09f5c092e8 lane=ultrafine gain_owner=direct_gain_v5 "
           "uptime_s=%lld heap_internal=%u\n", esp_app_get_description()->version,
           esp_timer_get_time() / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    control_print();
    receiver_print();
    profile_print();
    calibration_print();
    transport_print();
}

/* 8190 contiguous IQ bytes as hex lines (tools/capture_iq_snapshot.py). */
static void snapshot(void)
{
    if (s_menu_active || receiver_paused()) { printf("IQSNAP_REFUSED context_busy\n"); return; }
    uint8_t *raw = malloc(C5V4_LEVEL_SAMPLE_BYTES);
    if (!raw) { printf("IQSNAP_REFUSED memory\n"); return; }
    uint32_t epoch = s_gain_epoch;
    int64_t now = esp_timer_get_time();
    bool settled = now - s_last_gain_write_us >= 500 && s_gain.state != DG_SETTLE;
    if (!settled || !copy_snapshot(raw) || epoch != s_gain_epoch) {
        free(raw);
        printf("IQSNAP_REFUSED stale_or_settling\n");
        return;
    }
    uint32_t hash = 2166136261u;
    for (unsigned k = 0; k < C5V4_LEVEL_SAMPLE_BYTES; ++k) hash = (hash ^ raw[k]) * 16777619u;
    printf("IQSNAP_BEGIN bytes=%u rate_hz=%u format=Ihigh_Qlow gain=%u lane=2 freq_mhz=%u fnv1a=%08lx\n",
           C5V4_LEVEL_SAMPLE_BYTES, IQ_RATE_HZ, s_current_gain, rf_frequency_mhz(), (unsigned long)hash);
    static const char hex[] = "0123456789abcdef";
    char line[513];
    for (unsigned offset = 0; offset < C5V4_LEVEL_SAMPLE_BYTES; offset += 256) {
        unsigned size = C5V4_LEVEL_SAMPLE_BYTES - offset > 256 ? 256 : C5V4_LEVEL_SAMPLE_BYTES - offset;
        for (unsigned k = 0; k < size; ++k) {
            line[2 * k] = hex[raw[offset + k] >> 4];
            line[2 * k + 1] = hex[raw[offset + k] & 15];
        }
        line[2 * size] = 0;
        printf("IQSNAP_DATA offset=%u %s\n", offset, line);
        vTaskDelay(1);
    }
    free(raw);
    printf("IQSNAP_END fnv1a=%08lx\n", (unsigned long)hash);
}

static void help(void)
{
    printf("C5VRX-5 keys: T status, D range/health report, M gain map (for a preset), R per-second FIELD_DIAG lines on/off,\n"
           "  K kp ladder OFF -> AUTO kp -> AUTO kp+LP (brief restarts), k/j kp calmer/faster, P U85Q/U85Q-LP\n"
           "    (k/j/P save AUTO OFF; fresh settings default to AUTO kp+LP),\n"
           "  g OUTPUT 1.0/1.5/2.0/2.5x (safe 1.0x every boot; high levels may clip wide VTX),\n"
           "  A live VTX profile switch on/off (continuous matching every 30 s; off = report only),\n"
           "  I IQ snapshot, o menu open/close, n/x menu next/select, c next channel, b next band, s scan,\n"
           "  v video standard AUTO/NTSC/PAL, e close menu (web menu direct controls),\n"
           "  F calibrate (startup steps again; video stops), L bench IQ lane comparison (video stops ~3 s),\n"
           "  ` reboot to USB download (flashing)\n");
}

/* The USB reader runs above every receiver task: the download key and the
 * heartbeat work however busy the CPU is (a starved console used to make
 * flashing need the BOOT button, 2026-10-11). It drains the FIFO, handles
 * the download key itself and queues the rest for the console task. */
static QueueHandle_t s_keys;

static void usb_reader_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (unsigned k = 0; k < 64; ++k) {
            uint8_t c;
            if (usb_serial_jtag_ll_read_rxfifo(&c, 1) == 0) break;
            if (c == '`') rf_reboot_to_download();   /* never returns */
            (void)xQueueSend(s_keys, &c, 0);
        }
        /* Heartbeat: shows the board is alive whatever the console does. */
        static int64_t beat;
        if (esp_timer_get_time() - beat > 5000000) {
            beat = esp_timer_get_time();
            printf("HB t_s=%lld stage=%s gain=%u menu=%u startup=%u busy=%u\n", beat / 1000000, s_boot_stage,
                   s_current_gain, s_menu_active, calibration_in_startup(), menu_status_active());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void console_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t c;
        while (xQueueReceive(s_keys, &c, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (c) {
            case 'T': status(); break;
            case 'I': snapshot(); break;
            case 'F': filter_request(); break;
            case 'D': diag_report(); break;
            case 'M': receiver_print_map(); break;
            case 'R': diag_toggle_stream(); break;
            case 'L': lane_compare_request(); break;
            case 'h': case '?': help(); break;
            case 'o': case 'e': case 'v': case 'n': case 'x': case 'c': case 'b': case 's': case 'K': case 'k': case 'j': case 'P': case 'g': case 'A': {
                int command = c;
                if (s_commands) (void)xQueueSend(s_commands, &command, 0);
                break;
            }
            default: break;
            }
        }
        diag_poll();
        /* Status beat for the web menu: it updates without a T request. */
        static int64_t status_beat;
        if (esp_timer_get_time() - status_beat >= 2000000) {
            status_beat = esp_timer_get_time();
            control_print_beat();
        }
    }
}

void console_start(void)
{
    s_keys = xQueueCreate(64, 1);
    ESP_ERROR_CHECK(s_keys ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(console_task, "console", 6144, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(usb_reader_task, "usb_reader", 3072, NULL, 10, NULL) == pdPASS ? ESP_OK :
                    ESP_ERR_NO_MEM);
}

