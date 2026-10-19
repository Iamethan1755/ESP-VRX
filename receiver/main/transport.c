/* C5VRX-5 by Twotoz and the C5VRX contributors: realtime transport.
 * MODEM_DIAG -> PARLIO RX 40 MS/s -> 32 KiB cyclic ring -> TX BitScrambler
 * (U85Q) -> PARLIO TX 40 MHz -> 6-bit resistor DAC. Both GDMA rings run
 * with interrupts off and cleared suc_eof (espressif/esp-idf#19091), so no
 * DMA boundary is a stream boundary; the CPU never paces samples. */
#include "receiver.h"
#include <string.h>
#include "driver/bitscrambler.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_tx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/misc.h"
#include "hal/bitscrambler_ll.h"
#include "kp_lut_io.h"
#include "kp_switch_guard.h"
#include "transport_recovery.h"
#include "output_level.h"
#include "hal/parlio_ll.h"
#include "kp_tables.h"
#include "snapshot.h"
#include "soc/ahb_dma_struct.h"
#include "soc/bitscrambler_struct.h"
#include "soc/parl_io_struct.h"
#include "soc/pcr_struct.h"

/* U85Q UNTRAINED (model 1a09f5c092e8, C5VRX PR #190): closed-form 32-phase
 * tracker on the PAIR4411 shared word, origin cells as coarse quadrant
 * tokens, linear -6..+9 MHz output window. docs/FINDINGS.md. */
BITSCRAMBLER_PROGRAM(s_u85q, "u85q");
/* Calmer loop gains (the MATCHED profiles' kp 0.6 / 0.5) are LUTs of this
 * same program: u85q_kp060/kp050.bsasm have identical instructions and
 * their LUTs are kp_tables.h levels 5 and 7 bit-exactly, so they are not
 * separate programs here and the kp/LP ladder runs on every profile. */
/* U85Q-LP: U85Q plus a post-detection IIR low-pass (pole 1/2) in counter A,
 * same LUT and two bundles (research/u85q_lp.py, README sections 12/16).
 * transport_set_lp() uses the normal pipeline start sequence; a live
 * instruction rewrite left the board video black while USB kept running. */
BITSCRAMBLER_PROGRAM(s_u85q_lp, "u85q_lp");
static bool s_started;   /* live TX running (cleared while menu/status/recovery owns TX) */
static bool s_prog_lp;   /* U85Q-LP instructions loaded */
static const void *tracker_program(bool lp) { return lp ? s_u85q_lp : s_u85q; }

/* 6-bit resistor DAC, LSB first (XIAO D4..D9; 8.2k/3.9k/2k/1k/470R/240R
 * plus 200R). Do not change the tested pin order. */
static const int s_dac_gpio[8] = {23, 24, 11, 12, 8, 9, -1, -1};

DMA_ATTR __attribute__((aligned(64))) uint8_t s_raw_ring[RAW_RING_BYTES];
static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_rx_delimiter;
static parlio_tx_unit_handle_t s_tx;
static bitscrambler_handle_t s_bs;
static bool s_tx_enabled;
static bool s_rx_enabled, s_bs_enabled;
static bool s_transport_fault;
static unsigned s_output, s_verified_output, s_output_clipped;
static int s_verified_level;
static bool s_verified_lp, s_have_verified;
static uint32_t s_restart_errors, s_rollbacks, s_rollback_fail;
#define LIVE_TX_NODES ((RAW_RING_BYTES + 4091u) / 4092u)
DMA_ATTR __attribute__((aligned(64))) static dma_descriptor_t s_live_tx_nodes[LIVE_TX_NODES];
int s_rx_dma_ch = -1, s_tx_dma_ch = -1;
ring_node_t s_rx_nodes[MAX_RING_DESCRIPTORS];
int s_rx_node_count;
static uint32_t s_tx_empty, s_rx_overflow, s_dma_faults, s_bs_overload;

