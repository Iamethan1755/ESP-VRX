/* C5VRX by Twotoz and contributors; pinned C5 PHY investigation #150-153.
 * No production overrides. Polling cannot prove absence of short gate pulses. */
#include "phy_rx_lab.h"
#include "rf.h"
#include "predemod.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PHYBIT(n) (UINT32_C(1) << (n))
#define EVENT_CAP 64u
#define PROFILE_US 10000000LL
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Task ownership is recursive for nested channel/bandwidth restoration. The
 * short spinlock below protects observations; it must never cover vendor I2C. */
static StaticSemaphore_t s_transaction_storage;
static SemaphoreHandle_t s_transaction;
static void transaction_init(void)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_transaction)
        s_transaction=xSemaphoreCreateRecursiveMutexStatic(&s_transaction_storage);
    portEXIT_CRITICAL(&s_mux);
    configASSERT(s_transaction);
}
static void transaction_take(void)
{
    transaction_init();
    int taken=xSemaphoreTakeRecursive(s_transaction, portMAX_DELAY);
    configASSERT(taken==pdTRUE);
    (void)taken;
}
static void transaction_give(void)
{
    int given=xSemaphoreGiveRecursive(s_transaction);
    configASSERT(given==pdTRUE);
    (void)given;
}

typedef struct {
    uint32_t addr, baseline, current, changes;
    int64_t changed_us;
} invariant_t;
static invariant_t s_regs[] = {
    { .addr=0x600A0890 }, { .addr=0x600A9C18 },
    { .addr=0x600A7C3C }, { .addr=0x600A7C40 }, { .addr=0x600A7C08 },
    { .addr=0x600A0800 }, { .addr=0x600A0400 }, { .addr=0x600A7C80 },
    { .addr=0x600A7010 }, { .addr=0x600A7014 }, { .addr=0x600A7044 },
    { .addr=0x600A7108 }, { .addr=0x600A0814 }, { .addr=0x600A0874 },
    { .addr=0x600A0430 }, { .addr=0x600A0448 },
    { .addr=0x600A702C }, { .addr=0x600A7030 },
    { .addr=0x600A4400 }, { .addr=0x600A7CE0 }, { .addr=0x600A7CE4 },
    { .addr=0x600A7904 }, { .addr=0x600A7074 },
    { .addr=0x600A0438 }, { .addr=0x600A0C0C }, { .addr=0x600A043C },
    { .addr=0x600A0C38 }, { .addr=0x600A0C3C }, { .addr=0x600A7068 },
    { .addr=0x600A7018 }, { .addr=0x600A701C },
    { .addr=0x600A7C44 }, { .addr=0x600A7C50 }, { .addr=0x600A4C5C },
    /* Only slot zero is decoded here; do not infer arbitrary slot bounds. */
    { .addr=0x600A7C14 }, { .addr=0x600A7810 }, { .addr=0x600A7814 },
    { .addr=0x600A7830 },
};
#define NREG (sizeof(s_regs) / sizeof(s_regs[0]))
typedef struct {
    int64_t time_us;
    uint32_t generation, addr, old_value, new_value;
} event_t;
static event_t s_events[EVENT_CAP];
static uint32_t s_event_total, s_generation, s_depth;
static bool s_ready, s_monitor;
static uint32_t s_vendor[NREG];
static int64_t s_last_poll;
/* OSI fields are shared with Wi-Fi tasks. Protected by the same short lock. */
static uint32_t s_enables, s_disables;
static int64_t s_enable_us, s_disable_us;
static const char *s_reason = "boot";

typedef struct { uint32_t addr, mask, value; } patch_t;
typedef struct { const char *name; unsigned count; patch_t patches[3]; } profile_t;
/* All masks recovered from the pinned binary. Names describe writes, not
 * alleged bypass/sensitivity semantics. Never change ADC/filter tuples here. */
static const profile_t s_profiles[] = {
    {"STOCK", 0, {{0}}},
    {"PKDADC_BIT31_CLEAR", 1, {{0x600A0C38, PHYBIT(31), 0}}},
    {"NF_AUTO_FREEZE", 3, {{0x600A7018, PHYBIT(23)|PHYBIT(28), 0},
                           {0x600A7C44, PHYBIT(0), 0}, {0x600A7C50, PHYBIT(0), 0}}},
    {"BB_CCA_BIT30_CLEAR", 1, {{0x600A7018, PHYBIT(30), 0}}},
    /* phy_wifi_fbw_sel(0): multiple fields, not just one bit. */
    {"FRONT_BW_ARG0", 1, {{0x600A0874, 0x003F0000u, 0}}},
    /* phy_bb_bss_cbw40_dig(0) clears BOTH bits 2 and 3. */
    {"DIG_BW_ARG0", 1, {{0x600A9C18, PHYBIT(2)|PHYBIT(3), 0}}},
    {"CHAN_A_ARG0", 1, {{0x600A7904, PHYBIT(22)|7u, PHYBIT(22)}}},
    {"CHAN_A_ARG1", 1, {{0x600A7904, PHYBIT(22), 0}}},
    {"CHAN_B_ARG0", 1, {{0x600A7074, PHYBIT(13), PHYBIT(13)}}},
    {"CHAN_B_ARG1", 1, {{0x600A7074, PHYBIT(13), 0}}},
    {"SPUR_SLOT0_BIT13_CLEAR", 1, {{0x600A7C14, PHYBIT(13), 0}}},
};
static unsigned s_profile;
#ifdef C5VRX_PHY_RX_LAB_PINNED
static unsigned s_next_profile = 1;
#endif
static uint32_t s_saved[3];
static int64_t s_deadline;

static void record_locked(int64_t now, uint32_t addr, uint32_t old, uint32_t value)
{
    s_events[s_event_total % EVENT_CAP] = (event_t){now, s_generation, addr, old, value};
    ++s_event_total;
}

