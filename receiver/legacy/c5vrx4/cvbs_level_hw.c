/* C5VRX by Twotoz and contributors. Experimental LUT16 access lab.
 * Register source: ESP-IDF v6.0.2 ESP32-C5 bitscrambler_ll/struct headers.
 * This never uses load_lut (which changes the live width) or RUN/HALT/reset.
 * Live RAM arbitration and FIFO continuity still require physical acceptance.
 */
#include "cvbs_level_hw.h"
#include "cvbs_level.h"
#include "c5vrx4.h"
#include "predemod.h"
#include "soc/bitscrambler_struct.h"
#include "hal/bitscrambler_ll.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
static SemaphoreHandle_t access_lock;
void c5v4_level_hw_lock(void)
{
    if (!access_lock) access_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(access_lock ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreTakeRecursive(access_lock, portMAX_DELAY);
}
void c5v4_level_hw_unlock(void) { xSemaphoreGiveRecursive(access_lock); }
static c5v4_level_t servo;
static uint16_t original[1024];
static bool ready;
static bool blocked;
/* Stopped-engine addressing probe passed: LUT16 host writes are mapped.
 * Kept separately from the level servo so DC recentring can use it too. */
static bool lut_verified, decoder_blocked;
static uint16_t decoder_pristine[256];
static int decoder_dc[2];
static uint32_t decoder_updates, decoder_faults;
static uint32_t writes, faults;
static uint16_t read_entry(unsigned index)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    return (uint16_t)BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut;
}
static void write_entry(unsigned index, uint16_t value)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut = value;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
}
/* Live write with read-back. The TX engine reads the LUT while the host
 * selects an index through the shared lut_idx register, so one read-back
 * can disagree. Board 2026-10-06: a single mismatch had blocked DC
 * recentring for good (blocked=1, updates=0, faults=1) while it measured
 * -2.45/+4.4 cells. Retry the same word; block only if it keeps failing. */
static uint32_t retries;
static bool write_verified(unsigned index, uint16_t value)
{
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        write_entry(index, value);
        uint16_t got = read_entry(index);
        if (got == value) {
            if (attempt) ++retries;
            return true;
        }
        if (retries + faults + decoder_faults < 8u)
            printf("C5V4_LUT readback_mismatch index=%u want=0x%04x got=0x%04x attempt=%u\n",
                   index, value, got, attempt + 1u);
    }
    return false;
}
void c5v4_level_hw_stop(void)
{
    c5v4_level_hw_lock(); ready = false; c5v4_level_hw_unlock();
}
void c5v4_level_hw_prepare(void)
{
    ready = lut_verified = false;
    decoder_dc[0] = decoder_dc[1] = 0; /* every program load is pristine */
    c5v4_level_init(&servo);
    if (bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) != 1) return;
    /* The headers disagree on whether host address units follow LUT width.
     * A stopped-engine differential probe verifies all 1024 halfword views,
     * including decoder neighbours, before any live update is allowed. */
    for (unsigned i = 0; i < 1024; ++i) original[i] = read_entry(i);
    unsigned probe = 32;
    uint16_t changed = original[probe] ^ 1u;
    write_entry(probe, changed);
    bool ok = true;
    for (unsigned i = 0; i < 1024; ++i)
        if (read_entry(i) != (i == probe ? changed : original[i])) ok = false;
    write_entry(probe, original[probe]);
    for (unsigned i = 0; i < 1024; ++i)
        if (read_entry(i) != original[i]) ok = false;
    lut_verified = ok;
    for (unsigned i = 0; i < 256; ++i) decoder_pristine[i] = original[256 + i];
    ready = ok && !blocked && c5vrx4_level_enabled() && !c5vrx4_cvbs_legacy_enabled();
    if (ready) {
        /* Seed from the loaded even plane (M may select CVBS150). */
        uint8_t loaded[256];
        for (unsigned i = 0; i < 256; ++i) loaded[i] = (uint8_t)(original[i] & 63u);
        c5v4_level_seed(&servo, loaded);
    }
    if (!ok) ++faults;
    printf("C5V4_LEVEL startup_probe=%s level=%u live_arbitration=unproven\n",
           ok ? "pass" : "refused", ready);
}
bool c5v4_level_hw_lut_verified(void)
{ c5v4_level_hw_lock(); bool value=lut_verified; c5v4_level_hw_unlock(); return value; }

/* Digital DC recentring: rewrite both odd (decoder) banks for a new I/Q
 * centre in milli-cells; (0,0) restores the pristine generated table. STATIC
 * decode only. Live LUT arbitration is the same unproven gate as the level
 * servo; one sample may decode with a mixed table during the rewrite. */