void sync_dma_c2m(const void *addr, size_t size)
{
    if (!addr || !size) return;
    uint32_t start = (uint32_t)addr & ~63u, end = ((uint32_t)addr + size + 63u) & ~63u;
    (void)esp_cache_msync((void *)start, end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void sync_dma_m2c(const void *addr, size_t size)
{
    if (!addr || !size) return;
    uint32_t start = (uint32_t)addr & ~63u, end = ((uint32_t)addr + size + 63u) & ~63u;
    (void)esp_cache_msync((void *)start, end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t enable_tx(void)
{
    esp_err_t err = parlio_tx_unit_enable(s_tx);
    if (err == ESP_OK) s_tx_enabled = true;
    return err;
}

static esp_err_t disable_tx(void)
{
    esp_err_t err = parlio_tx_unit_disable(s_tx);
    if (err == ESP_OK) s_tx_enabled = false;
    return err;
}

static esp_err_t prepare_rx(void)
{
    const parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size = sizeof(s_raw_ring),
        .dma_burst_size = 32u,
        .data_width = 8u,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .exp_clk_freq_hz = IQ_RATE_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        /* Same order as rf.c s_iq_pins: Q bits 0..3, I bits 4..7. */
        .data_gpio_nums = {GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_25, GPIO_NUM_7,
                           GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_3, GPIO_NUM_4},
        .flags = {.free_clk = true, .clk_gate_en = false, .allow_pd = false},
    };
    esp_err_t err = parlio_new_rx_unit(&cfg, &s_rx);
    if (err != ESP_OK) return err;
    const parlio_rx_soft_delimiter_config_t delim = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = sizeof(s_raw_ring),
        .timeout_ticks = 0u,
    };
    err = parlio_new_rx_soft_delimiter(&delim, &s_rx_delimiter);
    if (err != ESP_OK) return err;
    err = parlio_rx_unit_enable(s_rx, false);
    if (err == ESP_OK) s_rx_enabled = true;
    return err;
}

static esp_err_t prepare_tx(void)
{
    parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = 40000000u,
        .data_width = 8u,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = 1u,
        .max_transfer_size = sizeof(s_raw_ring),
        .dma_burst_size = 32u,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    for (unsigned k = 0; k < 8u; ++k) cfg.data_gpio_nums[k] = s_dac_gpio[k];
    esp_err_t err = parlio_new_tx_unit(&cfg, &s_tx);
    if (err != ESP_OK) return err;
    const bitscrambler_config_t bs = {.dir = BITSCRAMBLER_DIR_TX, .attach_to = SOC_BITSCRAMBLER_ATTACH_PARL_IO};
    err = bitscrambler_new(&bs, &s_bs);
    if (err != ESP_OK) return err;
    return enable_tx();
}

static esp_err_t start_rx(void)
{
    esp_err_t err = parlio_rx_soft_delimiter_start_stop(s_rx, s_rx_delimiter, true);
    if (err != ESP_OK) return err;
    const parlio_receive_config_t cfg = {
        .delimiter = s_rx_delimiter,
        .flags = {.partial_rx_en = true, .indirect_mount = false},
    };
    return parlio_rx_unit_receive(s_rx, s_raw_ring, sizeof(s_raw_ring), &cfg);
}

/* Use an owned fixed raw-ring TX chain, just as the menu uses an owned
 * raster chain. Re-submitting IDF loop transactions after repeated P hung
 * in its unbounded FIFO-ready spin. This chain never remounts driver loop
 * buffers, and the readiness wait yields with a bounded deadline. */
static esp_err_t start_tx(void)
{
    size_t offset = 0;
    for (unsigned k = 0; k < LIVE_TX_NODES; ++k) {
        dma_descriptor_t *d = &s_live_tx_nodes[k];
        size_t length = sizeof(s_raw_ring) - offset;
        if (length > 4092u) length = 4092u;
        memset(d, 0, sizeof(*d));
        d->dw0.size = length;
        d->dw0.length = length;
        d->dw0.owner = 1;
        d->buffer = s_raw_ring + offset;
        d->next = &s_live_tx_nodes[(k + 1u) % LIVE_TX_NODES];
        offset += length;
    }
    sync_dma_c2m(s_live_tx_nodes, sizeof(s_live_tx_nodes));
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    parlio_ll_tx_reset_clock(&PARL_IO);
    parlio_ll_tx_reset_fifo(&PARL_IO);
    parlio_ll_tx_set_idle_data_value(&PARL_IO, DAC_IDLE_CODE);
    parlio_ll_tx_set_eof_condition(&PARL_IO, PARLIO_LL_TX_EOF_COND_DATA_LEN);
    parlio_ll_tx_set_trans_bit_len(&PARL_IO, 1);
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    AHB_DMA.out_link_addr[s_tx_dma_ch].val = (uint32_t)&s_live_tx_nodes[0];
    AHB_DMA.channel[s_tx_dma_ch].out.out_link.outlink_start_chn = 1;
    int64_t deadline = esp_timer_get_time() + 20000;
    while (!parlio_ll_tx_is_ready(&PARL_IO)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);   /* taskYIELD alone would still starve lower-priority IDLE */
    }
    parlio_ll_tx_start(&PARL_IO, true);
    parlio_ll_tx_enable_clock(&PARL_IO, true);
    return ESP_OK;
}

/* The CPU LUT port follows lut_mode (ESP32-C5 TRM 44.3.1). On this
 * board, writes/readback under RUN or PAUSE fail; HALT with the normal
 * load/reset/start sequence succeeds. Do not claim a seamless live update. */
static uint16_t s_lut_live[1024];   /* what the LUT RAM holds */
static bool s_lut_known;            /* shadow valid: U85Q program, LUT16 */
static uint8_t s_kp_target;
static uint16_t s_lut_work[1024];
static uint32_t s_kp_writes, s_kp_verify_fail;
static uint32_t s_kp_switches, s_kp_restart_fail, s_kp_restart_us;
static bool resume_live(const uint16_t *restore_lut);
static bool switch_live(int level, bool lp);
static kp_switch_guard_t s_switch_guard;

bool transport_kp_set(int level)
{
    if (s_transport_fault || !s_lut_known || level < 0 || level >= KP_LEVELS) return false;
    s_kp_target = (uint8_t)level;
    return true;
}

/* Verified level, -1 while a different target is pending or unknown. */
int transport_kp_level(void)
{
    if (!s_lut_known || s_transport_fault) return -1;
    return s_verified_level;
}

int transport_kp_target(void) { return s_lut_known ? s_kp_target : -1; }

static void select_lut_entry(void *ctx, unsigned entry)
{
    (void)ctx;
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = entry;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static void store_lut_entry(void *ctx, uint16_t value)
{
    (void)ctx;
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut = value;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static uint32_t read_lut_entry(void *ctx)
{
    (void)ctx;
    return BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut;
}

static bool write_lut_entry(void *ctx, unsigned entry, uint16_t value)
{
    return kp_lut_write16(ctx, entry, value, select_lut_entry, store_lut_entry, read_lut_entry);
}

/* Apply one complete target using the normal pipeline restart. Copy the
 * shadow only after the halted RAM verifies; a failed load resumes baseline
 * video instead of repeatedly retrying writes under the running core. */
unsigned transport_kp_tick(void)
{
    if (!s_lut_known || !s_started || s_menu_active || transport_kp_level() == s_kp_target) return 0;
    uint32_t before = s_kp_writes;
    (void)switch_live(s_kp_target, s_prog_lp);
    return s_kp_writes - before;
}

/* A paused instruction rewrite did not reliably resume video on the board.
 * Program changes therefore use the existing stop/load/reset/start topology,
 * with the verified current kp LUT restored before the core starts. This has
 * a brief video interruption; it is not a sample-seamless program handover. */
static uint32_t s_prog_switches, s_prog_verify_fail, s_prog_restart_fail;
static uint32_t s_prog_restart_us;

static bool program_words(const uint8_t *bin, const uint8_t **inst)
{
    /* bitscrambler_program_hdr_t (esp_driver_bitscrambler): version, hw_rev,
     * hdr_len (words), inst_ct, ... */
    if (bin[0] != 1u || bin[2] != 3u || bin[3] != 8u || bin[4] != 0u || bin[5] != 2u || bin[6] != 1u)
        return false;
    *inst = bin + (size_t)bin[2] * 4u;
    return true;
}

/* All live kp/program writes share one cooldown. Manual P, automatic LP,
 * manual k and mode changes cannot bypass it or alternate two timers. */
static bool switch_live(int level, bool lp)
{
    if (!s_lut_known || !s_started || s_menu_active) return false;
    if (level >= KP_LEVELS || level < -1) return false;
    const uint16_t *lut = level < 0 ? s_lut_live : kp_tables[level];
    bool kp_changed = level >= 0 && level != s_verified_level;
    bool lp_changed = lp != s_prog_lp;
    if (!kp_changed && !lp_changed) return true;
    int64_t began = esp_timer_get_time();
    /* Into LP is the rescue: short gap; everything else keeps the 5 s. */
    if (!(lp_changed && lp ? kp_switch_ready_rescue(&s_switch_guard, began) : kp_switch_ready(&s_switch_guard, began)))
        return false;
    bool previous = s_prog_lp;
    s_prog_lp = lp;
    bool ok = resume_live(lut);
    if (!ok) {
        s_prog_lp = previous;
        if (kp_changed) ++s_kp_restart_fail;
        if (lp_changed) ++s_prog_restart_fail;
    } else {
        if (level >= 0) s_kp_target = (uint8_t)level;
        if (kp_changed) ++s_kp_switches;
        if (lp_changed) ++s_prog_switches;
    }
    int64_t finished = esp_timer_get_time();
    kp_switch_mark(&s_switch_guard, finished);  /* failures interrupted video too */
    if (kp_changed) s_kp_restart_us = (uint32_t)(finished - began);
    if (lp_changed) s_prog_restart_us = (uint32_t)(finished - began);
    ++s_control_epoch;
    return ok;
}

bool transport_apply_auto(int level, bool lp) { return switch_live(level, lp); }

uint32_t transport_switch_wait_ms(void)
{
    int64_t now = esp_timer_get_time();
    return kp_switch_ready(&s_switch_guard, now) ? 0 :
           (uint32_t)((KP_SWITCH_GAP_US - (now - s_switch_guard.last_switch_us) + 999) / 1000);
}

bool transport_set_lp(bool lp)
{
    if (lp == s_prog_lp) return true;
    /* Menu selection does not touch live RAM; applied at menu exit. */
    if (s_menu_active) { s_prog_lp = lp; return true; }
    return switch_live(-1, lp);
}

bool transport_lp(void) { return s_prog_lp; }

const char *transport_program_name(void) { return s_prog_lp ? "U85Q-LP" : "U85Q"; }
bool transport_faulted(void) { return s_transport_fault; }
uint32_t transport_restart_errors(void) { return s_restart_errors; }
unsigned transport_output(void) { return s_output; }
const char *transport_output_name(void)
{
    static const char *const names[OUTPUT_LEVELS] = {"1.0x", "1.5x", "2.0x", "2.5x"};
    return names[s_output];
}
bool transport_set_output(unsigned level)
{
    if (s_transport_fault || level >= OUTPUT_LEVELS) return false;
    if (level == s_output) return true;
    if (s_menu_active || !s_started) { s_output = level; return true; }
    if (!kp_switch_ready(&s_switch_guard, esp_timer_get_time())) return false;
    s_output = level;
    bool ok = resume_live(s_lut_known ? kp_tables[s_verified_level] : NULL);
    kp_switch_mark(&s_switch_guard, esp_timer_get_time());
    ++s_control_epoch;
    return ok;
}

/* Selected configuration stays separate from the last fully verified live
 * configuration. Publication waits for both DMA starts, including readback
 * of the complete program and LUT on boot/menu/rollback as well as P/k. */
typedef struct {
    const uint8_t *program;
    int level;
    bool lp;
    unsigned output, clipped;
} restart_t;

static bool stop_part(void *ctx, tr_part_t part)
{
    (void)ctx;
    esp_err_t err = part == TR_TX ? disable_tx() : part == TR_BS ? bitscrambler_disable(s_bs) :
                    parlio_rx_unit_disable(s_rx);
    if (err != ESP_OK) printf("TRANSPORT teardown part=%d error=%s\n", part, esp_err_to_name(err));
    return err == ESP_OK;
}
static bool quiesce(void *ctx)
{
    bool ok = tr_stop_parts(ctx, stop_part, &s_tx_enabled, &s_bs_enabled, &s_rx_enabled, true);
    s_started = false;
    s_rx_node_count = 0; /* observers cannot consume stopped/stale DMA */
    return ok;
}

static esp_err_t start_rings_tail(void);
static bool restart_stage(void *ctx, tr_stage_t stage, bool rollback)
{
    restart_t *r = ctx;
    if (stage == TR_BS_ENABLE && rollback) {
        s_prog_lp = s_verified_lp;
        s_output = s_verified_output;
        r->lp = s_verified_lp;
        r->level = s_verified_level;
        r->output = s_verified_output;
        r->program = tracker_program(r->lp);
        memcpy(s_lut_work, s_lut_live, sizeof(s_lut_work));
    }
    esp_err_t err = ESP_OK;
    switch (stage) {
    case TR_BS_ENABLE:
        err = bitscrambler_enable(s_bs);
        if (err == ESP_OK) s_bs_enabled = true;
        break;
    case TR_PROGRAM_LOAD: err = bitscrambler_load_program(s_bs, r->program); break;
    case TR_INSTRUCTION_VERIFY: {
        const uint8_t *inst;
        if (!program_words(r->program, &inst)) { ++s_prog_verify_fail; return false; }
        bool verified = true;
        for (unsigned i = 0; i < 8u; ++i)
            for (unsigned w = 0; w < BITSCRAMBLER_LL_INST_LEN_WORDS; ++w) {
                uint32_t expected;
                memcpy(&expected, inst + (i * BITSCRAMBLER_LL_INST_LEN_WORDS + w) * 4u, 4);
                /* Instructions have 257 bits: only bit 0 of word 8 exists. */
                uint32_t mask = w == 8u ? 1u : UINT32_MAX;
                uint32_t actual = bitscrambler_ll_instmem_read(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX, i, w);
                if ((actual & mask) != (expected & mask)) { ++s_prog_verify_fail; verified = false; }
            }
        if (!verified) {
            printf("TRANSPORT restart instruction_verify failed rollback=%u\n", rollback);
            return false;
        }
        break;
    }
    case TR_LUT_VERIFY:
        if (!kp_lut_load(s_lut_work, s_lut_work, write_lut_entry, NULL, &s_kp_writes, &s_kp_verify_fail)) {
            printf("TRANSPORT restart lut_verify failed rollback=%u\n", rollback);
            return false;
        }
        break;
    case TR_BS_RESET: err = bitscrambler_reset(s_bs); break;
    case TR_BS_START: err = bitscrambler_start(s_bs); break;
    case TR_RX_ENABLE:
        err = parlio_rx_unit_enable(s_rx, false);
        if (err == ESP_OK) s_rx_enabled = true;
        break;
    case TR_TX_ENABLE: err = enable_tx(); break;
    case TR_RX_START: err = start_rx(); break;
    case TR_TX_START: err = start_rings_tail(); break;
    default: return false;
    }
    if (err != ESP_OK)
        printf("TRANSPORT restart stage=%d rollback=%u error=%s\n", stage, rollback, esp_err_to_name(err));
    return err == ESP_OK;
}

static int find_dma(bool rx)
{
    for (int k = 0; k < 3; ++k) {
        if (rx && AHB_DMA.channel[k].in.in_peri_sel.peri_in_sel_chn == 9) return k;
        if (!rx && AHB_DMA.channel[k].out.out_peri_sel.peri_out_sel_chn == 9) return k;
    }
    return -1;
}

/* Clear suc_eof on every ring descriptor: the ring becomes endless. */
static int clear_eof(int ch, bool rx)
{
    if (ch < 0 || ch >= 3) return 0;
    uint32_t first = rx ? AHB_DMA.channel[ch].in.in_dscr_bf0.val : AHB_DMA.channel[ch].out.out_dscr_bf0.val;
    if (first < 0x40800000u || first >= 0x40860000u) return 0;
    dma_descriptor_t *d = (dma_descriptor_t *)(uintptr_t)first;
    int count = 0;
    while (d && count < MAX_RING_DESCRIPTORS) {
        d->dw0.suc_eof = 0;
        sync_dma_c2m(d, sizeof(*d));
        if (rx) {
            s_rx_nodes[count].dscr = d;
            s_rx_nodes[count].buffer = (uint8_t *)d->buffer;
            s_rx_nodes[count].length = d->dw0.size ? d->dw0.size : CONTROL_BYTES;
        }
        d = d->next;
        ++count;
        if ((uintptr_t)d == first) break;
    }
    if (rx) s_rx_node_count = count;
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    return count;
}

static void quiet_interrupts(void)
{
    for (int k = 0; k < 3; ++k) { AHB_DMA.in_intr[k].ena.val = 0; AHB_DMA.out_intr[k].ena.val = 0; }
    PARL_IO.int_ena.val = 0;
    /* External-enable EOF selection: soft-delimiter RX never stalls on EOF. */
    PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;
}

/* RX first, TX half a ring later; both run from PLL_F240M/6, so the
 * producer/consumer separation cannot drift. */
static esp_err_t start_rings_tail(void)
{
    quiet_interrupts();
    esp_rom_delay_us((RAW_RING_BYTES / 2ULL) * 1000000ULL / IQ_RATE_HZ);
    s_tx_dma_ch = find_dma(false);
    if (s_tx_dma_ch < 0) return ESP_ERR_NOT_FOUND;
    esp_err_t err = start_tx();
    if (err != ESP_OK) return err;
    s_rx_dma_ch = find_dma(true);
    s_tx_dma_ch = find_dma(false);
    if (s_rx_dma_ch < 0 || s_tx_dma_ch < 0) return ESP_ERR_NOT_FOUND;
    quiet_interrupts();
    if (clear_eof(s_rx_dma_ch, true) < 3 || clear_eof(s_tx_dma_ch, false) < 1)
        return ESP_ERR_INVALID_STATE;
    PARL_IO.int_clr.val = UINT32_MAX;
    if (s_rx_dma_ch >= 0) AHB_DMA.in_intr[s_rx_dma_ch].clr.val = UINT32_MAX;
    if (s_tx_dma_ch >= 0) AHB_DMA.out_intr[s_tx_dma_ch].clr.val = UINT32_MAX;
    BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].val = 1u << 31;
    return ESP_OK;
}

esp_err_t transport_start(void)
{
    memset(s_raw_ring, 0, sizeof(s_raw_ring));
    sync_dma_c2m(s_raw_ring, sizeof(s_raw_ring));
    esp_err_t err = prepare_rx();
    if (err == ESP_OK) err = prepare_tx();
    if (err != ESP_OK) {
        (void)quiesce(NULL);
        s_transport_fault = true;
        return err;
    }
    return resume_live(NULL) ? ESP_OK : ESP_FAIL;
}

/* Start level of a profile (kp_tables.h), LP off. Never under live video:
 * only before the pipeline starts or while the menu / a status screen owns
 * TX; the LUT is then loaded and verified by the normal start sequence. A
 * live profile switch uses transport_apply_auto() (guarded restart). */
bool transport_set_base_level(int level)
{
    if (level < 0 || level >= KP_LEVELS) return false;
    if (s_started && !s_menu_active) {
        printf("BASE level change refused: live video\n");
        return false;
    }
    s_kp_target = (uint8_t)level;
    s_prog_lp = false;
    return true;
}

/* Live -> menu: the raster takes TX, but RX keeps collecting IQ for filter,
 * DC and profile calibration. Full recovery quiesce stops RX as well. */
void transport_stop_tx(void)
{
    bool ok = tr_stop_parts(NULL, stop_part, &s_tx_enabled, &s_bs_enabled, &s_rx_enabled, false);
    s_started = false;
    if (!ok) {
        s_transport_fault = true;
        printf("TRANSPORT ERROR: teardown failed; live recovery latched OFF, USB remains available\n");
    }
}

/* Start a standalone descriptor chain (menu raster) on the TX GDMA. */
void transport_start_chain(dma_descriptor_t *first)
{
    if (s_transport_fault || enable_tx() != ESP_OK) return;
    s_tx_dma_ch = find_dma(false);
    if (s_tx_dma_ch < 0) { (void)disable_tx(); s_transport_fault = true; return; }
    AHB_DMA.out_intr[s_tx_dma_ch].ena.val = 0;
    AHB_DMA.out_intr[s_tx_dma_ch].clr.val = UINT32_MAX;
    PARL_IO.int_ena.val = 0;
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    parlio_ll_tx_reset_clock(&PARL_IO);
    parlio_ll_tx_reset_fifo(&PARL_IO);
    parlio_ll_tx_set_idle_data_value(&PARL_IO, DAC_IDLE_CODE);
    parlio_ll_tx_set_eof_condition(&PARL_IO, PARLIO_LL_TX_EOF_COND_DATA_LEN);
    parlio_ll_tx_set_trans_bit_len(&PARL_IO, 1);
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    AHB_DMA.out_link_addr[s_tx_dma_ch].val = (uint32_t)first;
    AHB_DMA.channel[s_tx_dma_ch].out.out_link.outlink_start_chn = 1;
    /* Wait up to 50 ms, yielding; PARLIO outputs the idle code meanwhile. */
    int64_t deadline = esp_timer_get_time() + 50000;
    while (!parlio_ll_tx_is_ready(&PARL_IO) && esp_timer_get_time() < deadline) vTaskDelay(1);
    if (!parlio_ll_tx_is_ready(&PARL_IO)) {
        (void)disable_tx(); s_transport_fault = true;
        printf("TRANSPORT ERROR: menu TX timeout, USB remains available\n");
        return;
    }
    parlio_ll_tx_start(&PARL_IO, true);
    parlio_ll_tx_enable_clock(&PARL_IO, true);
}

void transport_stop_chain(void)
{
    if (s_tx_enabled && disable_tx() != ESP_OK) s_transport_fault = true;
}

/* Menu -> live: the exact boot topology again. */
static bool resume_live(const uint16_t *restore_lut)
{
    if (s_transport_fault) return false;
    restart_t r = {.program=tracker_program(s_prog_lp), .level=0, .lp=s_prog_lp, .output=s_output};
    if (!restore_lut) restore_lut = kp_tables[s_kp_target];
    if (restore_lut == s_lut_live) {
        memcpy(s_lut_work, s_lut_live, sizeof(s_lut_work));
        r.level = s_verified_level;
        r.clipped = s_output_clipped;
    } else {
        const uint8_t *inst;
        if (!program_words(r.program, &inst) || r.program[6] != 1u) return false;
        for (unsigned i = 0; i < 1024u; ++i) {
            bool clipped;
            s_lut_work[i] = output_entry(restore_lut[i], s_output, &clipped);
            r.clipped += clipped;
        }
        for (int k = 0; k < KP_LEVELS; ++k) if (restore_lut == kp_tables[k]) r.level = k;
    }
    tr_result_t result = tr_restart(&r, quiesce, restart_stage, s_have_verified);
    if (result != TR_APPLIED) ++s_restart_errors;
    if (result == TR_LATCHED) {
        ++s_rollback_fail;
        s_transport_fault = true;
        s_lut_known = false;
        printf("TRANSPORT ERROR: verified recovery failed; live OFF latched, USB/reflash available\n");
        return false;
    }
    s_started = true;
    if (result == TR_ROLLED_BACK) {
        ++s_rollbacks;
        s_kp_target = (uint8_t)s_verified_level;
        printf("TRANSPORT rollback=verified program=%s kp_level=%d output=%s\n",
               transport_program_name(), s_verified_level, transport_output_name());
        return false;
    }
    memcpy(s_lut_live, s_lut_work, sizeof(s_lut_live));
    s_verified_lp = r.lp;
    s_verified_level = r.level;
    s_verified_output = r.output;
    s_output_clipped = r.clipped;
    s_have_verified = true;
    s_lut_known = true;
    s_kp_target = (uint8_t)r.level;
    return true;
}

void transport_resume_live(void)
{
    if (!resume_live(NULL)) printf("TRANSPORT menu return failed; inspect T recovery status\n");
    kp_switch_mark(&s_switch_guard, esp_timer_get_time());
}

void transport_poll_faults(void)
{
    if (PARL_IO.int_raw.tx_fifo_rempty_int_raw) { ++s_tx_empty; PARL_IO.int_clr.tx_fifo_rempty_int_clr = 1; }
    if (PARL_IO.int_raw.rx_fifo_wovf_int_raw) { ++s_rx_overflow; PARL_IO.int_clr.rx_fifo_wovf_int_clr = 1; }
    if (s_rx_dma_ch >= 0) {
        uint32_t raw = AHB_DMA.in_intr[s_rx_dma_ch].raw.val & 0xfcu;
        if (raw) { ++s_dma_faults; AHB_DMA.in_intr[s_rx_dma_ch].clr.val = raw; }
    }
    if (s_tx_dma_ch >= 0) {
        uint32_t raw = AHB_DMA.out_intr[s_tx_dma_ch].raw.val & 0x7cu;
        if (raw) { ++s_dma_faults; AHB_DMA.out_intr[s_tx_dma_ch].clr.val = raw; }
    }
    if (BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].eof_overload) {
        ++s_bs_overload;
        BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].val = 1u << 31;
    }
}