static void capture_locked(void)
{
    for (unsigned i=0; i<NREG; ++i) {
        s_regs[i].baseline = s_regs[i].current = REG(s_regs[i].addr);
    }
}

static void stock_locked(void)
{
    const profile_t *p = &s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x = p->patches[i];
        /* Restore exactly the owned fields. Do not replay unrelated volatile
         * status or a vendor update in another field of the same register. */
        REG(x.addr) = (REG(x.addr) & ~x.mask) | (s_saved[i] & x.mask);
    }
    s_profile = 0;
    s_deadline = 0;
}

void phy_rx_lab_osi_event(bool enable)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (enable) { ++s_enables; s_enable_us=now; }
    else { ++s_disables; s_disable_us=now; }
    if (s_ready) record_locked(now, enable ? 1u : 2u, s_depth,
                              enable ? s_enables : s_disables);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_begin(const char *reason)
{
    transaction_take();
    portENTER_CRITICAL(&s_mux);
    if (s_depth++ == 0) {
        stock_locked(); /* Never carry a previous-channel snapshot into a tune. */
        s_reason = reason;
        if (s_ready) record_locked(esp_timer_get_time(), 3, 0, 1);
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_capture_vendor(void)
{
    portENTER_CRITICAL(&s_mux);
    for (unsigned i=0; i<NREG; ++i) s_vendor[i]=REG(s_regs[i].addr);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_end(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_depth && --s_depth == 0) {
        ++s_generation;
        capture_locked();
        s_ready = true;
        record_locked(esp_timer_get_time(), 3, 1, 0);
    }
    portEXIT_CRITICAL(&s_mux);
    transaction_give();
}

uint32_t phy_rx_lab_generation(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t generation=s_generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}

bool phy_rx_lab_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy=s_depth != 0;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

/* Gain/lane writes share the RF mutex without cancelling an A/B profile or
 * publishing a new tune generation. Never wait with a stale control decision. */
bool phy_rx_lab_try_actuator(uint32_t expected_generation)
{
    transaction_init();
    if (xSemaphoreTakeRecursive(s_transaction, 0) != pdTRUE) return false;
    portENTER_CRITICAL(&s_mux);
    bool valid = !s_depth && s_generation == expected_generation;
    portEXIT_CRITICAL(&s_mux);
    if (!valid) transaction_give();
    return valid;
}
void phy_rx_lab_end_actuator(void) { transaction_give(); }

void phy_rx_lab_poll(void)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_ready && !s_depth && s_profile && now >= s_deadline) {
        stock_locked();
        ++s_generation;
        capture_locked();
        record_locked(now, 4, 1, 0); /* timeout, no UART in polling */
    }
    if (!s_ready || s_depth || !s_monitor || now-s_last_poll < 50000) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_last_poll=now;
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t *r=&s_regs[i];
        uint32_t value=REG(r->addr);
        if (value != r->current) {
            record_locked(now, r->addr, r->current, value);
            r->current=value;
            ++r->changes;
            r->changed_us=now;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_toggle_monitor(void)
{
    portENTER_CRITICAL(&s_mux);
    s_monitor=!s_monitor;
    bool enabled=s_monitor;
    if (s_ready && !s_depth) capture_locked();
    portEXIT_CRITICAL(&s_mux);
    printf("PHYINV monitor=%u poll_us=50000 ring=%u\n", enabled, EVENT_CAP);
}

void phy_rx_lab_stock(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    stock_locked();
    if (active) { ++s_generation; capture_locked(); }
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=STOCK restored=%u\n", active);
}

bool phy_rx_lab_profile_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

void phy_rx_lab_next_profile(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    printf("PHYLAB refused=unverified_PHY_binary read_only=1\n");
    return;
#else
    if (rf_native_agc_active()) {
        printf("PHYLAB refused=native_AGC_owner\n");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (!s_ready || s_depth) {
        portEXIT_CRITICAL(&s_mux);
        printf("PHYLAB refused=transaction_active\n");
        return;
    }
    stock_locked();
    s_profile=s_next_profile++;
    if (s_next_profile >= sizeof(s_profiles)/sizeof(s_profiles[0])) s_next_profile=1;
    const profile_t *p=&s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        s_saved[i]=REG(x.addr);
        REG(x.addr)=(s_saved[i] & ~x.mask) | x.value;
    }
    __sync_synchronize();
    bool verified=true;
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        if ((REG(x.addr)&x.mask) != x.value) verified=false;
    }
    const char *name=p->name;
    if (!verified) stock_locked();
    s_deadline=s_profile ? esp_timer_get_time()+PROFILE_US : 0;
    ++s_generation;
    capture_locked();
    record_locked(esp_timer_get_time(), 4, 0, s_profile);
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=%s verified=%u timeout_ms=10000 persistent=0\n", name, verified);
#endif
}

void phy_rx_lab_mark(void)
{
    phy_rx_lab_poll();
    portENTER_CRITICAL(&s_mux);
    int64_t now=esp_timer_get_time();
    record_locked(now, 5, 0, 0);
    uint32_t total=s_event_total;
    portEXIT_CRITICAL(&s_mux);
    printf("PHYMARK t=%" PRId64 " events=%" PRIu32 "\n", now, total);
    uint32_t start=total>EVENT_CAP ? total-EVENT_CAP : 0;
    for (uint32_t i=start; i<total; ++i) {
        portENTER_CRITICAL(&s_mux);
        event_t e=s_events[i%EVENT_CAP];
        portEXIT_CRITICAL(&s_mux);
        printf("PHYEV t=%" PRId64 " gen=%" PRIu32 " reg=%08" PRIx32
               " old=%08" PRIx32 " new=%08" PRIx32 "\n",
               e.time_us,e.generation,e.addr,e.old_value,e.new_value);
    }
}

extern unsigned char phy_param[];
extern uint8_t phy_i2c_readReg(uint8_t block, uint8_t host, uint8_t reg);
void phy_rx_lab_dump(bool analog_i2c)
{
    /* Copy under lock; print outside it. No logging in the RF timing path. */
    invariant_t snapshot[NREG];
    uint32_t vendor[NREG];
    portENTER_CRITICAL(&s_mux);
    bool ready=s_ready && !s_depth;
    if (ready) {
        for (unsigned i=0; i<NREG; ++i) {
            snapshot[i]=s_regs[i];
            vendor[i]=s_vendor[i];
            snapshot[i].current=REG(snapshot[i].addr);
        }
    }
    uint32_t generation=s_generation, en=s_enables, dis=s_disables;
    int64_t en_us=s_enable_us, dis_us=s_disable_us;
    const char *reason=s_reason, *profile=s_profiles[s_profile].name;
    portEXIT_CRITICAL(&s_mux);
    if (!ready) { printf("PHYINV unavailable=transaction_or_startup\n"); return; }
    printf("PHYINV gen=%" PRIu32 " reason=%s profile=%s native=%u bw=%u freq=%u offset=%d"
           " enable=%" PRIu32 " enable_us=%" PRId64 " disable=%" PRIu32 " disable_us=%" PRId64 "\n",
           generation,reason,profile,rf_native_agc_active(),rf_get_analog_bandwidth()?40:20,
           rf_get_frequency_mhz(),rf_get_frequency_offset_khz(),en,en_us,dis,dis_us);
    printf("PHYOWN direct_agc_disabled=%u expected=%u\n",
           (unsigned)((REG(0x600A7030) >> 29) & 1u), !rf_native_agc_active());
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t r=snapshot[i];
        printf("PHYREG reg=%08" PRIx32 " vendor=%08" PRIx32 " lock=%08" PRIx32 " now=%08" PRIx32
               " changes=%" PRIu32 " last_us=%" PRId64 "\n",
               r.addr,vendor[i],r.baseline,r.current,r.changes,r.changed_us);
    }
#ifdef C5VRX_PHY_RX_LAB_PINNED
    if (analog_i2c) {
        /* Explicit lab-only vendor I2C reads; never poll these in LOCK. */
        for (unsigned i=0xF5; i<=0xFC; ++i)
            printf("PHYPARAM offset=%03x value=%02x\n",i,phy_param[i]);
        for (uint8_t reg=6; reg<=21; ++reg)
            printf("PHYI2C block=67 reg=%02x value=%02x\n",reg,
                   phy_i2c_readReg(0x67,1,reg));
    }
#else
    (void)analog_i2c;
#endif
}

