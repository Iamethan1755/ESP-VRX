/* SPDX-License-Identifier: GPL-3.0-only */
/* ESP32-S3 camera-port capture of the C5VRX video bus, see capture.h.
 *
 * The LCD_CAM camera receiver samples eight data inputs on every edge of
 * CAM_PCLK. Nothing external provides that clock: the module's own CAM_CLK
 * output (PLL 160 MHz / N) is routed out to an unconnected pad and back in
 * as PCLK. DE and HSYNC are tied high through the GPIO matrix, so every
 * clock is a sample; data bits 6..7 are tied low. VSYNC is tied low while
 * the receiver is set up and switched high to start it (recording begins
 * on that edge, as in ok-home/logic_analyzer). The receiver ends a DMA
 * block (in_suc_eof) every CAPTURE_NODE_BYTES, matching one descriptor of a
 * circular list, so the EOF interrupt tells which node is complete. */
#include "capture.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gdma.h"
#include "esp_private/periph_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/cam_ll.h"
#include "hal/dma_types.h"
#include "soc/gdma_struct.h"
#include "soc/gpio_sig_map.h"

#define NODES (CAPTURE_RING_BYTES / CAPTURE_NODE_BYTES)

static const char *TAG = "capture";
static const int s_data_gpio[6] = {
    CONFIG_BRIDGE_GPIO_D0, CONFIG_BRIDGE_GPIO_D1, CONFIG_BRIDGE_GPIO_D2,
    CONFIG_BRIDGE_GPIO_D3, CONFIG_BRIDGE_GPIO_D4, CONFIG_BRIDGE_GPIO_D5,
};

static uint8_t *s_ring;
static dma_descriptor_t *s_desc;
static gdma_channel_handle_t s_dma;
static lcd_cam_dev_t *s_hw;
static uint32_t s_fs_hz;
static volatile uint32_t s_nodes;
static volatile uint32_t s_generation;
static uint32_t s_last_idx;
static bool s_running;

static TaskHandle_t s_notify;

static bool IRAM_ATTR on_eof(gdma_channel_handle_t chan, gdma_event_data_t *ev, void *ctx)
{
    (void)chan;
    (void)ctx;
    dma_descriptor_t *d = (dma_descriptor_t *)ev->rx_eof_desc_addr;
    uint32_t idx = (uint32_t)(d - s_desc);
    /* Only a node the DMA has just closed counts: start_engines() clears
     * suc_eof, so a late interrupt from a stopped run is ignored. */
    if (idx >= NODES || !d->dw0.suc_eof) return false;
    d->dw0.suc_eof = 0;
    uint32_t adv = (idx - s_last_idx) & (NODES - 1);
    s_nodes += adv ? adv : NODES;
    s_last_idx = idx;
    BaseType_t woken = pdFALSE;
    if (s_notify) vTaskNotifyGiveFromISR(s_notify, &woken);
    return woken == pdTRUE;
}

void capture_set_notify(TaskHandle_t task) { s_notify = task; }

#ifdef CONFIG_BRIDGE_KEYS_ON_D0
#define FIRST_BIT 1   /* D0 carries the receiver keys (c5link); video bit 0 reads 0 */
#else
#define FIRST_BIT 0
#endif

static void data_pins_input(void)
{
    if (FIRST_BIT) esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_DATA_IN0_IDX, false);
    for (int k = FIRST_BIT; k < 6; k++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_data_gpio[k],
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* unconnected reads 0, negligible load on the C5 */
        };
        gpio_config(&io);
        esp_rom_gpio_connect_in_signal(s_data_gpio[k], CAM_DATA_IN0_IDX + k, false);
    }
}

static void route_signals(void)
{
    data_pins_input();
    for (int k = 6; k < 8; k++)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_DATA_IN0_IDX + k, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_ENABLE_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);   /* stopped */

    gpio_config_t clk = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_GPIO_PCLK_LOOP,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&clk);
    esp_rom_gpio_connect_out_signal(CONFIG_BRIDGE_GPIO_PCLK_LOOP, CAM_CLK_IDX, false, false);
    esp_rom_gpio_connect_in_signal(CONFIG_BRIDGE_GPIO_PCLK_LOOP, CAM_PCLK_IDX, false);
}

static void enable_cam(void)
{
    PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LCD_CAM_MODULE, ref_count) {
        if (ref_count == 0) {
            cam_ll_enable_bus_clock(0, true);
            cam_ll_reset_register(0);
        }
    }
    ESP_ERROR_CHECK(esp_clk_tree_enable_src(SOC_MOD_CLK_PLL_F160M, true));
    s_hw = CAM_LL_GET_HW(0);
}

