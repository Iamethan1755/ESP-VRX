/* SPDX-License-Identifier: GPL-3.0-only */
/* SPI TFT screen, see display.h.
 *
 * The display task takes the newest finished frame from the frame mailbox
 * (stream.c, mbox.h) as the JPEG encoder does, so neither the decoder nor
 * the encoder ever waits for the screen, and the decoder does not render
 * into the frame while it is read. Gray goes to RGB565 through a table,
 * colour (luma and chroma planes) through JFIF YCbCr-to-RGB tables,
 * STRIP_LINES lines at a time into two internal-RAM DMA buffers: one strip
 * is converted while the other goes out. The screen is one 320x240 window
 * per frame; the first strip starts it (RAMWR), the others continue it
 * (RAMWRC). esp_lcd sends a command only once the strip before it is out,
 * so the buffer converted next is never one still on the wire. The frame
 * goes back to the mailbox as soon as its last strip is converted.
 *
 * NTSC fields are 320x240, the screen's size; PAL fields (288 lines) leave
 * out every 6th line. Without line and field lock a NO SIGNAL badge (like
 * the phone page's) covers the top left of the picture; with no frames for
 * a second (capture stopped) the screen goes black with the badge.
 *
 * At 40 MHz a frame takes 31 ms on the wire, just under the 33 ms of a
 * 30 fps stream: the task mostly waits for the SPI DMA; converting should
 * cost core 0 about 2 ms per frame (estimated, not measured yet). */
#include "display.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "bridge.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stream.h"
#include "video.h"
#ifdef CONFIG_BRIDGE_DISPLAY_ILI9341
#include "esp_lcd_ili9341.h"
#endif

/* Pins the bridge uses for something else: the video bits (D0..D5, the
 * keys on D0), the separate key wire if used, the clock loop pad (GPIO42)
 * and the boot log (GPIO43, D6). */
#ifdef CONFIG_BRIDGE_KEYS_ON_D0
#define KEY_PIN(p) 0
#else
#define KEY_PIN(p) ((p) == CONFIG_BRIDGE_GPIO_C5_TX)
#endif
#define PIN_TAKEN(p) ((p) == CONFIG_BRIDGE_GPIO_D0 || (p) == CONFIG_BRIDGE_GPIO_D1 || (p) == CONFIG_BRIDGE_GPIO_D2 || \
                      (p) == CONFIG_BRIDGE_GPIO_D3 || (p) == CONFIG_BRIDGE_GPIO_D4 || (p) == CONFIG_BRIDGE_GPIO_D5 || \
                      (p) == CONFIG_BRIDGE_GPIO_PCLK_LOOP || (p) == 43 || KEY_PIN(p))
#if PIN_TAKEN(CONFIG_BRIDGE_DISPLAY_GPIO_SCK) || PIN_TAKEN(CONFIG_BRIDGE_DISPLAY_GPIO_MOSI) || \
    PIN_TAKEN(CONFIG_BRIDGE_DISPLAY_GPIO_CS) || PIN_TAKEN(CONFIG_BRIDGE_DISPLAY_GPIO_DC)
#error "BRIDGE_DISPLAY_GPIO_*: a display pin is a video pin, the key wire, GPIO42 (clock loop) or GPIO43 (boot log)"
#endif
#if CONFIG_BRIDGE_DISPLAY_GPIO_SCK == CONFIG_BRIDGE_DISPLAY_GPIO_MOSI || \
    CONFIG_BRIDGE_DISPLAY_GPIO_SCK == CONFIG_BRIDGE_DISPLAY_GPIO_CS || \
    CONFIG_BRIDGE_DISPLAY_GPIO_SCK == CONFIG_BRIDGE_DISPLAY_GPIO_DC || \
    CONFIG_BRIDGE_DISPLAY_GPIO_MOSI == CONFIG_BRIDGE_DISPLAY_GPIO_CS || \
    CONFIG_BRIDGE_DISPLAY_GPIO_MOSI == CONFIG_BRIDGE_DISPLAY_GPIO_DC || \
    CONFIG_BRIDGE_DISPLAY_GPIO_CS == CONFIG_BRIDGE_DISPLAY_GPIO_DC
