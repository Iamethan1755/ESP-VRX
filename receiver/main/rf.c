/* C5VRX-5 by Twotoz and the C5VRX contributors: receive-only RF front end.
 * From legacy/c5vrx4/main/rf.c (C5VRX-2 wifi5.c lineage), native-AGC, lab and
 * adaptive-lane paths removed. */
#include "rf.h"
#include "phy.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/gpio_sig_map.h"
#include "soc/lp_aon_reg.h"

#define REG32(a)         (*(volatile uint32_t *)(uintptr_t)(a))
#define MAC_TXQ0_CONF    0x600a4d6cu
#define MAC_TXQ_STRIDE   0x10u
#define MAC_TXQ_ENABLE   0x80000000u
#define MAC_TXQ_COUNT    5u
/* Continuous modem front end: keeps the ADC streaming into MODEM_DIAG
 * without 802.11 packets (C5VRX-2 autonomous dump-first writer). */
#define DUMP_CTRL       0x600a9004u
#define DUMP_PTR_MODE   0x600a9008u
#define DUMP_FORMAT     0x600a9018u
#define FE_PATH         0x600a20b4u
#define FE_ENABLE       0x600a0800u
#define SOURCE_CTRL     0x600a08ccu
#define SOURCE_MUX      0x600a70b8u
#define MODEM_CLOCK     0x600a9c04u
#define CTRL_ENABLE     0x80000000u
#define CTRL_DUMP_FIRST 0x00020000u
#define TX_START_SELECT 0x00060000u
#define SELECTOR_MASK   0x01fe0000u
#define HP_SRAM_USAGE   0x60095004u

static const char *TAG = "c5vrx5_rf";

/* PARLIO RX data pins, in data-bit order: Q on bits 0..3, I on 4..7. */
static const gpio_num_t s_iq_pins[8] = {
    GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_25, GPIO_NUM_7,
    GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_3, GPIO_NUM_4,
};
/* Ultrafine lane: sign bit 9 plus ADC bits 6..4, i.e. 16-code steps in a
 * +-128 window (+12 dB over the coarse {9,8,7,6} slice, an exact
 * power-of-two scale inside the window). Q on DIAG[4,5,6,9], I on
 * DIAG[14,15,16,19] (bit-exact against the RF dump, PR #122). Fixed for
 * the session: Direct Gain V5 brings a folding carrier back into the
 * window with analog gain (operator default since 2026-10-08). */
static const uint8_t s_lane_diag[RF_LANES][8] = {
    {6u, 7u, 8u, 9u, 16u, 17u, 18u, 19u},   /* coarse    {9,8,7,6} */
    {5u, 6u, 7u, 9u, 15u, 16u, 17u, 19u},   /* fine      {9,7,6,5} */
    {4u, 5u, 6u, 9u, 14u, 15u, 16u, 19u},   /* ultrafine {9,6,5,4} */
};
static const char *const s_lane_names[RF_LANES] = {"coarse", "fine", "ultrafine"};
static unsigned s_lane = RF_LANE_ULTRAFINE;

extern int lmac_stop_hw_txq(void);
extern void phy_wifi_fbw_sel(uint32_t val);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);
extern void phy_disable_agc(void);
extern void phy_rfagc_disable(void);
extern void phy_set_freq(uint16_t freq_mhz, int offset);
extern void phy_chip_set_chan_offset(int offset_khz);

static arc_gain_table_t s_table;
static uint32_t s_table_generation;
static uint8_t s_gain = 52u;
static void (*volatile s_post_gain_hook)(uint8_t gain);
static rf_filter_t s_filter = {PHY_FILTER_CALIBRATED, 0, false};