/* Full LCD_CAM reset and configuration. Done before every start: after a
 * stop the receiver did not record again with only cam_reset/afifo_reset
 * (board test 2026-10-17); a fresh peripheral works every time. */
static void configure_cam(void)
{
    PERIPH_RCC_ATOMIC() {
        (cam_ll_reset_register)(0);   /* the function, not the macro that wants a ref-count scope */
        cam_ll_enable_clk(0, true);
        cam_ll_select_clk_src(0, CAM_CLK_SRC_PLL160M);
        cam_ll_set_group_clock_coeff(0, (int)(160000000u / s_fs_hz), 0, 0);
    }
    cam_ll_enable_stop_signal(s_hw, false);
    cam_ll_swap_dma_data_byte_order(s_hw, false);
    cam_ll_reverse_dma_data_bit_order(s_hw, false);
    cam_ll_enable_vsync_generate_eof(s_hw, false);
    cam_ll_set_recv_data_bytelen(s_hw, CAPTURE_NODE_BYTES - 1);
    cam_ll_enable_hs_line_int(s_hw, false);
    cam_ll_enable_vsync_filter(s_hw, false);
    cam_ll_enable_invert_pclk(s_hw, false);
    cam_ll_set_input_data_width(s_hw, 8);
    cam_ll_enable_invert_de(s_hw, false);
    cam_ll_enable_invert_hsync(s_hw, false);
    cam_ll_enable_invert_vsync(s_hw, false);
    cam_ll_set_vh_de_mode(s_hw, false);
    cam_ll_enable_rgb_yuv_convert(s_hw, false);
}

esp_err_t capture_init(uint32_t fs_hz)
{
    s_fs_hz = fs_hz;
    ESP_RETURN_ON_FALSE(160000000u % fs_hz == 0, ESP_ERR_INVALID_ARG, TAG, "rate must divide 160 MHz");

    gdma_channel_alloc_config_t alloc = {0};
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&alloc, NULL, &s_dma), TAG, "no DMA channel");
    ESP_RETURN_ON_ERROR(gdma_connect(s_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0)), TAG, "connect");
    gdma_strategy_config_t strategy = {.owner_check = false, .auto_update_desc = false};
    ESP_RETURN_ON_ERROR(gdma_apply_strategy(s_dma, &strategy), TAG, "strategy");
    gdma_transfer_config_t transfer = {.max_data_burst_size = 32, .access_ext_mem = false};
    ESP_RETURN_ON_ERROR(gdma_config_transfer(s_dma, &transfer), TAG, "transfer");
    size_t int_align = 4, ext_align = 4;
    gdma_get_alignment_constraints(s_dma, &int_align, &ext_align);

    s_ring = heap_caps_aligned_calloc(int_align > 64 ? int_align : 64, 1, CAPTURE_RING_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_desc = heap_caps_aligned_calloc(8, NODES, sizeof(dma_descriptor_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_ring && s_desc, ESP_ERR_NO_MEM, TAG, "no internal DMA memory for the ring");

    gdma_rx_event_callbacks_t cbs = {.on_recv_eof = on_eof};
    ESP_RETURN_ON_ERROR(gdma_register_rx_event_callbacks(s_dma, &cbs, NULL), TAG, "callbacks");

    route_signals();
    enable_cam();
    configure_cam();
    ESP_LOGI(TAG, "%lu MS/s on GPIO %d,%d,%d,%d,%d,%d (bit 0..5), clock loop GPIO %d",
             (unsigned long)(fs_hz / 1000000u), s_data_gpio[0], s_data_gpio[1], s_data_gpio[2],
             s_data_gpio[3], s_data_gpio[4], s_data_gpio[5], CONFIG_BRIDGE_GPIO_PCLK_LOOP);
    return ESP_OK;
}

uint8_t *capture_ring(void) { return s_ring; }
bool capture_running(void) { return s_running; }
uint32_t capture_write_pos(void) { return s_nodes * CAPTURE_NODE_BYTES; }
uint32_t capture_generation(void) { return s_generation; }

static void start_engines(void)
{
    for (uint32_t k = 0; k < NODES; k++) {
        s_desc[k].dw0.size = CAPTURE_NODE_BYTES;
        s_desc[k].dw0.length = 0;
        s_desc[k].dw0.err_eof = 0;
        s_desc[k].dw0.suc_eof = 0;
        s_desc[k].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        s_desc[k].buffer = s_ring + k * CAPTURE_NODE_BYTES;
        s_desc[k].next = &s_desc[(k + 1) % NODES];
    }
    s_last_idx = NODES - 1;
    s_nodes = 0;
    s_generation++;
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
    configure_cam();
    cam_ll_reset(s_hw);
    cam_ll_fifo_reset(s_hw);
    gdma_reset(s_dma);
    gdma_start(s_dma, (intptr_t)s_desc);
    cam_ll_start(s_hw);
    /* The receiver applies cam_update/cam_start in its own (PCLK) clock
     * domain; a VSYNC edge right after them was missed on every restart
     * (board test 2026-10-17), so give it a few clock cycles first. */
    esp_rom_delay_us(20);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);   /* go */
}