uint32_t transport_fault_count(void)
{
    return s_tx_empty + s_rx_overflow + s_dma_faults + s_bs_overload;
}

void transport_print(void)
{
    printf("OUTPUT level=%s boot_default=1.0x clipped_lut_entries=%u recovery_errors=%lu rollbacks=%lu "
           "rollback_fail=%lu fault_latched=%u live=%u\n", transport_output_name(), s_output_clipped,
           (unsigned long)s_restart_errors, (unsigned long)s_rollbacks, (unsigned long)s_rollback_fail,
           s_transport_fault, s_started);
    printf("TRANSPORT rx_descriptors=%d tx_empty=%lu rx_overflow=%lu dma_faults=%lu bs_overload=%lu "
           "kp_level=%d kp_target=%d kp_writes=%lu kp_verify_fail=%lu program=%s prog_switches=%lu "
           "switch_wait_ms=%lu kp_switches=%lu kp_restart_fail=%lu kp_restart_us=%lu prog_verify_fail=%lu prog_restart_fail=%lu prog_restart_us=%lu\n",
           s_rx_node_count, (unsigned long)s_tx_empty, (unsigned long)s_rx_overflow,
           (unsigned long)s_dma_faults, (unsigned long)s_bs_overload, transport_kp_level(),
           transport_kp_target(), (unsigned long)s_kp_writes, (unsigned long)s_kp_verify_fail,
           transport_program_name(), (unsigned long)s_prog_switches,
           (unsigned long)transport_switch_wait_ms(), (unsigned long)s_kp_switches, (unsigned long)s_kp_restart_fail, (unsigned long)s_kp_restart_us,
           (unsigned long)s_prog_verify_fail,
           (unsigned long)s_prog_restart_fail, (unsigned long)s_prog_restart_us);
}