static esp_err_t lock_rx_only(void)
{
    (void)lmac_stop_hw_txq();
    for (unsigned q = 0; q < MAC_TXQ_COUNT; ++q)
        REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~MAC_TXQ_ENABLE;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    for (unsigned q = 0; q < MAC_TXQ_COUNT; ++q)
        if (REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) & MAC_TXQ_ENABLE) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t route_modem_iq(unsigned lane)
{
    uint64_t mask = 0;
    for (unsigned k = 0; k < 8; ++k) mask |= 1ULL << s_iq_pins[k];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    for (unsigned k = 0; k < 8; ++k)
        esp_rom_gpio_connect_out_signal(s_iq_pins[k], MODEM_DIAG0_IDX + s_lane_diag[lane][k], false, false);
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    s_lane = lane;
    return ESP_OK;
}

esp_err_t rf_set_lane(unsigned lane)
{
    if (lane >= RF_LANES) return ESP_ERR_INVALID_ARG;
    return route_modem_iq(lane);
}

unsigned rf_lane(void) { return s_lane; }
const char *rf_lane_name(unsigned lane) { return lane < RF_LANES ? s_lane_names[lane] : "?"; }

static void enable_continuous_modem(void)
{
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & 0xfffef0ffu) | 0x00010000u;
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX) = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;
    REG32(FE_PATH) &= ~1u;
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffc0fffu) | 0x0001a000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffff03fu) | 0x00000640u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xffffffc0u) | 0x18u;
    REG32(DUMP_FORMAT) = v | 0x01000000u;
    /* Pre-trigger circular mode on TX_START: the MAC queues are disabled,
     * so it never fires and the writer streams continuously. */
    REG32(DUMP_PTR_MODE) = (REG32(DUMP_PTR_MODE) & ~SELECTOR_MASK) | TX_START_SELECT;
    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u);
    ctrl |= CTRL_DUMP_FIRST;
    ctrl = (ctrl & ~0x0001ffffu) | 16384u;
    REG32(DUMP_CTRL) = ctrl;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static void apply_filter(void)
{
    if (!phy_filter_set_code(s_filter.code) && s_filter.code != PHY_FILTER_CALIBRATED)
        ESP_EARLY_LOGW(TAG, "filter code %d not applied", s_filter.code);
    /* Written even at 0: leaving a skirt must undo it. */
    if (!phy_filter_set_skirt(s_filter.skirt) && s_filter.skirt)
        ESP_EARLY_LOGW(TAG, "filter skirt %u not applied", s_filter.skirt);
}

/* Every tuning/filter transaction restores the same receive state: vendor
 * AGC off, digital filter, forced gain, analog filter, DC hold. */
static void restore_lock(void)
{
    (void)phy_dco_release();          /* the restore must replay vendor rows */
    enable_continuous_modem();
    ESP_ERROR_CHECK(lock_rx_only());
    phy_disable_agc();
    phy_rfagc_disable();
    phy_wifi_fbw_sel(s_filter.bw20 ? 0u : 1u);
    phy_force_rx_gain(true, s_gain);
    apply_filter();
    if (s_post_gain_hook) s_post_gain_hook(s_gain);
}

static void capture_table(void)
{
    arc_phy_capture_gain_table(&s_table);
    ++s_table_generation;
}

esp_err_t rf_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    /* sta_disconnected_pm must be off: Wi-Fi power management would gate
     * the RF/PHY clocks that keep MODEM_DIAG streaming. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.sta_disconnected_pm = false;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK) return err;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return err;
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK) return err;
    wifi_bandwidths_t bandwidths = {.ghz_2g = WIFI_BW20, .ghz_5g = WIFI_BW40};
    if ((err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths)) != ESP_OK) return err;
    if ((err = esp_wifi_set_channel(173, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return err;
    /* Promiscuous RX keeps the receive path alive; a zero filter keeps the
     * LMAC from buffering packets or raising interrupts. */
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;
    wifi_promiscuous_filter_t filter = {.filter_mask = 0};
    (void)esp_wifi_set_promiscuous_filter(&filter);
    if ((err = lock_rx_only()) != ESP_OK) return err;
    if ((err = route_modem_iq(RF_LANE_ULTRAFINE)) != ESP_OK) return err;
    phy_begin();
    /* The filter capacitors still hold the per-chip calibration from PHY
     * init; capture them before anything else writes the filter. */
    (void)phy_filter_capture_base();
    restore_lock();
    capture_table();
    phy_end();
    return ESP_OK;
}