/* Second VSYNC edge, in case the receiver still missed the first. */
static void pulse_vsync(void)
{
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
    esp_rom_delay_us(20);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);
}

static void stop_engines(void)
{
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
    cam_ll_stop(s_hw);
    gdma_stop(s_dma);
}

esp_err_t capture_start(void)
{
    if (s_running) return ESP_OK;
    start_engines();
    vTaskDelay(pdMS_TO_TICKS(3));
    if (s_nodes == 0) {
        pulse_vsync();
        vTaskDelay(pdMS_TO_TICKS(3));
        if (s_nodes == 0) {
            stop_engines();
            ESP_LOGE(TAG, "camera port records nothing");
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "camera port needed a second VSYNC edge to start");
    }
    s_running = true;
    return ESP_OK;
}

void capture_stop(void)
{
    if (!s_running) return;
    stop_engines();
    s_running = false;
}

void capture_debug(char *out, size_t len)
{
    const bool was_running = s_running;
    capture_stop();
    int ch = -1;
    gdma_get_group_channel_id(s_dma, NULL, &ch);

    int highs = 0;
    for (int i = 0; i < 20000; i++) highs += gpio_get_level(CONFIG_BRIDGE_GPIO_PCLK_LOOP);

    memset(s_ring, 0xaa, CAPTURE_RING_BYTES);
    GDMA.channel[ch].in.int_clr.val = UINT32_MAX;
    start_engines();
    vTaskDelay(pdMS_TO_TICKS(5));
    const uint32_t nodes = s_nodes;
    const uint32_t raw = GDMA.channel[ch].in.int_raw.val;
    const uint32_t link = GDMA.channel[ch].in.link.val, state = GDMA.channel[ch].in.state.val;
    const uint32_t dscr = GDMA.channel[ch].in.dscr, suc = GDMA.channel[ch].in.suc_eof_des_addr;
    const uint32_t sel = GDMA.channel[ch].in.peri_sel.val, conf0 = GDMA.channel[ch].in.conf0.val;
    const uint32_t ctrl = s_hw->cam_ctrl.val, ctrl1 = s_hw->cam_ctrl1.val;
    stop_engines();

    uint32_t written = 0;
    for (uint32_t i = 0; i < CAPTURE_RING_BYTES; i++) written += s_ring[i] != 0xaa;
    int n = snprintf(out, len,
                     "DEBUG pclk_pad_high=%d/20000 written=%lu/%u nodes=%lu gdma_ch=%d int_raw=%03lx "
                     "link=%08lx state=%08lx dscr=%08lx suc_eof=%08lx desc0=%08lx peri_sel=%lu conf0=%02lx "
                     "cam_ctrl=%08lx cam_ctrl1=%08lx ring=%p desc=%p",
                     highs, (unsigned long)written, CAPTURE_RING_BYTES, (unsigned long)nodes, ch,
                     (unsigned long)raw, (unsigned long)link, (unsigned long)state, (unsigned long)dscr,
                     (unsigned long)suc, (unsigned long)(uintptr_t)s_desc, (unsigned long)sel,
                     (unsigned long)conf0, (unsigned long)ctrl, (unsigned long)ctrl1, s_ring, s_desc);
    for (int k = 0; k < 3 && n > 0 && (size_t)n < len; k++) {
        n += snprintf(out + n, len - (size_t)n, " d%d:len=%u,eof=%u,own=%u", k, (unsigned)s_desc[k].dw0.length,
                      (unsigned)s_desc[k].dw0.suc_eof, (unsigned)s_desc[k].dw0.owner);
    }

    /* Restart variants: which one brings the data back after a stop? */
    static const char *names[] = {"plain", "clear-stop", "reconnect", "vsync-pulse"};
    for (int variant = 0; variant < 4 && n > 0 && (size_t)n < len; variant++) {
        if (variant == 2) {
            gdma_disconnect(s_dma);
            gdma_connect(s_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0));
        }
        if (variant == 1) GDMA.channel[ch].in.link.stop = 0;
        start_engines();
        if (variant == 3) {
            vTaskDelay(1);
            pulse_vsync();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        const uint32_t got = s_nodes, raw2 = GDMA.channel[ch].in.int_raw.val;
        stop_engines();
        n += snprintf(out + n, len - (size_t)n, " | %s: nodes=%lu int_raw=%03lx", names[variant],
                      (unsigned long)got, (unsigned long)raw2);
    }
    if (was_running) capture_start();
}