int ring_node_index(uint32_t addr)
{
    for (int k = 0; k < s_rx_node_count; ++k)
        if ((uintptr_t)s_rx_nodes[k].dscr == addr) return k;
    return -1;
}

static uint32_t rx_active_addr(void) { return AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val; }

/* The newest completed descriptor, copied and proven not lapped. */
bool copy_control_window(uint8_t *dst, size_t bytes)
{
    if (s_rx_dma_ch < 0 || s_rx_node_count < 3) return false;
    /* The control task (priority 3) was preempted mid-copy by the gain
     * observer (same priority, up to ~0.5 ms per window) and the sentinel
     * (4) often enough that 30-44 % of control windows failed the overrun
     * check on the board (T LADDER copy_fail, 2026-10-13). The copy itself
     * takes tens of microseconds: hold the scheduler for it. Interrupts
     * (GDMA, timers) still run. */
    vTaskSuspendAll();
    int64_t start = esp_timer_get_time();
    int active = ring_node_index(rx_active_addr());
    int idx = active < 0 ? -1 : (active - 1 + s_rx_node_count) % s_rx_node_count;
    uint8_t *src = idx < 0 ? NULL : s_rx_nodes[idx].buffer;
    if (!src || s_rx_nodes[idx].length < bytes || src < s_raw_ring ||
        src + bytes > s_raw_ring + sizeof(s_raw_ring)) {
        (void)xTaskResumeAll();
        return false;
    }
    sync_dma_m2c(src, bytes);
    memcpy(dst, src, bytes);
    int after = ring_node_index(rx_active_addr());
    const int64_t took = esp_timer_get_time() - start;
    (void)xTaskResumeAll();
    uint64_t safe_bytes = 0;
    for (int k = 0; k < s_rx_node_count; ++k)
        if (k != active && k != idx) safe_bytes += s_rx_nodes[k].length;
    int advance = after < 0 ? s_rx_node_count : (after - active + s_rx_node_count) % s_rx_node_count;
    return after >= 0 && after != idx && advance < s_rx_node_count - 1 &&
           (uint64_t)took < safe_bytes * 1000000ULL / IQ_RATE_HZ;
}

