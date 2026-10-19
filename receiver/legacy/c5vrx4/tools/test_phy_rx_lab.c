#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
static int64_t clock_us;
static bool native;
unsigned char phy_param[0x500];
int64_t esp_timer_get_time(void) { return clock_us; }
bool rf_native_agc_active(void) { return native; }
bool rf_get_analog_bandwidth(void) { return true; }
uint16_t rf_get_frequency_mhz(void) { return 5865; }
int rf_get_frequency_offset_khz(void) { return 0; }
static uint8_t analog_regs[32];
static bool fail_restore, fail_apply;
static uint8_t adc_i2c = 0x5Bu; /* block 0x66 host 0 reg 4; bit 2 = !rate */
uint8_t phy_i2c_readReg(uint8_t b, uint8_t h, uint8_t r)
{
    if (b==0x66) { assert(h==0 && r==4); return adc_i2c; }
    assert(b==0x67 && h==1); return analog_regs[r];
}
#include "../main/phy_rx_lab.c"
void phy_i2c_writeReg(uint8_t b,uint8_t h,uint8_t r,uint8_t v)
{ assert(b==0x67 && h==1); if (!fail_restore) analog_regs[r]=v; }
void phy_11p_set(uint8_t enable,uint8_t mode)
{
    assert(enable==1 && mode==0);
    if (fail_apply) return;
    phy_param[0x26]=enable; phy_param[0x27]=mode;
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned i=0;i<4;++i) {
        patch_t x=s_11p_fields[i]; REG(x.addr)=(REG(x.addr)&~x.mask)|x.value;
    }