/* Issue #155: scoped vendor experiment, never a production/NVS setting.
 * phy_11p_set(0,0) installs defaults, so it is NOT an exact rollback. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_11p_set(uint8_t enable, uint8_t mode);
extern void phy_i2c_writeReg(uint8_t block, uint8_t host, uint8_t reg, uint8_t value);
static const patch_t s_11p_fields[] = {
    {0x600A7CE4, 0x1cu, 0x10u}, {0x600A7030, PHYBIT(5), 0},
    {0x600A7048, 0x7f00u, 0x4000u}, {0x600A71C4, 0x00fe0000u, 0x00440000u},
};
static void snapshot_11p(const char *stage)
{
    printf("PHY11P_STATE stage=%s flags=%02x/%02x\n",stage,phy_param[0x26],phy_param[0x27]);
    for (unsigned i=0; i<4; ++i)
        printf("PHY11P_REG reg=%08" PRIx32 " value=%08" PRIx32 " mask=%08" PRIx32 "\n",
               s_11p_fields[i].addr,REG(s_11p_fields[i].addr),s_11p_fields[i].mask);
    for (uint8_t i=6; i<=13; ++i)
        printf("PHY11P_I2C reg=%u value=%02x\n",i,phy_i2c_readReg(0x67,1,i));
}
static bool verify_11p(const uint32_t *values, const uint8_t *analog)
{
    for (unsigned i=0; i<4; ++i)
        if ((REG(s_11p_fields[i].addr)&s_11p_fields[i].mask) != values[i]) return false;
    for (uint8_t i=0; i<8; ++i)
        if (phy_i2c_readReg(0x67,1,6+i)!=analog[i]) return false;
    return true;
}
#endif
esp_err_t phy_rx_lab_run_11p_probe(void (*observe)(const char *stage))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe;
    printf("PHY11P refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!observe || rf_native_agc_active()) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("11p_AB");
    uint32_t saved[4], enabled[4];
    uint8_t analog[8], expected[8];
    uint8_t flags[2]={phy_param[0x26],phy_param[0x27]};
    for (unsigned i=0; i<4; ++i) {
        saved[i]=REG(s_11p_fields[i].addr)&s_11p_fields[i].mask;
        enabled[i]=s_11p_fields[i].value;
    }
    for (uint8_t i=0; i<8; ++i) {
        analog[i]=phy_i2c_readReg(0x67,1,6+i);
        expected[i]=60;
    }
    snapshot_11p("BASELINE");
    observe("BASELINE");
    phy_11p_set(1,0);
    bool applied=phy_param[0x26]==1 && phy_param[0x27]==0 && verify_11p(enabled,expected);
    printf("PHY11P apply_verified=%u persistent=0\n",applied);
    if (applied) { snapshot_11p("ENABLED"); observe("ENABLED"); }
    /* Clear the persistent vendor flags before another channel operation can
     * replay 11p. Restore saved analog bytes, then only the owned MMIO masks. */
    phy_param[0x26]=flags[0]; phy_param[0x27]=flags[1];
    for (uint8_t i=0; i<8; ++i) phy_i2c_writeReg(0x67,1,6+i,analog[i]);
    for (unsigned i=0; i<4; ++i) {
        patch_t x=s_11p_fields[i];
        REG(x.addr)=(REG(x.addr)&~x.mask)|saved[i];
    }
    __sync_synchronize();
    bool restored=phy_param[0x26]==flags[0] && phy_param[0x27]==flags[1] && verify_11p(saved,analog);
    printf("PHY11P restore_verified=%u\n",restored);
    if (restored) { snapshot_11p("RESTORED"); observe("RESTORED"); }
    phy_rx_lab_end();
    return restored ? (applied ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : ESP_FAIL;
#endif
}