/* Four separated 64-byte regions of the newest completed descriptor. */
bool copy_probe_window(uint8_t dst[PROBE_BYTES], int *node)
{
    static const size_t offset[PROBE_REGIONS] = {512u, 1536u, 2560u, 4028u};
    if (s_rx_dma_ch < 0 || s_rx_node_count < 2) return false;
    int64_t start = esp_timer_get_time();
    int active = ring_node_index(rx_active_addr());
    if (active < 0) return false;
    int idx = (active - 1 + s_rx_node_count) % s_rx_node_count;
    uint8_t *src = s_rx_nodes[idx].buffer;
    if (!src || s_rx_nodes[idx].length < CONTROL_BYTES || src < s_raw_ring ||
        src + CONTROL_BYTES > s_raw_ring + sizeof(s_raw_ring)) return false;
    for (unsigned r = 0; r < PROBE_REGIONS; ++r) {
        sync_dma_m2c(src + offset[r], PROBE_REGION_BYTES);
        memcpy(dst + r * PROBE_REGION_BYTES, src + offset[r], PROBE_REGION_BYTES);
    }
    if (esp_timer_get_time() - start > 300) return false;
    if (node) *node = idx;
    return ring_node_index(rx_active_addr()) != idx;
}

/* 8190 contiguous bytes (204.75 us) ending where the active descriptor
 * began, valid only if RX could not have reached them during the copy. */