esp_err_t capture_selftest(char *out, size_t len)
{
    static const uint8_t patterns[] = {0x2a, 0x15, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3f, 0x00};
    /* Refuse when something drives the pins: with their pull-downs they read
     * 0 unless a (powered) C5 is attached, whose video toggles them. */
    for (int i = 0; i < 2000; i++) {
        for (int k = FIRST_BIT; k < 6; k++) {
            if (gpio_get_level(s_data_gpio[k])) {
                snprintf(out, len, "SELFTEST refused: D%d is driven from outside (C5 attached?). "
                                   "Disconnect D0..D5 first.", k);
                return ESP_ERR_INVALID_STATE;
            }
        }
        esp_rom_delay_us(1);
    }
    /* An unpowered C5 drives nothing but clamps its pins to its dead rail
     * through the protection diodes: with the S3's pull-up a free pin reads
     * high, a clamped one stays low. Driving them would feed that C5. */
    for (int k = FIRST_BIT; k < 6; k++) {
        gpio_set_pull_mode(s_data_gpio[k], GPIO_PULLUP_ONLY);
        esp_rom_delay_us(50);
        const bool high = gpio_get_level(s_data_gpio[k]);
        gpio_set_pull_mode(s_data_gpio[k], GPIO_PULLDOWN_ONLY);
        if (!high) {
            snprintf(out, len, "SELFTEST refused: D%d is held low (C5 attached, maybe unpowered?). "
                               "Disconnect D0..D5 first.", k);
            return ESP_ERR_INVALID_STATE;
        }
    }
    const bool was_running = s_running;
    capture_stop();
    for (int k = FIRST_BIT; k < 6; k++) {
        gpio_set_pull_mode(s_data_gpio[k], GPIO_FLOATING);
        gpio_set_direction(s_data_gpio[k], GPIO_MODE_INPUT_OUTPUT);
    }
    const uint8_t mask = (uint8_t)(0x3f & ~((1u << FIRST_BIT) - 1u));   /* bits under test */

    int fails = 0;
    size_t n = (size_t)snprintf(out, len, "SELFTEST");
    for (size_t p = 0; p < sizeof patterns; p++) {
        const uint8_t want = patterns[p] & mask;
        if (p && want == (patterns[p - 1] & mask)) continue;   /* bit-0-only pattern with keys on D0 */
        for (int k = FIRST_BIT; k < 6; k++) gpio_set_level(s_data_gpio[k], (want >> k) & 1);
        if (capture_start() != ESP_OK) {
            fails++;
            n += (size_t)snprintf(out + n, len > n ? len - n : 0, " %02x:no-data", patterns[p]);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        const uint32_t wp = capture_write_pos();
        capture_stop();
        const uint8_t *node = s_ring + ((wp - CAPTURE_NODE_BYTES) & (CAPTURE_RING_BYTES - 1));
        int wrong = 0;
        uint8_t seen = want;
        for (uint32_t i = 0; i < CAPTURE_NODE_BYTES; i++) {
            if (node[i] != want) {
                wrong++;
                seen = node[i];
            }
        }
        if (wrong) fails++;
        n += (size_t)snprintf(out + n, len > n ? len - n : 0, wrong ? " %02x->%02x(x%d)" : " %02x:ok",
                              want, seen, wrong);
    }

    float msps = 0;
    if (capture_start() == ESP_OK) {
        const int64_t t0 = esp_timer_get_time();
        const uint32_t w0 = capture_write_pos();
        vTaskDelay(pdMS_TO_TICKS(200));
        const uint32_t w1 = capture_write_pos();
        const int64_t t1 = esp_timer_get_time();
        capture_stop();
        msps = (float)(w1 - w0) / (float)(t1 - t0);
    }
    const float want = s_fs_hz / 1e6f;
    if (fabsf(msps - want) > 0.03f * want) fails++;
    n += (size_t)snprintf(out + n, len > n ? len - n : 0, " rate=%.2fMS/s(want %.0f) %s", msps, want,
                          fails ? "FAIL" : "PASS");

    for (int k = FIRST_BIT; k < 6; k++) gpio_set_level(s_data_gpio[k], 0);
    data_pins_input();
    if (was_running) capture_start();
    return fails ? ESP_FAIL : ESP_OK;
}