/* ---- Range labs (2026-10-04) ----------------------------------------------
 * Idea from FPVGateC5RX (RaceFPV / Louis Hitchcock, docs/PROVENANCE.md and
 * EXTENDED_TUNING.md: facts only, no code). Own reading of the pinned
 * libphy: phy_get_sigrssi() = (int8_t)(0x600A706C >> 8); the enable rewrites
 * AGC words 0x600A7008/0C/10/18/30/48/90/B0/C4/EC/150. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_check_sigrssi_en(uint8_t enable);
extern int8_t phy_get_sigrssi(void);
extern void phy_param_track_tot(uint8_t wifi, uint8_t bt);
static const uint16_t s_sigrssi_words[] = {
    0x008, 0x00C, 0x010, 0x018, 0x030, 0x048, 0x090, 0x0B0, 0x0C4, 0x0EC, 0x150,
};
#define SIGRSSI_WORDS (sizeof(s_sigrssi_words) / sizeof(s_sigrssi_words[0]))
#define SIGRSSI_SAMPLES 1000u
#endif
esp_err_t phy_rx_lab_run_sigrssi_probe(void (*observe)(const char *stage),
                                       phy_rx_lab_rssi_stats_t *stats)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe; (void)stats;
    printf("SIGRSSI refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!observe || !stats) return ESP_ERR_INVALID_STATE;
    if (REG(0x600A7030) & PHYBIT(29)) {
        printf("SIGRSSI refused=bb_agc_gate_held\n");
        return ESP_ERR_INVALID_STATE;
    }
    phy_rx_lab_begin("sigrssi_AB");
    uint32_t saved[SIGRSSI_WORDS];
    for (unsigned i = 0; i < SIGRSSI_WORDS; ++i) saved[i] = REG(0x600A7000u + s_sigrssi_words[i]);
    observe("BASELINE");
    phy_check_sigrssi_en(1);
    unsigned hist[256] = {0};
    int64_t sum = 0;
    *stats = (phy_rx_lab_rssi_stats_t){.min_dbm = 127, .max_dbm = -128};
    for (unsigned n = 0; n < SIGRSSI_SAMPLES; ++n) {
        int v = phy_get_sigrssi();
        ++hist[(unsigned)(v + 128) & 255u];
        sum += v;
        if (v < stats->min_dbm) stats->min_dbm = v;
        if (v > stats->max_dbm) stats->max_dbm = v;
        vTaskDelay(1);
    }
    stats->samples = SIGRSSI_SAMPLES;
    stats->mean_dbm_x10 = (int)(sum * 10 / (int64_t)SIGRSSI_SAMPLES);
    unsigned cumulative = 0;
    bool p10 = false, p50 = false;
    for (unsigned b = 0; b < 256u; ++b) {
        cumulative += hist[b];
        int dbm = (int)b - 128;
        if (!p10 && cumulative * 10u >= SIGRSSI_SAMPLES) { stats->p10_dbm = dbm; p10 = true; }
        if (!p50 && cumulative * 2u >= SIGRSSI_SAMPLES) { stats->p50_dbm = dbm; p50 = true; }
        if (cumulative * 10u >= SIGRSSI_SAMPLES * 9u) { stats->p90_dbm = dbm; break; }
    }
    observe("SIGRSSI_ON");
    for (unsigned i = 0; i < SIGRSSI_WORDS; ++i) REG(0x600A7000u + s_sigrssi_words[i]) = saved[i];
    __sync_synchronize();
    bool restored = true;
    for (unsigned i = 0; i < SIGRSSI_WORDS; ++i)
        restored &= REG(0x600A7000u + s_sigrssi_words[i]) == saved[i];
    printf("SIGRSSI restore_verified=%u words=%u\n", restored, (unsigned)SIGRSSI_WORDS);
    if (restored) observe("RESTORED");
    phy_rx_lab_end();
    return restored ? ESP_OK : ESP_FAIL;
#endif
}

esp_err_t phy_rx_lab_run_track_probe(void (*observe)(const char *stage))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe;
    printf("PHYTRACK refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!observe) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("track_AB");
    observe("BASELINE");
    int64_t start = esp_timer_get_time();
    phy_param_track_tot(1, 0);
    printf("PHYTRACK call_us=%u freq=%u\n", (unsigned)(esp_timer_get_time() - start),
           rf_get_frequency_mhz());
    observe("TRACKED");
    phy_rx_lab_end();
    return ESP_OK;
#endif
}

/* ---- Digital RX filter / ADC-rate lab (2026-10-04) ------------------------
 * Own reading of the pinned libphy. phy_chip_set_chan -> phy_rfpll_set_adc_rate:
 * on 5 GHz phy_adc_rate_set(0) + phy_rx_filter_mode(0) (mode 4 when the stored
 * width is BW20), then above 5830 MHz phy_adc_rate_set(1) + mode 8.
 * phy_rx_filter_mode(m) = 0x600A0430[21:18]; phy_adc_rate_set(r) = I2C block
 * 0x66 host 0 reg 4 bit 2 = !r and 0x600A0448[1:0] = r,r. The PARLIO capture
 * takes every second sample of the ~80 MS/s MODEM_DIAG bus with no filter in
 * between, so a digital filter ahead of the tap would be a free steep
 * pre-detection filter. Whether one is ahead of the tap is unknown: this lab
 * measures it. Normal C5 Wi-Fi never calls phy_spur_coef_cfg (only
 * librftest's set_spur_reg does), so the spur slots are snapshotted, not
 * changed. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_adc_rate_set(uint32_t rate);
extern void phy_rx_filter_mode(uint32_t mode);
#define DFILT_REG      0x600A0430u
#define DFILT_SHIFT    18u
#define DFILT_MASK     (UINT32_C(0xF) << DFILT_SHIFT)
#define DFILT_ADC_REG  0x600A0448u
#define DFILT_ADC_BLOCK 0x66u
#define DFILT_ADC_HOST 0u
#define DFILT_ADC_I2C  4u
#endif
esp_err_t phy_rx_lab_run_dfilt_probe(void (*observe)(const char *stage, int arg))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe;
    printf("DFILT refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!observe) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("dfilt_AB");
    const uint32_t filt = REG(DFILT_REG), adc = REG(DFILT_ADC_REG);
    const uint8_t adc_i2c = phy_i2c_readReg(DFILT_ADC_BLOCK, DFILT_ADC_HOST, DFILT_ADC_I2C);
    const unsigned mode = (filt & DFILT_MASK) >> DFILT_SHIFT, rate = adc & 1u;
    printf("DFILT begin freq=%u vendor_mode=%u adc_sel=%u adc_word=0x%08" PRIx32
           " adc_i2c=0x%02x spur_slots=0x%08" PRIx32 "/0x%08" PRIx32 "/0x%08" PRIx32 "/0x%08" PRIx32 "\n",
           rf_get_frequency_mhz(), mode, rate, adc, (unsigned)adc_i2c,
           REG(0x600A7C14), REG(0x600A7C18), REG(0x600A7C1C), REG(0x600A7C20));
    observe("BASELINE", (int)mode);
    /* Filter mode alone, ADC rate unchanged. */
    for (unsigned m = 0; m < 16u; ++m) {
        REG(DFILT_REG) = (filt & ~DFILT_MASK) | ((uint32_t)m << DFILT_SHIFT);
        observe("MODE", (int)m);
    }
    REG(DFILT_REG) = filt;
    observe("MODE_RESTORED", (int)mode);
    /* The other ADC rate through the vendor setter: first with the current
     * filter mode, then with the mode the vendor pairs it with on 5 GHz. */
    const unsigned alt = rate ^ 1u;
    phy_adc_rate_set(alt);
    observe("ADC_ALT", (int)alt);
    phy_rx_filter_mode(alt ? 8u : 0u);
    observe("ADC_ALT_VENDOR_MODE", alt ? 8 : 0);
    phy_adc_rate_set(rate);
    REG(DFILT_ADC_REG) = adc;
    REG(DFILT_REG) = filt;
    __sync_synchronize();
    bool restored = REG(DFILT_REG) == filt && REG(DFILT_ADC_REG) == adc &&
                    phy_i2c_readReg(DFILT_ADC_BLOCK, DFILT_ADC_HOST, DFILT_ADC_I2C) == adc_i2c;
    printf("DFILT restore_verified=%u\n", restored);
    if (restored) observe("RESTORED", (int)mode);
    phy_rx_lab_end();
    return restored ? ESP_OK : ESP_FAIL;