bool copy_snapshot(uint8_t *dst)
{
    if (s_rx_dma_ch < 0 || s_rx_node_count < 4) return false;
    int64_t start = esp_timer_get_time();
    int active = ring_node_index(rx_active_addr());
    if (active < 0) return false;
    uintptr_t base = (uintptr_t)s_raw_ring, first = (uintptr_t)s_rx_nodes[0].buffer;
    if (first < base || first - base >= sizeof(s_raw_ring)) return false;
    size_t total = 0;
    for (int k = 0; k < s_rx_node_count; ++k) {
        uintptr_t a = (uintptr_t)s_rx_nodes[k].buffer;
        if (a < base || !c5v4_snapshot_segment(sizeof(s_raw_ring), first - base, total, a - base,
                                               s_rx_nodes[k].length)) return false;
        total += s_rx_nodes[k].length;
    }
    if (total != sizeof(s_raw_ring)) return false;
    size_t end = (size_t)(s_rx_nodes[active].buffer - s_raw_ring);
    c5v4_snapshot_plan_t plan;
    if (!c5v4_snapshot_plan(total, end, s_rx_nodes[active].length, C5V4_LEVEL_SAMPLE_BYTES, &plan)) return false;
    sync_dma_m2c(s_raw_ring + plan.offset, plan.first);
    memcpy(dst, s_raw_ring + plan.offset, plan.first);
    size_t rest = C5V4_LEVEL_SAMPLE_BYTES - plan.first;
    if (rest) { sync_dma_m2c(s_raw_ring, rest); memcpy(dst + plan.first, s_raw_ring, rest); }
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start);
    int later = ring_node_index(rx_active_addr());
    if (later < 0) return false;
    size_t after = (size_t)(s_rx_nodes[later].buffer - s_raw_ring);
    return c5v4_snapshot_current(total, C5V4_LEVEL_SAMPLE_BYTES, (after + total - end) % total,
                                 s_rx_nodes[later].length, elapsed, plan.safe_bytes, IQ_RATE_HZ);
}