#endif
    for (unsigned i=6;i<=13;++i) analog_regs[i]=60;
}
void vTaskDelay(unsigned ticks) { clock_us += (int64_t)ticks * 1000; }
#ifdef C5VRX_PHY_RX_LAB_PINNED
static unsigned sig_reads, sig_enables, sig_stages, track_calls, track_stages;
void phy_check_sigrssi_en(uint8_t enable)
{
    assert(enable==1); ++sig_enables;
    for (unsigned i=0;i<SIGRSSI_WORDS;++i) REG(0x600A7000u+s_sigrssi_words[i])^=0x0000FFFFu;
}
int8_t phy_get_sigrssi(void) { return (int8_t)(-95 + (int)((sig_reads++ * 7u) % 51u)); }
void phy_param_track_tot(uint8_t wifi, uint8_t bt) { assert(wifi==1 && bt==0); ++track_calls; }
/* Vendor ABI as read from the pinned archive. */
static bool dfilt_drop_i2c_restore;
static unsigned dfilt_rate_calls, dfilt_mode_calls, dfilt_stages, dfilt_modes_seen;
void phy_adc_rate_set(uint32_t rate)
{
    ++dfilt_rate_calls;
    if (!(dfilt_drop_i2c_restore && dfilt_rate_calls > 1))
        adc_i2c = (uint8_t)((adc_i2c & ~4u) | (rate ? 0u : 4u));
    REG(0x600A0448) = (REG(0x600A0448) & ~3u) | (rate & 1u) | ((rate & 1u) << 1);
}
void phy_rx_filter_mode(uint32_t mode)
{
    ++dfilt_mode_calls;
    REG(0x600A0430) = (REG(0x600A0430) & ~(0xFu << 18)) | ((mode & 0xFu) << 18);
}
static void dfilt_observe(const char *stage, int arg)
{
    assert(phy_rx_lab_busy());
    unsigned mode = (REG(0x600A0430) >> 18) & 0xFu, rate = REG(0x600A0448) & 1u;
    if (!strcmp(stage, "MODE")) {
        assert(arg >= 0 && arg < 16 && mode == (unsigned)arg && rate == 1u);
        assert((REG(0x600A0430) & ~(0xFu << 18)) == (0x12345678u & ~(0xFu << 18)));
        dfilt_modes_seen |= 1u << arg;
    } else if (!strcmp(stage, "ADC_ALT")) {
        assert(arg == 0 && rate == 0u && mode == 8u && (adc_i2c & 4u));
    } else if (!strcmp(stage, "ADC_ALT_VENDOR_MODE")) {
        assert(arg == 0 && rate == 0u && mode == 0u);
    } else {
        assert(!strcmp(stage, "BASELINE") || !strcmp(stage, "MODE_RESTORED") || !strcmp(stage, "RESTORED"));
        assert(arg == 8 && mode == 8u && rate == 1u);
    }
    ++dfilt_stages;
}
static void sig_observe(const char *stage)
{
    assert(phy_rx_lab_busy()); ++sig_stages;
    bool on = !strcmp(stage,"SIGRSSI_ON");
    assert(on == (sig_enables==1 && sig_stages==2));
}
static void track_observe(const char *stage)
{
    assert(phy_rx_lab_busy()); ++track_stages;
    assert(!strcmp(stage, track_calls ? "TRACKED" : "BASELINE"));
}
#endif
static unsigned native_holds, native_releases, native_observations;
static bool fail_native_hold, fail_native_release, interfere_native;
void phy_disable_agc(void)
{
    ++native_holds;
    if (!fail_native_hold) REG(0x600A7030)|=PHYBIT(29);
}
void phy_enable_agc(void)
{
    ++native_releases;
    if (!fail_native_release) {
        REG(0x600A7030)&=~PHYBIT(29);
        REG(0x600A702C)|=PHYBIT(23);
        REG(0x600A702C)&=~PHYBIT(23);
    }
}
static void native_observe(const char *stage,unsigned cycle)
{
    assert(phy_rx_lab_busy()); ++native_observations;
    if (!strcmp(stage,"HOLD")) {
        assert(cycle && (REG(0x600A7030)&PHYBIT(29)));
        assert(!(REG(0x600A702C)&PHYBIT(23)));
        REG(0x600A7030)^=1u; /* unrelated bit must survive */
        if (interfere_native) REG(0x600A7030)&=~PHYBIT(29);
    } else assert(!(REG(0x600A7030)&PHYBIT(29)));
    clock_us+=40000;
}
static unsigned pbus[4][3];
static bool pbus_debug, corrupt_workmode, fail_measure;
uint16_t phy_pbus_rd(uint32_t block, uint32_t bank) { return (uint16_t)pbus[block][bank]; }
void phy_pbus_force_test(uint32_t block, uint32_t bank, uint32_t value)
{
    assert(pbus_debug && block < 4 && bank >= 1 && bank <= 2 && value <= 511);
    pbus[block][bank] = value;
}
void phy_pbus_debugmode(void) { pbus_debug = true; }
void phy_pbus_workmode(void) { pbus_debug = false; if (corrupt_workmode) pbus[2][2] ^= 1u; }
/* Linear DC model with cross-coupling: block 2 bank 2 mainly I, block 3 bank 2 mainly Q. */
static bool dco_measure(int dc[2])
{
    assert(phy_rx_lab_busy());
    if (fail_measure) return false;
    int a = (int)pbus[2][2] - 200, b = (int)pbus[3][2] - 300;
    dc[0] = 1500 + 25 * a + 3 * b;
    dc[1] = -900 + 30 * b - 2 * a;
    return true;
}
static unsigned dco_stages;
static void dco_observe(const char *stage)
{
    int dc[2];
    if (fail_measure) { assert(!strcmp(stage, "RESTORED")); ++dco_stages; return; }
    assert(dco_measure(dc));
    if (!strcmp(stage, "CORRECTED")) assert(dc[0] * dc[0] + dc[1] * dc[1] <= 100 * 100 && pbus_debug);
    if (!strcmp(stage, "RESTORED")) assert(dc[0] == 1500 && dc[1] == -900 && !pbus_debug);
    ++dco_stages;
}
static unsigned filter_stages;
static void filter_observe(const char *stage, int offset)
{
    assert(phy_rx_lab_busy());
    for (unsigned r = 6; r <= 13; ++r) {
        unsigned base = 0xC0u | (r + 10u);
        unsigned code = offset ? ((r + 10u + (unsigned)offset) > 60u ? 60u : r + 10u + (unsigned)offset) : r + 10u;
        assert(analog_regs[r] == ((base & ~63u) | code));
    }
    if (!strcmp(stage, "NARROW")) assert(offset > 0); else assert(offset == 0);
    ++filter_stages;
}
static unsigned stages;
static void observe(const char *stage)
{
    assert(phy_rx_lab_busy());
    if (stages==0) assert(!strcmp(stage,"BASELINE"));
    else if (stages==1 && !fail_apply) {
        assert(!strcmp(stage,"ENABLED"));
        assert(phy_param[0x26]==1 && phy_param[0x27]==0);
        /* Preserve unrelated volatile bits despite exact owned rollback. */
        REG(0x600A7030)^=1u;
    } else assert(!strcmp(stage,"RESTORED"));
    ++stages;
}
static atomic_bool attempted, acquired;
static void *actuator_contender(void *unused)
{
    (void)unused;
    bool got=phy_rx_lab_try_actuator(phy_rx_lab_generation());
    atomic_store(&acquired,got);
    if (got) phy_rx_lab_end_actuator();
    return NULL;
}
static void *contender(void *unused)
{
    (void)unused; atomic_store(&attempted,true);
    phy_rx_lab_begin("other_task");
    atomic_store(&acquired,true); phy_rx_lab_end(); return NULL;
}
int main(void)
{
    void *memory=mmap((void *)0x600A0000, 0x10000, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0xA5, 0x10000);
    phy_rx_lab_osi_event(true);
    phy_rx_lab_begin("boot");
    phy_rx_lab_capture_vendor();
    phy_rx_lab_end();
    assert(s_enables==1 && s_disables==0 && s_generation==1);
    /* A gain/lane actuator is nonblocking, excludes vendor transactions and
     * cannot apply a decision made before a completed retune. */
    uint32_t actuator_generation=phy_rx_lab_generation();
    assert(phy_rx_lab_try_actuator(actuator_generation));
    assert(!phy_rx_lab_busy()); /* not a tune: does not reset controller */
    pthread_t actuator_thread;
    assert(!pthread_create(&actuator_thread,NULL,actuator_contender,NULL));
    assert(!pthread_join(actuator_thread,NULL));
    assert(!atomic_load(&acquired));
    phy_rx_lab_end_actuator();
    assert(phy_rx_lab_generation()==actuator_generation);
    phy_rx_lab_begin("retune_between_measure_and_write");
    assert(!phy_rx_lab_try_actuator(actuator_generation)); /* same-task nesting */
    assert(!pthread_create(&actuator_thread,NULL,actuator_contender,NULL));
    assert(!pthread_join(actuator_thread,NULL));
    assert(!atomic_load(&acquired));
    phy_rx_lab_end();
    assert(!phy_rx_lab_try_actuator(actuator_generation));
    assert(phy_rx_lab_try_actuator(phy_rx_lab_generation()));
    phy_rx_lab_end_actuator();
    uint32_t baseline=REG(0x600A0890);
    REG(0x600A0890)^=1;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==0); /* monitoring off by default */
    phy_rx_lab_toggle_monitor();
    REG(0x600A0890)=baseline;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==1);
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned n=1; n<sizeof(s_profiles)/sizeof(s_profiles[0]); ++n) {
        memset(memory, 0xA5, 0x10000);
        phy_rx_lab_next_profile();
        assert(s_profile==n);
        const profile_t *p=&s_profiles[n];
        for (unsigned i=0; i<p->count; ++i) {
            patch_t x=p->patches[i];
            assert((REG(x.addr)&x.mask)==x.value);
            /* An unrelated field changes while the lab owns its mask. */
            uint32_t outside=(~x.mask)&(x.mask+1u);
            if (!outside) outside=(~x.mask)&(0u-~x.mask);
            REG(x.addr)^=outside;
            s_saved[i]^=outside;
        }
        if (n%3==0) { clock_us+=PROFILE_US; phy_rx_lab_poll(); }
        else if (n%3==1) { phy_rx_lab_begin("retune"); phy_rx_lab_end(); }
        else phy_rx_lab_stock();
        assert(!s_profile);
        for (unsigned i=0; i<p->count; ++i) assert(REG(p->patches[i].addr)==s_saved[i]);
    }
    native=true;
    phy_rx_lab_next_profile();
    assert(!s_profile);
    native=false;
    phy_rx_lab_begin("outer");
    uint32_t generation=s_generation;
    phy_rx_lab_begin("inner");
    phy_rx_lab_next_profile();
    assert(!s_profile);
    phy_rx_lab_end();
    assert(s_generation==generation && phy_rx_lab_busy());
    phy_rx_lab_end();
    assert(s_generation==generation+1 && !phy_rx_lab_busy());