void rf_reboot_to_download(void)
{
    /* esptool's USB reset does not reset the modem domain; the forced clocks
     * and the running dump writer would keep the ROM loader from answering. */
    printf("C5VRX5 download_boot=requested\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    REG32(HP_SRAM_USAGE) &= ~0x00010f00u;
    (void)esp_wifi_stop();
    REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1u);
    esp_restart();
}

bool rf_set_gain(uint8_t gain, uint32_t expected_generation)
{
    if (!phy_try_actuator(expected_generation)) return false;
    /* Re-writing the gain in force while the DC pair is held changes
     * nothing but would leave debug mode for ~2 us: skip it (V5 used to
     * re-write the same gain 50-100 times a second; seen as rolling). */
    if (gain == s_gain && phy_dco_held()) { phy_end_actuator(); return true; }
    (void)phy_dco_release();          /* a held hold keeps PBUS in debug mode */
    phy_force_rx_gain(true, gain);
    s_gain = gain;
    if (s_post_gain_hook) s_post_gain_hook(gain);
    phy_end_actuator();
    return true;
}

void rf_force_gain(uint8_t gain)
{
    phy_begin();
    (void)phy_dco_release();
    phy_force_rx_gain(true, gain);
    s_gain = gain;
    if (s_post_gain_hook) s_post_gain_hook(gain);
    phy_end();
}

void rf_set_post_gain_hook(void (*hook)(uint8_t gain)) { s_post_gain_hook = hook; }
const arc_gain_table_t *rf_gain_table(void) { return &s_table; }
uint32_t rf_table_generation(void) { return s_table_generation; }

bool rf_set_filter(rf_filter_t filter)
{
    phy_begin();
    s_filter = filter;
    restore_lock();
    bool ok = rf_filter_applied();
    phy_end();
    return ok;
}

rf_filter_t rf_filter(void) { return s_filter; }

/* Requested versus read back (phy.c verifies every register write and
 * falls back to the calibrated bytes). Without the pinned PHY nothing can
 * be written, so only the chip's own calibration counts as applied. */
bool rf_filter_applied(void)
{
    return phy_filter_code() == s_filter.code && phy_filter_skirt() == (int)s_filter.skirt;
}