/* Endpoint bytes (odd ring byte of each pair) of the newest `n` pairs. */
bool copy_recent_endpoints(uint8_t *dst, size_t n)
{
    if (s_rx_dma_ch < 0 || s_rx_node_count < 2 || 2u * n > sizeof(s_raw_ring) / 2u) return false;
    int idx = ring_node_index(rx_active_addr());
    if (idx < 0) return false;
    uint8_t *buf = s_rx_nodes[idx].buffer;
    if (buf < s_raw_ring || buf >= s_raw_ring + sizeof(s_raw_ring)) return false;
    size_t end = (size_t)(buf - s_raw_ring) & ~(size_t)1u;
    size_t pos = (end + sizeof(s_raw_ring) - 2u * n) % sizeof(s_raw_ring);
    sync_dma_m2c(s_raw_ring, sizeof(s_raw_ring));
    for (size_t k = 0; k < n; ++k) {
        dst[k] = s_raw_ring[pos + 1u];
        pos = (pos + 2u) % sizeof(s_raw_ring);
    }
    return true;
}

/* Sampling phase: holding the RX divider one count higher for ~1 us moves
 * the RX edge against the MODEM_DIAG update by a few 4.17 ns ticks
 * (zerowidth PR #3 does this on its sample clock). */
void rx_clock_slip(uint32_t us)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    uint32_t div = PCR.parl_clk_rx_conf.parl_clk_rx_div_num;
    HAL_FORCE_MODIFY_U32_REG_FIELD(PCR.parl_clk_rx_conf, parl_clk_rx_div_num, div + 1u);
    esp_rom_delay_us(us);
    HAL_FORCE_MODIFY_U32_REG_FIELD(PCR.parl_clk_rx_conf, parl_clk_rx_div_num, div);
    portEXIT_CRITICAL(&mux);
}