#else
    phy_rx_lab_next_profile();
    assert(!s_profile);
#endif

    /* A competing task cannot join another owner's nested transaction. */
    phy_rx_lab_begin("owner");
    pthread_t thread; assert(!pthread_create(&thread,NULL,contender,NULL));
    while (!atomic_load(&attempted)) sched_yield();
    assert(!atomic_load(&acquired));
    phy_rx_lab_end(); assert(!pthread_join(thread,NULL));
    assert(atomic_load(&acquired));
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned i=6;i<=13;++i) analog_regs[i]=i+20;
    phy_param[0x26]=7; phy_param[0x27]=9;
    uint32_t before=REG(0x600A7030), fields[4];
    for (unsigned i=0;i<4;++i) fields[i]=REG(s_11p_fields[i].addr);
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_OK);
    assert(stages==3 && !phy_rx_lab_busy());
    assert(REG(0x600A7030)==(before^1u));
    for (unsigned i=0;i<4;++i)
        assert((REG(s_11p_fields[i].addr)&s_11p_fields[i].mask)==(fields[i]&s_11p_fields[i].mask));
    assert(phy_param[0x26]==7 && phy_param[0x27]==9);
    for (unsigned i=6;i<=13;++i) assert(analog_regs[i]==i+20);
    native=true; stages=0;
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_INVALID_STATE && !stages);
    native=false;
    fail_apply=true;
    /* A failed application still restores and releases task ownership. */
    assert(phy_rx_lab_run_11p_probe(NULL)==ESP_ERR_INVALID_STATE);
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_INVALID_RESPONSE);
    assert(stages==2 && !phy_rx_lab_busy());
    fail_apply=false;
    fail_restore=true; stages=0;
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_FAIL);
    assert(stages==2 && !phy_rx_lab_busy()); fail_restore=false;