#endif
}

/* Native analog patch prototype (#139): retain the hardware-selected tuple,
 * stop only BB acquisition, then use the vendor resume strobe. This bounded
 * console experiment does not infer acquisition-complete from an unknown FSM
 * byte and does not establish automatic analog tracking. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_disable_agc(void);
extern void phy_enable_agc(void);
#endif
esp_err_t phy_rx_lab_run_native_hold(unsigned cycles,
    void (*observe)(const char *stage, unsigned cycle))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)cycles; (void)observe;
    printf("NATIVEHOLD refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!rf_native_agc_active() || !observe || (cycles!=1 && cycles!=100))
        return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("native_hold_lab");
    /* 702C is force/control configuration, NOT live gain status. Bit23 is
     * also the vendor resume strobe. Refuse any pre-existing forced mode. */
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
        phy_rx_lab_end();
        printf("NATIVEHOLD refused=already_held_or_forced\n");
        return ESP_ERR_INVALID_STATE;
    }
    unsigned completed=0;
    bool verified=true;
    observe("FREE_BASELINE",0);
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) verified=false;
    for (unsigned n=1; verified && n<=cycles; ++n) {
        phy_disable_agc(); /* Exactly 7030 bit29; no RF disable, no gain write. */
        if (!(REG(0x600A7030)&PHYBIT(29))) { verified=false; break; }
        observe("HOLD",n);
        /* An asynchronous writer reopening BB acquisition taints this trial. */
        if (!(REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
            verified=false; break;
        }
        phy_enable_agc(); /* Clear29 and pulse702C[23]; leaves RF AGC intact. */
        if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
            verified=false; break;
        }
        ++completed;
        observe("FREE_RESTORED",n);
        if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) verified=false;
    }
    /* Always recover baseline native ownership, even after failed hold. */
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23)))
        phy_enable_agc();
    bool restored=!(REG(0x600A7030)&PHYBIT(29)) && !(REG(0x600A702C)&PHYBIT(23));
    phy_rx_lab_end();
    printf("NATIVEHOLD completed=%u requested=%u gate_verified=%u restored=%u "
           "explicit_gain_index_writes=0 rf_disable_calls=0 hardware_acceptance=pending\n",
           completed,cycles,verified,restored);
    return restored ? (verified ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : ESP_FAIL;
#endif
}