#error "BRIDGE_DISPLAY_GPIO_*: two display signals on one pin"
#endif

#define LCD_W 320
#define LCD_H 240
#define LCD_HOST SPI2_HOST
#define STRIP_LINES 12                         /* 20 strips per frame */
#define STRIP_BYTES (LCD_W * STRIP_LINES * 2)  /* 7.5 KiB, two of them */
_Static_assert(VIDEO_W == LCD_W, "frames are as wide as the screen");
_Static_assert(LCD_H % STRIP_LINES == 0, "whole strips per frame");

/* RGB565, byte-swapped: the panel takes the high byte first. */
#define RGB565(r, g, b) ((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))
#define PANEL565(r, g, b) ((uint16_t)((RGB565(r, g, b) >> 8) | ((RGB565(r, g, b) & 0xff) << 8)))

/* The NO SIGNAL badge, top left: the phone page's red (#f85149) on its
 * panel colour (#161a20), 5x7 capitals drawn twice the size. */
#define BADGE_TEXT  "NO SIGNAL"
#define BADGE_SCALE 2
#define BADGE_PAD   6
#define BADGE_X     8
#define BADGE_Y     8
#define BADGE_CHARS ((int)sizeof BADGE_TEXT - 1)
#define BADGE_W     (2 * BADGE_PAD + BADGE_CHARS * 6 * BADGE_SCALE - BADGE_SCALE)
#define BADGE_H     (2 * BADGE_PAD + 7 * BADGE_SCALE)

static const char *TAG = "display";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_strip[2];
static uint16_t s_gray[256];   /* gray level -> panel RGB565 */
static int16_t s_r_cr[256], s_g_cb[256], s_g_cr[256], s_b_cb[256];   /* YCbCr -> RGB, added to Y */
static uint8_t s_clip[768];    /* value + 256 -> 0..255 */
static float s_fps;

float display_fps(void) { return s_fps; }