#else
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_NOT_SUPPORTED && !stages);
#endif

#ifdef C5VRX_PHY_RX_LAB_PINNED
    native=true;
    REG(0x600A7030)&=~PHYBIT(29); REG(0x600A702C)&=~PHYBIT(23);
    uint32_t ctrl=REG(0x600A7030), force=REG(0x600A702C), rfctrl=REG(0x600A705C);
    assert(phy_rx_lab_run_native_hold(100,native_observe)==ESP_OK);
    assert(native_holds==100 && native_releases==100 && native_observations==201);
    assert(REG(0x600A7030)==ctrl && REG(0x600A702C)==force && REG(0x600A705C)==rfctrl);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_OK);
    assert(REG(0x600A7030)==(ctrl^1u));
    assert(REG(0x600A702C)==force && REG(0x600A705C)==rfctrl);
    unsigned calls=native_holds;
    assert(phy_rx_lab_run_native_hold(0,native_observe)==ESP_ERR_INVALID_STATE);
    assert(phy_rx_lab_run_native_hold(1,NULL)==ESP_ERR_INVALID_STATE);
    REG(0x600A702C)|=PHYBIT(23);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    REG(0x600A702C)&=~PHYBIT(23);
    REG(0x600A7030)|=PHYBIT(29);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    REG(0x600A7030)&=~PHYBIT(29);
    native=false;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    assert(native_holds==calls); native=true;
    fail_native_hold=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_RESPONSE);
    fail_native_hold=false; interfere_native=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_RESPONSE);
    interfere_native=false; fail_native_release=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_FAIL);
    assert(!phy_rx_lab_busy());
    fail_native_release=false; phy_enable_agc(); native=false;