/* Pre-demodulation labs (#165): DC DACs and RX filter capacitors. Recovered
 * from the pinned libphy: phy_pbus_set_dco() writes PBUS (block,bank)
 * (2,1),(3,1),(2,2),(3,2); phy_pbus_force_test() owns them in debug mode;
 * phy_filter_dcap_set() programs 0x67 regs 6..20 from phy_param[0xF5..0xFC];
 * phy_11p_set(1,0) writes 60 to regs 6..13. Bank 2 is the pair ESPARGOS
 * esp-sdr corrects on the S31; the response matrix is measured, not assumed. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern uint16_t phy_pbus_rd(uint32_t block, uint32_t bank);
extern void phy_pbus_force_test(uint32_t block, uint32_t bank, uint32_t value);
extern void phy_pbus_debugmode(void);
extern void phy_pbus_workmode(void);
#define DCO_PROBE 16
#define DCO_RANGE 96
#define DCO_TRIAL 32
#define DCO_DONE_MCELLS 100
static int dco_cost(const int dc[2]) { return dc[0] * dc[0] + dc[1] * dc[1]; }
static int dco_clamp(int value, int base)
{
    if (value < base - DCO_RANGE) value = base - DCO_RANGE;
    if (value > base + DCO_RANGE) value = base + DCO_RANGE;
    return value < 0 ? 0 : value > 511 ? 511 : value;
}
static void dco_apply(int i, int q)
{
    phy_pbus_force_test(2, 2, (uint32_t)i);
    phy_pbus_force_test(3, 2, (uint32_t)q);
}
/* Last search result, RAM only, for the on/off A/B ('6'). */
static int s_dco_codes[2], s_dco_vendor[2];
static bool s_dco_valid;
#endif

/* Fixed analog bandwidth state (see phy_rx_lab_filter_set_code). */
#ifdef C5VRX_PHY_RX_LAB_PINNED
static uint8_t s_filter_base[8];
static bool s_filter_base_valid;
#endif
static int s_filter_code = PHY_RX_LAB_FILTER_CALIBRATED;

void phy_rx_lab_predemod_status(void)
{
    uint16_t mhz = rf_get_frequency_mhz();
    printf("PREDEMOD_PHY freq=%u dc_cal_multi=%u dc_cal_mhz=%u above_last_point=%u "
           "pll_track=disabled_by_config\n", mhz, phy_param[0x2a] != 0,
           predemod_dc_cal_point(mhz, phy_param[0x2a] != 0), mhz > 5855u);
#ifdef C5VRX_PHY_RX_LAB_PINNED
    /* Recursive task ownership without a generation bump: read-only. */
    transaction_take();
    uint16_t dco[4] = {phy_pbus_rd(2, 1), phy_pbus_rd(3, 1), phy_pbus_rd(2, 2), phy_pbus_rd(3, 2)};
    uint8_t caps[8];
    for (uint8_t i = 0; i < 8; ++i) caps[i] = phy_i2c_readReg(0x67, 1, 6 + i);
    transaction_give();
    printf("PREDEMOD_DCO b2k1=%u b3k1=%u b2k2=%u b3k2=%u\n", dco[0], dco[1], dco[2], dco[3]);
    printf("PREDEMOD_FILTER_BASE valid=%u regs6_13=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x fixed_code=%d\n",
           s_filter_base_valid, s_filter_base[0], s_filter_base[1], s_filter_base[2],
           s_filter_base[3], s_filter_base[4], s_filter_base[5], s_filter_base[6],
           s_filter_base[7], s_filter_code);
    printf("PREDEMOD_FILTER regs6_13=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x "
           "cal_f5_fc=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x 11p=%02x/%02x\n",
           caps[0], caps[1], caps[2], caps[3], caps[4], caps[5], caps[6], caps[7],
           phy_param[0xF5], phy_param[0xF6], phy_param[0xF7], phy_param[0xF8],
           phy_param[0xF9], phy_param[0xFA], phy_param[0xFB], phy_param[0xFC],
           phy_param[0x26], phy_param[0x27]);
#else
    printf("PREDEMOD_PHY pbus_and_filter=unverified_PHY_binary\n");
#endif
}