/* One byte per row, bit 4 the left column. Only the badge's letters. */
static const uint8_t *glyph(char c)
{
    static const uint8_t n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t s[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t i[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t g[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f};
    static const uint8_t a[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t blank[7] = {0};
    switch (c) {
    case 'N': return n;
    case 'O': return o;
    case 'S': return s;
    case 'I': return i;
    case 'G': return g;
    case 'A': return a;
    case 'L': return l;
    default: return blank;
    }
}

/* Paints the badge's rows that fall into the strip starting at screen row y0. */
static void draw_badge(uint16_t *strip, int y0)
{
    static const char text[] = BADGE_TEXT;
    const uint16_t fg = PANEL565(0xf8, 0x51, 0x49), bg = PANEL565(0x16, 0x1a, 0x20);
    for (int r = 0; r < STRIP_LINES; r++) {
        const int by = y0 + r - BADGE_Y;
        if (by < 0 || by >= BADGE_H) continue;
        const int ty = by - BADGE_PAD;
        const int gy = ty >= 0 && ty < 7 * BADGE_SCALE ? ty / BADGE_SCALE : -1;
        uint16_t *px = strip + r * LCD_W + BADGE_X;
        for (int bx = 0; bx < BADGE_W; bx++) {
            const int tx = bx - BADGE_PAD;
            bool on = false;
            if (gy >= 0 && tx >= 0) {
                const int ch = tx / (6 * BADGE_SCALE), gx = tx % (6 * BADGE_SCALE) / BADGE_SCALE;
                on = ch < BADGE_CHARS && gx < 5 && (glyph(text[ch])[gy] >> (4 - gx) & 1);
            }
            px[bx] = on ? fg : bg;
        }
    }
}

/* Screen rows y0 .. y0 + STRIP_LINES - 1 from a frame of h rows (PSRAM).
 * PAL: row y shows frame row y * 288 / 240, which leaves out every 6th. */
static void convert(uint16_t *strip, const uint8_t *fb, int h, video_fmt_t fmt, int y0)
{
    if (fmt == VIDEO_FMT_COLOUR) {
        for (int r = 0; r < STRIP_LINES; r++) {
            const int fy = (y0 + r) * h / LCD_H;
            const uint16_t *src = (const uint16_t *)(fb + (size_t)fy * LCD_W);
            const uint8_t *c = fb + VIDEO_CHROMA_OFF + (size_t)(fy / 2) * VIDEO_CHROMA_ROW;
            uint32_t *dst = (uint32_t *)(strip + r * LCD_W);
            for (int x = 0; x < LCD_W / 2; x++) {
                const uint32_t p = src[x];   /* two luma pixels */
                const int k = (2 * x + 1) / 5;   /* chroma sample k covers pixels 5k .. 5k + 4 */
                const int ya = (int)(p & 0xff) + 256, yb = (int)(p >> 8) + 256, cb = c[k], cr = c[VIDEO_CW + k];
                const int dr = s_r_cr[cr], dg = s_g_cb[cb] + s_g_cr[cr], db = s_b_cb[cb];
                dst[x] = PANEL565(s_clip[ya + dr], s_clip[ya + dg], s_clip[ya + db]) |
                         (uint32_t)PANEL565(s_clip[yb + dr], s_clip[yb + dg], s_clip[yb + db]) << 16;
            }
        }
        return;
    }
    for (int r = 0; r < STRIP_LINES; r++) {
        const uint32_t *src = (const uint32_t *)(fb + (size_t)((y0 + r) * h / LCD_H) * LCD_W);
        uint32_t *dst = (uint32_t *)(strip + r * LCD_W);
        for (int x = 0; x < LCD_W / 4; x++) {
            const uint32_t p = src[x];   /* four pixels per read */
            dst[2 * x] = s_gray[p & 0xff] | (uint32_t)s_gray[p >> 8 & 0xff] << 16;
            dst[2 * x + 1] = s_gray[p >> 16 & 0xff] | (uint32_t)s_gray[p >> 24] << 16;
        }
    }
}

/* Sends a frame of h rows (NULL: black) to the whole screen. */
static void draw(const uint8_t *fb, int h, video_fmt_t fmt, bool badge)
{
    static bool failed;
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, (uint8_t[]){0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xff}, 4);
    if (err == ESP_OK)
        err = esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, (uint8_t[]){0, 0, (LCD_H - 1) >> 8, (LCD_H - 1) & 0xff}, 4);
    for (int k = 0; k < LCD_H / STRIP_LINES && err == ESP_OK; k++) {
        uint16_t *strip = s_strip[k & 1];
        if (fb) convert(strip, fb, h, fmt, k * STRIP_LINES);
        else memset(strip, 0, STRIP_BYTES);
        if (badge) draw_badge(strip, k * STRIP_LINES);
        err = esp_lcd_panel_io_tx_color(s_io, k ? LCD_CMD_RAMWRC : LCD_CMD_RAMWR, strip, STRIP_BYTES);
    }
    if (err != ESP_OK && !failed) ESP_LOGE(TAG, "SPI send failed: %s", esp_err_to_name(err));
    failed = err != ESP_OK;
}

static bool panel_setup(void)
{
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = GPIO_NUM_NC,   /* RESET wired to 3V3: software reset */
#ifdef CONFIG_BRIDGE_DISPLAY_ILI9341
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    /* Landscape: rows and columns exchanged, then the mirroring that shows
     * the picture the right way round on that controller's usual modules
     * (the red ILI9341 boards: none; ST7789 IPS modules: rows). */
#ifdef CONFIG_BRIDGE_DISPLAY_ILI9341
    esp_err_t err = esp_lcd_new_panel_ili9341(s_io, &dev, &s_panel);
    bool mirror_x = false, mirror_y = false;
#else
    esp_err_t err = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    bool mirror_x = false, mirror_y = true;
#endif
#ifdef CONFIG_BRIDGE_DISPLAY_ROTATE_180
    mirror_x = !mirror_x;
    mirror_y = !mirror_y;
#endif
#ifdef CONFIG_BRIDGE_DISPLAY_MIRROR
    mirror_x = !mirror_x;
#endif
#ifdef CONFIG_BRIDGE_DISPLAY_INVERT
    const bool invert = true;
#else
    const bool invert = false;
#endif
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    /* RESET is tied high, so after a restart of the S3 the panel is still
     * awake: a software reset then needs 120 ms before the sleep-out that
     * init sends (both datasheets); the drivers wait 20 ms. */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(s_panel, invert);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(s_panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    if (err == ESP_OK) draw(NULL, LCD_H, VIDEO_FMT_GRAY, false);   /* black, not the panel's power-up noise */
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) ESP_LOGE(TAG, "panel setup failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void display_task(void *arg)
{
    (void)arg;
    if (!panel_setup()) {
        vTaskDelete(NULL);
        return;
    }
    stream_display_attach();   /* every new frame notifies this task from here on */
    int64_t last_frame = esp_timer_get_time(), window = last_frame;
    uint32_t count = 0;
    bool blank = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        int w = 0, h = 0;
        video_fmt_t fmt = VIDEO_FMT_GRAY;
        const uint8_t *fb = stream_display_take(&w, &h, &fmt);
        const int64_t now = esp_timer_get_time();
        if (fb) {
            if (w == LCD_W && h > 0 && h <= VIDEO_H_MAX) {
                draw(fb, h, fmt, !bridge_locked());
                count++;
            }
            stream_display_release(fb);
            last_frame = now;
            blank = false;
        } else if (!blank && now - last_frame > 1000000) {
            draw(NULL, LCD_H, VIDEO_FMT_GRAY, true);   /* no frames: capture stopped */
            blank = true;
        }
        if (now - window >= 1000000) {
            s_fps = count * 1e6f / (float)(now - window);
            count = 0;
            window = now;
        }
    }
}

esp_err_t display_start(void)
{
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_strip[i]) return ESP_ERR_NO_MEM;
    }
    for (int g = 0; g < 256; g++) {
        s_gray[g] = PANEL565(g, g, g);
        s_r_cr[g] = (int16_t)lroundf(1.402f * (g - 128));
        s_g_cb[g] = (int16_t)lroundf(-0.344136f * (g - 128));
        s_g_cr[g] = (int16_t)lroundf(-0.714136f * (g - 128));
        s_b_cb[g] = (int16_t)lroundf(1.772f * (g - 128));
    }
    for (int k = 0; k < 768; k++) s_clip[k] = (uint8_t)(k < 256 ? 0 : k > 511 ? 255 : k - 256);
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_BRIDGE_DISPLAY_GPIO_SCK,
        .mosi_io_num = CONFIG_BRIDGE_DISPLAY_GPIO_MOSI,
        .miso_io_num = -1,   /* nothing is read back: MISO stays unconnected */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_BYTES,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_0,   /* core 1 belongs to the decoder */
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = CONFIG_BRIDGE_DISPLAY_GPIO_CS,
        .dc_gpio_num = CONFIG_BRIDGE_DISPLAY_GPIO_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_BRIDGE_DISPLAY_SPI_MHZ * 1000000,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_io);
    if (err != ESP_OK) return err;
    /* Above the JPEG encoder (2), which runs nonstop on a noisy picture,
     * below the web server (5) and the viewers (6). */
    if (xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 4, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
#ifdef CONFIG_BRIDGE_DISPLAY_ILI9341
    const char *panel = "ILI9341";
#else
    const char *panel = "ST7789";
#endif
    ESP_LOGI(TAG, "%s on SPI at %d MHz: SCK GPIO%d, MOSI GPIO%d, CS GPIO%d, DC GPIO%d", panel,
             CONFIG_BRIDGE_DISPLAY_SPI_MHZ, CONFIG_BRIDGE_DISPLAY_GPIO_SCK, CONFIG_BRIDGE_DISPLAY_GPIO_MOSI,
             CONFIG_BRIDGE_DISPLAY_GPIO_CS, CONFIG_BRIDGE_DISPLAY_GPIO_DC);
    return ESP_OK;
}