#else
    assert(phy_rx_lab_run_native_hold(100,native_observe)==ESP_ERR_NOT_SUPPORTED);
    assert(!native_holds && !native_releases);
#endif
#ifdef C5VRX_PHY_RX_LAB_PINNED
    /* DCO lab: measured 2x2 response, bounded correction, exact restore. */
    for (unsigned b = 0; b < 4; ++b) { pbus[b][1] = 100 + b; pbus[b][2] = 150 + b; }
    pbus[2][2] = 200; pbus[3][2] = 300;
    assert(phy_rx_lab_run_dco_probe(dco_measure, dco_observe) == ESP_OK);
    assert(dco_stages == 3 && !phy_rx_lab_busy() && !pbus_debug);
    assert(pbus[2][2] == 200 && pbus[3][2] == 300 && pbus[0][1] == 100 && pbus[1][2] == 151);
    native = true; dco_stages = 0;
    assert(phy_rx_lab_run_dco_probe(dco_measure, dco_observe) == ESP_ERR_INVALID_STATE && !dco_stages);
    native = false;
    assert(phy_rx_lab_run_dco_probe(NULL, dco_observe) == ESP_ERR_INVALID_STATE);
    fail_measure = true;
    assert(phy_rx_lab_run_dco_probe(dco_measure, dco_observe) == ESP_ERR_INVALID_RESPONSE);
    assert(!pbus_debug && !phy_rx_lab_busy());
    /* A restore that does not read back exactly reports a reboot. */
    fail_measure = false; corrupt_workmode = true; dco_stages = 0;
    assert(phy_rx_lab_run_dco_probe(dco_measure, dco_observe) == ESP_FAIL);
    assert(dco_stages == 2 && !phy_rx_lab_busy());
    corrupt_workmode = false; pbus[2][2] = 200;

    /* Filter lab: relative codes saturating at 60, exact restore. */
    for (unsigned r = 6; r <= 13; ++r) analog_regs[r] = 0xC0u | (r + 10u);
    assert(phy_rx_lab_run_filter_sweep(filter_observe) == ESP_OK);
    assert(filter_stages == 7 && !phy_rx_lab_busy());
    for (unsigned r = 6; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | (r + 10u)));
    native = true;
    assert(phy_rx_lab_run_filter_sweep(filter_observe) == ESP_ERR_INVALID_STATE);
    native = false;
    /* Writes that do not take are reported, never observed as NARROW. */
    fail_restore = true; filter_stages = 0;
    assert(phy_rx_lab_run_filter_sweep(filter_observe) == ESP_ERR_INVALID_RESPONSE);
    assert(filter_stages == 2 && !phy_rx_lab_busy()); fail_restore = false;
    /* Fixed BW (esp-sdr model): absolute code in regs 6/7 only, upper bits
     * and regs 8..13 keep the calibration; idempotent after a retune. */
    assert(!phy_rx_lab_filter_set_code(52) && phy_rx_lab_filter_calibrated_code() == -1);
    assert(phy_rx_lab_filter_capture_base() && phy_rx_lab_filter_calibrated_code() == 16);
    assert(phy_rx_lab_filter_code() == PHY_RX_LAB_FILTER_CALIBRATED);
    assert(phy_rx_lab_filter_set_code(52) && phy_rx_lab_filter_code() == 52);
    assert(analog_regs[6] == (0xC0u | 52u) && analog_regs[7] == (0xC0u | 52u));
    for (unsigned r = 8; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | (r + 10u)));
    assert(phy_rx_lab_filter_capture_base()); /* second capture keeps the base */
    assert(phy_rx_lab_filter_calibrated_code() == 16);
    analog_regs[6] = 0xC0u | 16u; analog_regs[7] = 0xC0u | 17u; /* retune rewrote them */
    assert(phy_rx_lab_filter_set_code(52));
    assert(analog_regs[6] == (0xC0u | 52u) && analog_regs[7] == (0xC0u | 52u));
    /* The sweep stays relative to the calibrated base, then restores code 52. */
    filter_stages = 0;
    analog_regs[6] = 0xC0u | 16u; analog_regs[7] = 0xC0u | 17u;
    assert(phy_rx_lab_run_filter_sweep(filter_observe) == ESP_OK && filter_stages == 7);
    assert(phy_rx_lab_filter_set_code(52));
    assert(!phy_rx_lab_filter_set_code(-2) && !phy_rx_lab_filter_set_code(64));
    assert(phy_rx_lab_filter_code() == 52);
    /* A write that does not take falls back to the calibrated bytes. */
    fail_restore = true;
    assert(!phy_rx_lab_filter_set_code(8));
    fail_restore = false;
    assert(phy_rx_lab_filter_code() == PHY_RX_LAB_FILTER_CALIBRATED);
    assert(phy_rx_lab_filter_set_code(PHY_RX_LAB_FILTER_CALIBRATED));
    for (unsigned r = 6; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | (r + 10u)));
    /* Second stage: regs 8..13 relative to the calibrated bytes, regs 6/7
     * untouched, saturating at 60; 0 restores the calibration exactly. */
    assert(phy_rx_lab_filter_set_code(52) && phy_rx_lab_filter_set_skirt(16));
    assert(phy_rx_lab_filter_skirt() == 16);
    assert(analog_regs[6] == (0xC0u | 52u) && analog_regs[7] == (0xC0u | 52u));
    for (unsigned r = 8; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | (r + 26u)));
    assert(phy_rx_lab_filter_set_skirt(60));
    for (unsigned r = 8; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | 60u));
    assert(!phy_rx_lab_filter_set_skirt(-1) && !phy_rx_lab_filter_set_skirt(61));
    assert(phy_rx_lab_filter_skirt() == 60);
    fail_restore = true;
    assert(!phy_rx_lab_filter_set_skirt(8) && phy_rx_lab_filter_skirt() == 0);
    fail_restore = false;
    assert(phy_rx_lab_filter_set_skirt(0));
    for (unsigned r = 8; r <= 13; ++r) assert(analog_regs[r] == (0xC0u | (r + 10u)));
    assert(phy_rx_lab_filter_set_code(PHY_RX_LAB_FILTER_CALIBRATED));
    /* Live poke keeps the current upper bits (another channel mode). */
    analog_regs[6] = 0x80u | 5u; analog_regs[7] = 0x40u | 9u;
    assert(phy_rx_lab_filter_poke_live(0));
    assert(analog_regs[6] == 0x80u && analog_regs[7] == 0x40u);
    assert(!phy_rx_lab_filter_poke_live(64));
    fail_restore = true; assert(!phy_rx_lab_filter_poke_live(8)); fail_restore = false;
    assert(phy_rx_lab_filter_set_code(PHY_RX_LAB_FILTER_CALIBRATED));
    assert(!phy_rx_lab_busy());
    phy_rx_lab_predemod_status();

    /* sigRSSI lab: every AGC word the enable rewrites is restored exactly. */
    for (unsigned i=0;i<SIGRSSI_WORDS;++i) REG(0x600A7000u+s_sigrssi_words[i])=0x01110000u+i;
    uint32_t sig_words[SIGRSSI_WORDS];
    for (unsigned i=0;i<SIGRSSI_WORDS;++i) sig_words[i]=REG(0x600A7000u+s_sigrssi_words[i]);
    phy_rx_lab_rssi_stats_t st;
    assert(phy_rx_lab_run_sigrssi_probe(sig_observe,&st)==ESP_OK);
    assert(sig_enables==1 && sig_stages==3 && !phy_rx_lab_busy());
    for (unsigned i=0;i<SIGRSSI_WORDS;++i) assert(REG(0x600A7000u+s_sigrssi_words[i])==sig_words[i]);
    assert(st.samples==1000 && st.min_dbm==-95 && st.max_dbm==-45);
    assert(st.p10_dbm<=st.p50_dbm && st.p50_dbm<=st.p90_dbm && st.p10_dbm>=-95 && st.p90_dbm<=-45);
    assert(st.mean_dbm_x10>=-950 && st.mean_dbm_x10<=-450);
    /* Refused while the BB-AGC gate is held (native hold / pacing). */
    REG(0x600A7030)|=PHYBIT(29);
    assert(phy_rx_lab_run_sigrssi_probe(sig_observe,&st)==ESP_ERR_INVALID_STATE && sig_enables==1);
    REG(0x600A7030)&=~PHYBIT(29);
    assert(phy_rx_lab_run_sigrssi_probe(NULL,&st)==ESP_ERR_INVALID_STATE);
    /* Temperature tracking lab: one call between two observations. */
    assert(phy_rx_lab_run_track_probe(track_observe)==ESP_OK);
    assert(track_calls==1 && track_stages==2 && !phy_rx_lab_busy());
    /* Digital filter / ADC-rate lab, vendor state above 5830 MHz: rate 1,
     * mode 8. All 16 modes, the other rate, then an exact restore. */
    REG(0x600A0430) = (0x12345678u & ~(0xFu << 18)) | (8u << 18);
    REG(0x600A0448) = 0xA5A50003u;
    adc_i2c = 0x5Bu & ~4u;
    const uint32_t dfilt_word = REG(0x600A0430);
    assert(phy_rx_lab_run_dfilt_probe(dfilt_observe) == ESP_OK);
    assert(dfilt_modes_seen == 0xFFFFu && dfilt_stages == 16u + 5u && !phy_rx_lab_busy());
    assert(REG(0x600A0430) == dfilt_word && REG(0x600A0448) == 0xA5A50003u && adc_i2c == (0x5Bu & ~4u));
    assert(dfilt_rate_calls == 2u && dfilt_mode_calls == 1u);
    /* The analog half of the ADC setter not returning: restore unverified. */
    dfilt_rate_calls = 0; dfilt_drop_i2c_restore = true;
    assert(phy_rx_lab_run_dfilt_probe(dfilt_observe) == ESP_FAIL && !phy_rx_lab_busy());
    dfilt_drop_i2c_restore = false; adc_i2c = 0x5Bu & ~4u;
    assert(phy_rx_lab_run_dfilt_probe(NULL) == ESP_ERR_INVALID_STATE);