esp_err_t phy_rx_lab_run_dco_probe(bool (*measure)(int dc[2]),
                                   void (*observe)(const char *stage))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)measure; (void)observe;
    printf("DCO refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!measure || !observe || rf_native_agc_active()) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("DCO_AB");
    uint16_t live[4][2];
    for (unsigned b = 0; b < 4; ++b)
        for (unsigned k = 0; k < 2; ++k) live[b][k] = phy_pbus_rd(b, k + 1);
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    int before[2] = {0, 0};
    bool debug = false;
    if (measure(before)) {
        observe("BASELINE");
        debug = true;
        /* Debug mode stops the work-mode table replay; re-assert every live
         * word first so RF/BB gain and the other DC pair stay as they were. */
        phy_pbus_debugmode();
        for (unsigned b = 0; b < 4; ++b)
            for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
        int base[2] = {live[2][1], live[3][1]}, cur[2] = {base[0], base[1]};
        int best[2] = {base[0], base[1]}, best_dc[2] = {before[0], before[1]};
        int di = base[0] > 511 - DCO_PROBE ? -DCO_PROBE : DCO_PROBE;
        int dq = base[1] > 511 - DCO_PROBE ? -DCO_PROBE : DCO_PROBE;
        int mi[2], mq[2];
        dco_apply(base[0] + di, base[1]);
        bool ok = measure(mi);
        dco_apply(base[0], base[1] + dq);
        ok = ok && measure(mq);
        dco_apply(base[0], base[1]);
        float j[4] = {(float)(mi[0] - before[0]) / di, (float)(mq[0] - before[0]) / dq,
                      (float)(mi[1] - before[1]) / di, (float)(mq[1] - before[1]) / dq};
        printf("DCO baseline_mcells=%d/%d base=%d/%d jacobian_mcells_per_code=%.1f,%.1f,%.1f,%.1f\n",
               before[0], before[1], base[0], base[1], (double)j[0], (double)j[1],
               (double)j[2], (double)j[3]);
        int err[2] = {before[0], before[1]};
        for (unsigned it = 0; ok && it < 4 && dco_cost(best_dc) > DCO_DONE_MCELLS * DCO_DONE_MCELLS; ++it) {
            int sa, sb;
            if (!predemod_dco_step(j, (float)err[0], (float)err[1], DCO_TRIAL, &sa, &sb)) {
                printf("DCO response=ill_conditioned\n");
                break;
            }
            cur[0] = dco_clamp(cur[0] + sa, base[0]);
            cur[1] = dco_clamp(cur[1] + sb, base[1]);
            dco_apply(cur[0], cur[1]);
            if (!measure(err)) { ok = false; break; }
            printf("DCO iteration=%u codes=%d/%d mcells=%d/%d\n", it + 1, cur[0], cur[1], err[0], err[1]);
            if (dco_cost(err) < dco_cost(best_dc)) {
                best[0] = cur[0]; best[1] = cur[1];
                best_dc[0] = err[0]; best_dc[1] = err[1];
            }
        }
        if (ok) {
            dco_apply(best[0], best[1]);
            s_dco_codes[0] = best[0]; s_dco_codes[1] = best[1];
            s_dco_vendor[0] = base[0]; s_dco_vendor[1] = base[1];
            s_dco_valid = true;
            printf("DCO corrected codes=%d/%d mcells=%d/%d before=%d/%d persistent=0\n",
                   best[0], best[1], best_dc[0], best_dc[1], before[0], before[1]);
            observe("CORRECTED");
            result = ESP_OK;
        }
    }
    /* Exact rollback: every saved word, then hand PBUS back to work mode. */
    if (debug) {
        for (unsigned b = 0; b < 4; ++b)
            for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
        phy_pbus_workmode();
    }
    bool restored = true;
    for (unsigned b = 0; b < 4; ++b)
        for (unsigned k = 0; k < 2; ++k) {
            uint16_t now = phy_pbus_rd(b, k + 1);
            if (now != live[b][k]) {
                /* Board 2026-10-06: work mode does not replay the table at
                 * once; report which words differ instead of rebooting. */
                printf("DCO restore_diff block=%u bank=%u saved=%u now=%u\n", b, k + 1, live[b][k], now);
                restored = false;
            }
        }
    printf("DCO restore_verified=%u\n", restored);
    if (restored) observe("RESTORED");
    phy_rx_lab_end();
    return restored ? result : ESP_FAIL;
#endif
}

/* Hold the last DCO search result (on) or release it (off). Board
 * 2026-10-06: the forced pair exists only in PBUS debug mode; returning to
 * work mode does not replay the current gain's row (block 0 bank 2 fell
 * 383 -> 263 and the IQ went dead) until the next gain write. So "on" stays
 * in debug mode with every live word re-asserted plus the corrected pair,
 * and "off" returns to work mode; the caller then re-writes the gain. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
static bool s_dco_held;
#endif
esp_err_t phy_rx_lab_dco_set(bool on)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (on && !s_dco_valid) { printf("DCO_SET refused=no_search_result ('#' first, VTX off)\n"); return ESP_ERR_INVALID_STATE; }
    if (rf_native_agc_active()) return ESP_ERR_INVALID_STATE;
    transaction_take();
    if (on && !s_dco_held) {
        uint16_t live[4][2];
        for (unsigned b = 0; b < 4; ++b)
            for (unsigned k = 0; k < 2; ++k) live[b][k] = phy_pbus_rd(b, k + 1);
        phy_pbus_debugmode();
        for (unsigned b = 0; b < 4; ++b)
            for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
        dco_apply(s_dco_codes[0], s_dco_codes[1]);
        s_dco_held = true;
    } else if (!on && s_dco_held) {
        phy_pbus_workmode();
        s_dco_held = false;
    }
    uint16_t i = phy_pbus_rd(2, 2), q = phy_pbus_rd(3, 2);
    transaction_give();
    printf("DCO_SET on=%u held=%u codes=%d/%d readback=%u/%u\n", on, s_dco_held,
           s_dco_codes[0], s_dco_codes[1], i, q);
    return ESP_OK;
#endif
}

/* Range-edge DC correction (rf.c releases before every gain write and PHY
 * restore; video.c searches and holds at maximum gain). */
bool phy_rx_lab_dco_release(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    return false;
#else
    if (!s_dco_held) return false;
    transaction_take();
    phy_pbus_workmode();
    s_dco_held = false;
    transaction_give();
    return true;
#endif
}
bool phy_rx_lab_dco_held(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    return false;
#else
    return s_dco_held;
#endif
}
bool phy_rx_lab_dco_valid(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    return false;
#else
    return s_dco_valid;
#endif
}
void phy_rx_lab_dco_invalidate(void)
{
#ifdef C5VRX_PHY_RX_LAB_PINNED
    s_dco_valid = false;
#endif
}