bool c5v4_decoder_recenter(int di, int dq)
{
    uint16_t words[256];
    bool pristine = !di && !dq;
    for (unsigned raw = 0; raw < 256; ++raw)
        words[raw] = pristine ? 0u : predemod_decoder_word((uint8_t)raw, di, dq);
    c5v4_level_hw_lock();
    /* The AGC-mask program decodes Q3 and keeps the hold identity in bank 3. */
    bool ok = lut_verified && !decoder_blocked && !c5vrx4_history_enabled() &&
              !c5vrx4_agc_mask_active() &&
              bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) == 1;
    for (unsigned raw = 0; ok && raw < 256; ++raw) {
        uint16_t word = pristine ? decoder_pristine[raw] : words[raw];
        for (unsigned bank = 1; ok && bank < 4; bank += 2) {
            unsigned index = bank * 256 + raw;
            if (read_entry(index) == word) continue;
            if (!write_verified(index, word)) { ok = false; decoder_blocked = true; ++decoder_faults; }
        }
    }
    if (ok) { decoder_dc[0] = di; decoder_dc[1] = dq; ++decoder_updates; }
    c5v4_level_hw_unlock();
    return ok;
}
void c5v4_decoder_dc(int dc[2])
{
    c5v4_level_hw_lock();
    /* A program reload (menu exit, retune restart) restores the pristine
     * table without prepare: never believe a correction the LUT lost. */
    static const uint8_t sentinel[] = {0x00, 0x0f, 0xf0, 0xff};
    if ((decoder_dc[0] || decoder_dc[1]) && lut_verified) {
        bool pristine = true;
        for (unsigned k = 0; k < sizeof(sentinel); ++k)
            if (read_entry(256 + sentinel[k]) != decoder_pristine[sentinel[k]]) pristine = false;
        if (pristine) decoder_dc[0] = decoder_dc[1] = 0;
    }
    dc[0] = decoder_dc[0]; dc[1] = decoder_dc[1];
    c5v4_level_hw_unlock();
}
void c5v4_decoder_print(void)
{
    c5v4_level_hw_lock();
    printf("C5V4_DC_RECENTER enabled=%u lut_verified=%u blocked=%u applied_mcells=%d/%d "
           "updates=%lu faults=%lu lut_retries=%lu decode=%s pre_q4_correction=0\n",
           c5vrx4_dc_recenter_enabled(), lut_verified, decoder_blocked, decoder_dc[0], decoder_dc[1],
           (unsigned long)decoder_updates, (unsigned long)decoder_faults, (unsigned long)retries,
           c5vrx4_history_enabled() ? "history_refused" : "static");
    c5v4_level_hw_unlock();
}
bool c5v4_level_hw_ready(void)
{ c5v4_level_hw_lock(); bool value=ready; c5v4_level_hw_unlock(); return value; }
unsigned c5v4_level_hw_period(uint32_t context, uint64_t now)
{
    c5v4_level_hw_lock();
    unsigned period = c5v4_level_period(&servo, context, now);
    c5v4_level_hw_unlock();
    return period;
}
void c5v4_level_hw_observe(const c5v4_cvbs_stats_t *stats, bool fresh,
                         uint32_t context, uint64_t now)
{
    c5v4_level_hw_lock();
    if (!ready || !c5vrx4_level_enabled()) goto done;
    if (bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) != 1) {
        ready = false; ++faults; goto done;
    }
    if (!c5v4_level_observe(&servo, stats, fresh, context, now)) goto done;
    /* Board 2026-10-06: while the TX engine runs, a LUT read-back returned
     * random words, so a live write can land on another index. The servo
     * keeps measuring; it no longer writes until a stopped-engine (or
     * double-buffered) update path exists. */
    {
        static bool reported;
        if (!reported) {
            reported = true;
            printf("C5V4_LEVEL live_lut_writes=refused reason=unreliable_live_lut_access\n");
        }
        goto done;
    }
    /* Write the matching planes next to one another, shortening the time
     * each single entry disagrees; still not an atomic hardware bank swap. */
    for (unsigned i = 0; i < 256; ++i) {
        for (unsigned bank = 0; bank < 4; bank += 2) {
            unsigned index = bank * 256 + i;
            uint16_t next = c5v4_level_word(original[index], servo.codes[i]);
            if (next != original[index]) {
                if (!write_verified(index, next)) {
                    ready = false; blocked = true; ++faults; goto done;
                }
                original[index] = next; ++writes;
            }
        }
    }
done:
    c5v4_level_hw_unlock();
}
void c5v4_level_hw_invalidate(void)
{
    c5v4_level_hw_lock(); servo.good = 0; c5v4_level_hw_unlock();
}
void c5v4_level_hw_transport_fault(void)
{
    c5v4_level_hw_lock();
    if (ready && servo.updates) { ready = false; blocked = true; ++faults; }
    c5v4_level_hw_unlock();
}
void c5v4_level_hw_print(void)
{
    c5v4_level_hw_lock();
    printf("C5V4_LEVEL requested=%u ready=%u experimental=1 updates=%lu writes=%lu faults=%lu "
           "good=%u refused=%u span_bins=%d blank_bins=%d target_sync_mv=10 target_depth_mv=%u "
           "period_us=20000 recovery_period_us=5000 recovery_us=100000 slew_uv=32000 blocked=%u loss=hold sync_regeneration=0 atomic_update=0\n",
           c5vrx4_level_enabled(), ready, (unsigned long)servo.updates,
           (unsigned long)writes, (unsigned long)faults, servo.good, servo.refusals,
           servo.span, servo.blank, servo.target_depth_mv, blocked);
    c5v4_level_hw_unlock();
}