#else
    assert(phy_rx_lab_run_dfilt_probe(NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(phy_rx_lab_run_sigrssi_probe(NULL,NULL)==ESP_ERR_NOT_SUPPORTED);
    assert(phy_rx_lab_run_track_probe(NULL)==ESP_ERR_NOT_SUPPORTED);
    assert(!phy_rx_lab_filter_capture_base() && !phy_rx_lab_filter_set_code(52));
    assert(!phy_rx_lab_filter_set_skirt(8) && phy_rx_lab_filter_skirt() == 0);
    assert(!phy_rx_lab_filter_poke_live(0));
    assert(phy_rx_lab_filter_calibrated_code() == -1);
    assert(phy_rx_lab_run_dco_probe(dco_measure, dco_observe) == ESP_ERR_NOT_SUPPORTED);
    assert(phy_rx_lab_run_filter_sweep(filter_observe) == ESP_ERR_NOT_SUPPORTED);
    phy_rx_lab_predemod_status();
#endif
    for (unsigned i=0; i<100; ++i) { ++clock_us; phy_rx_lab_osi_event(false); }
    assert(s_disables==100);
    assert(s_event_total>EVENT_CAP);
    phy_rx_lab_mark();
    munmap(memory,0x10000);
    puts("PHY RX lab lifecycle tests passed");
}