esp_err_t phy_rx_lab_run_filter_sweep(void (*observe)(const char *stage, int offset))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe;
    printf("FILTER refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    static const int offsets[] = {4, 8, 16, 24, 60};
    if (!observe || rf_native_agc_active()) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("FILTER_SWEEP");
    uint8_t saved[8];
    for (uint8_t i = 0; i < 8; ++i) saved[i] = phy_i2c_readReg(0x67, 1, 6 + i);
    observe("BASELINE", 0);
    bool applied = true;
    for (unsigned n = 0; applied && n < sizeof(offsets) / sizeof(offsets[0]); ++n) {
        for (uint8_t i = 0; i < 8; ++i) {
            /* Offsets are relative to the calibrated baseline, not to a
             * fixed-BW offset that may already be applied. */
            uint8_t code = predemod_filter_code(s_filter_base_valid ? s_filter_base[i] : saved[i],
                                                offsets[n]);
            phy_i2c_writeReg(0x67, 1, 6 + i, code);
            if (phy_i2c_readReg(0x67, 1, 6 + i) != code) applied = false;
        }
        if (applied) observe("NARROW", offsets[n]);
    }
    bool restored = true;
    for (uint8_t i = 0; i < 8; ++i) phy_i2c_writeReg(0x67, 1, 6 + i, saved[i]);
    for (uint8_t i = 0; i < 8; ++i) restored = restored && phy_i2c_readReg(0x67, 1, 6 + i) == saved[i];
    printf("FILTER apply_verified=%u restore_verified=%u persistent=0\n", applied, restored);
    if (restored) observe("RESTORED", 0);
    phy_rx_lab_end();
    return restored ? (applied ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : ESP_FAIL;
#endif
}

/* Fixed analog bandwidth, after ESPARGOS esp-sdr (GPL-3.0, commit ac627b0b,
 * main/families/c5_c6_c61/receiver.c): the C5 RX0 capacitor DAC is BBTOP 0x67
 * regs 6/7; an absolute 6-bit code replaces the low bits, the upper bits and
 * regs 8..13 keep the PHY calibration. The code is re-applied from the
 * boot-captured bytes, so a retune that did or did not rewrite them gives the
 * same result. PHY_RX_LAB_FILTER_CALIBRATED restores the calibrated bytes. */
bool phy_rx_lab_filter_capture_base(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    return false;
#else
    transaction_take();
    if (!s_filter_base_valid) {
        for (uint8_t i = 0; i < 8; ++i) s_filter_base[i] = phy_i2c_readReg(0x67, 1, 6 + i);
        s_filter_base_valid = true;
        s_filter_code = PHY_RX_LAB_FILTER_CALIBRATED;
    }
    transaction_give();
    return true;
#endif
}

bool phy_rx_lab_filter_set_code(int code)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)code;
    return false;
#else
    if (code != PHY_RX_LAB_FILTER_CALIBRATED && (code < 0 || code > 63)) return false;
    transaction_take();
    bool ok = s_filter_base_valid;
    for (uint8_t i = 0; ok && i < 2; ++i) {
        uint8_t value = code == PHY_RX_LAB_FILTER_CALIBRATED ? s_filter_base[i] :
                        (uint8_t)((s_filter_base[i] & ~63u) | (unsigned)code);
        phy_i2c_writeReg(0x67, 1, 6 + i, value);
        ok = phy_i2c_readReg(0x67, 1, 6 + i) == value;
    }
    if (ok) s_filter_code = code;
    else if (s_filter_base_valid) {
        for (uint8_t i = 0; i < 2; ++i) phy_i2c_writeReg(0x67, 1, 6 + i, s_filter_base[i]);
        s_filter_code = PHY_RX_LAB_FILTER_CALIBRATED;
    }
    transaction_give();
    return ok;
#endif
}

/* Second filter stage (2026-10-04): regs 8..13 relative to the calibrated
 * bytes, for a steeper skirt at the same -3 dB width. 0 restores them. */
static int s_filter_skirt;
bool phy_rx_lab_filter_set_skirt(int offset)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)offset;
    return false;
#else
    if (offset < 0 || offset > 60) return false;
    transaction_take();
    bool ok = s_filter_base_valid;
    for (uint8_t i = 2; ok && i < 8; ++i) {
        uint8_t value = offset ? predemod_filter_code(s_filter_base[i], offset) : s_filter_base[i];
        phy_i2c_writeReg(0x67, 1, 6 + i, value);
        ok = phy_i2c_readReg(0x67, 1, 6 + i) == value;
    }
    if (ok) s_filter_skirt = offset;
    else if (s_filter_base_valid) {
        for (uint8_t i = 2; i < 8; ++i) phy_i2c_writeReg(0x67, 1, 6 + i, s_filter_base[i]);
        s_filter_skirt = 0;
    }
    transaction_give();
    return ok;
#endif
}
int phy_rx_lab_filter_skirt(void) { return s_filter_skirt; }

/* BW20-wide lab: regs 6/7 low six bits on top of whatever the current channel
 * setup wrote (another PHY channel mode may calibrate other upper bits).
 * Bookkeeping is untouched: the caller ends with a normal retune, whose
 * restore re-applies the stored fixed-BW code. */
bool phy_rx_lab_filter_poke_live(int code)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)code;
    return false;
#else
    if (code < 0 || code > 63) return false;
    transaction_take();
    bool ok = true;
    for (uint8_t i = 0; ok && i < 2; ++i) {
        uint8_t value = (uint8_t)((phy_i2c_readReg(0x67, 1, 6 + i) & ~63u) | (unsigned)code);
        phy_i2c_writeReg(0x67, 1, 6 + i, value);
        ok = phy_i2c_readReg(0x67, 1, 6 + i) == value;
    }
    transaction_give();
    return ok;
#endif
}

int phy_rx_lab_filter_code(void) { return s_filter_code; }

int phy_rx_lab_filter_calibrated_code(void)
{
#ifdef C5VRX_PHY_RX_LAB_PINNED
    return s_filter_base_valid ? (int)(s_filter_base[0] & 63u) : -1;
#else
    return -1;
#endif
}