/* 6 bands x 8 channels. */
static const fpv_channel_t s_channels[FPV_BANDS][8] = {
    [FPV_BAND_R] = {{"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769},
                    {"R5", 5806}, {"R6", 5843}, {"R7", 5880}, {"R8", 5917}},
    [FPV_BAND_A] = {{"A1", 5865}, {"A2", 5845}, {"A3", 5825}, {"A4", 5805},
                    {"A5", 5785}, {"A6", 5765}, {"A7", 5745}, {"A8", 5725}},
    [FPV_BAND_B] = {{"B1", 5733}, {"B2", 5752}, {"B3", 5771}, {"B4", 5790},
                    {"B5", 5809}, {"B6", 5828}, {"B7", 5847}, {"B8", 5866}},
    [FPV_BAND_E] = {{"E1", 5705}, {"E2", 5685}, {"E3", 5665}, {"E4", 5645},
                    {"E5", 5885}, {"E6", 5905}, {"E7", 5925}, {"E8", 5945}},
    [FPV_BAND_F] = {{"F1", 5740}, {"F2", 5760}, {"F3", 5780}, {"F4", 5800},
                    {"F5", 5820}, {"F6", 5840}, {"F7", 5860}, {"F8", 5880}},
    [FPV_BAND_L] = {{"L1", 5362}, {"L2", 5399}, {"L3", 5436}, {"L4", 5473},
                    {"L5", 5510}, {"L6", 5547}, {"L7", 5584}, {"L8", 5621}},
};
static const char *const s_band_names[FPV_BANDS] = {
    "RACEBAND R", "BOSCAM A", "BOSCAM B", "BOSCAM E", "FATSHARK F", "LOWBAND L",
};
static size_t s_index = FPV_BAND_A * 8u;   /* A1, 5865 MHz */
static uint16_t s_freq = 5865u;
static int s_offset_khz;

const fpv_channel_t *rf_channel(void) { return &s_channels[s_index / 8u][s_index % 8u]; }
uint16_t rf_channel_freq(size_t index) { return index < FPV_CHANNELS ? s_channels[index / 8u][index % 8u].freq_mhz : 0; }
size_t rf_channel_index(void) { return s_index; }
const char *rf_band_name(unsigned band) { return band < FPV_BANDS ? s_band_names[band] : "?"; }
uint16_t rf_frequency_mhz(void) { return s_freq; }
int rf_offset_khz(void) { return s_offset_khz; }

/* Public 5 GHz centres. Off-grid FPV channels are first tuned to the
 * nearest one, then moved with phy_set_freq (zerowidth/C5VRX PR #3
 * decoded R8 and tuned E8 this way). */
#define C5_MIN_MHZ 5180u
#define C5_MAX_MHZ 5945u
static const struct { uint8_t channel; uint16_t mhz; } s_centres[] = {
    {132, 5660}, {136, 5680}, {140, 5700}, {144, 5720},
    {149, 5745}, {153, 5765}, {157, 5785}, {161, 5805},
    {165, 5825}, {169, 5845}, {173, 5865}, {177, 5885},
};

esp_err_t rf_set_channel(size_t index)
{
    if (index >= FPV_CHANNELS) return ESP_ERR_INVALID_ARG;
    uint16_t mhz = s_channels[index / 8u][index % 8u].freq_mhz;
    if (mhz < C5_MIN_MHZ || mhz > C5_MAX_MHZ) return ESP_ERR_NOT_SUPPORTED;
    unsigned best = 0;
    for (unsigned k = 1; k < sizeof(s_centres) / sizeof(s_centres[0]); ++k)
        if (abs((int)mhz - (int)s_centres[k].mhz) < abs((int)mhz - (int)s_centres[best].mhz)) best = k;
    phy_begin();
    esp_err_t err = esp_wifi_set_channel(s_centres[best].channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK && mhz != s_centres[best].mhz) phy_set_freq(mhz, 0);
    restore_lock();
    if (err == ESP_OK) {
        /* A retune may regenerate the vendor gain table. */
        capture_table();
        s_index = index;
        s_freq = mhz;
        s_offset_khz = 0;
    }
    phy_end();
    return err;
}

void rf_next_band(void)
{
    for (unsigned step = 1; step <= FPV_BANDS; ++step) {
        size_t next = ((s_index / 8u + step) % FPV_BANDS) * 8u + s_index % 8u;
        if (rf_set_channel(next) == ESP_OK) return;
    }
}

void rf_next_channel(void)
{
    size_t band = s_index / 8u;
    for (unsigned step = 1; step <= 8u; ++step) {
        size_t next = band * 8u + (s_index % 8u + step) % 8u;
        if (rf_set_channel(next) == ESP_OK) return;
    }
}

/* Clamped to +-2.5 MHz (was 1.5): cheap VTXs can be 2 MHz off frequency,
 * and a residual of -0.75 MHz already costs the picture at weak signal
 * (research/situations.py). FPV channels are >= 19 MHz apart, so the AFC
 * can never walk onto another channel. */
#define RF_OFFSET_MAX_KHZ 2500
void rf_set_offset_khz(int offset_khz)
{
    if (offset_khz < -RF_OFFSET_MAX_KHZ) offset_khz = -RF_OFFSET_MAX_KHZ;
    if (offset_khz > RF_OFFSET_MAX_KHZ) offset_khz = RF_OFFSET_MAX_KHZ;
    if (offset_khz == s_offset_khz) return;
    phy_begin();
    s_offset_khz = offset_khz;
    phy_chip_set_chan_offset(offset_khz);
    restore_lock();   /* the vendor helper re-enables BB AGC */
    phy_end();
}