metrics_t measure_window(const uint8_t *s, size_t n)
{
    metrics_t m = {0};
    uint16_t hist[129] = {0};
    int clip = 0, origin = 0, coherent = 0, pi = 0, pq = 0;
    for (size_t k = 0; k < n; ++k) {
        int q = (int8_t)((s[k] & 0x0fu) << 4) >> 4, i = (int8_t)(s[k] & 0xf0u) >> 4;
        if (i == -8 || i == 7 || q == -8 || q == 7) ++clip;
        int p = i * i + q * q;
        if (p <= 4) ++origin;
        if (k) {
            int dot = i * pi + q * pq, cross = q * pi - i * pq;
            if (p >= 8 && dot > 0 && (cross < 0 ? -cross : cross) <= dot) ++coherent;
        }
        ++hist[p > 128 ? 128 : p];
        pi = i; pq = q;
    }
    unsigned cumulative = 0;
    for (unsigned p = 0; p <= 128u; ++p) {
        cumulative += hist[p];
        if (cumulative >= (n + 1u) / 2u) { m.p_median = (int)p; break; }
    }
    m.q_phase = n > 1u ? coherent * 100 / (int)(n - 1u) : 0;
    m.clip_pm = n ? clip * 1000 / (int)n : 0;
    m.origin_pm = n ? origin * 1000 / (int)n : 1000;
    return m;
}
