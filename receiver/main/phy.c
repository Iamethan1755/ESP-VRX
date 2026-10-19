/* C5VRX-5 by Twotoz and the C5VRX contributors: owned PHY access, from
 * legacy/c5vrx4/main/phy_rx_lab.c (pinned C5 PHY investigation #150-153, #165). */
#include "phy.h"
#include "dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Recursive task ownership for nested channel/filter restores; the short
 * spinlock only protects the counters, never vendor I2C. */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static uint32_t s_generation, s_depth;

static void lock_take(void)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}
static void lock_give(void) { xSemaphoreGiveRecursive(s_lock); }

void phy_begin(void)
{
    lock_take();
    portENTER_CRITICAL(&s_mux);
    ++s_depth;
    portEXIT_CRITICAL(&s_mux);
}

void phy_end(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_depth && --s_depth == 0) ++s_generation;
    portEXIT_CRITICAL(&s_mux);
    lock_give();
}

uint32_t phy_generation(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t g = s_generation;
    portEXIT_CRITICAL(&s_mux);
    return g;
}

bool phy_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy = s_depth != 0;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

bool phy_try_actuator(uint32_t expected_generation)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    portEXIT_CRITICAL(&s_mux);
    if (xSemaphoreTakeRecursive(s_lock, 0) != pdTRUE) return false;
    portENTER_CRITICAL(&s_mux);
    bool valid = !s_depth && s_generation == expected_generation;
    portEXIT_CRITICAL(&s_mux);
    if (!valid) lock_give();
    return valid;
}

void phy_end_actuator(void) { lock_give(); }

#ifdef C5VRX_PHY_PINNED
extern uint8_t phy_i2c_readReg(uint8_t block, uint8_t host, uint8_t reg);
extern void phy_i2c_writeReg(uint8_t block, uint8_t host, uint8_t reg, uint8_t value);
static uint8_t s_filter_base[8];
static bool s_filter_base_valid;
#endif
static int s_filter_code = PHY_FILTER_CALIBRATED, s_filter_skirt;

bool phy_filter_capture_base(void)
{
#ifdef C5VRX_PHY_PINNED
    lock_take();
    if (!s_filter_base_valid) {
        for (uint8_t i = 0; i < 8; ++i) s_filter_base[i] = phy_i2c_readReg(0x67, 1, 6 + i);
        s_filter_base_valid = true;
    }
    lock_give();
    return true;
#else
    return false;
#endif
}

/* The code is applied from the boot-captured bytes, so a retune that did or
 * did not rewrite them gives the same result; read-back verified, on a
 * mismatch the calibrated bytes go back. */
bool phy_filter_set_code(int code)
{
#ifdef C5VRX_PHY_PINNED
    if (code != PHY_FILTER_CALIBRATED && (code < 0 || code > 63)) return false;
    lock_take();
    bool ok = s_filter_base_valid;
    for (uint8_t i = 0; ok && i < 2; ++i) {
        uint8_t v = code == PHY_FILTER_CALIBRATED ? s_filter_base[i] :
                    (uint8_t)((s_filter_base[i] & ~63u) | (unsigned)code);
        phy_i2c_writeReg(0x67, 1, 6 + i, v);
        ok = phy_i2c_readReg(0x67, 1, 6 + i) == v;
    }
    if (ok) s_filter_code = code;
    else if (s_filter_base_valid) {
        for (uint8_t i = 0; i < 2; ++i) phy_i2c_writeReg(0x67, 1, 6 + i, s_filter_base[i]);
        s_filter_code = PHY_FILTER_CALIBRATED;
    }
    lock_give();
    return ok;
#else
    (void)code;
    return false;
#endif
}

bool phy_filter_set_skirt(int offset)
{
#ifdef C5VRX_PHY_PINNED
    if (offset < 0 || offset > 60) return false;
    lock_take();
    bool ok = s_filter_base_valid;
    for (uint8_t i = 2; ok && i < 8; ++i) {
        uint8_t v = offset ? dsp_filter_code(s_filter_base[i], offset) : s_filter_base[i];
        phy_i2c_writeReg(0x67, 1, 6 + i, v);
        ok = phy_i2c_readReg(0x67, 1, 6 + i) == v;
    }
    if (ok) s_filter_skirt = offset;
    else if (s_filter_base_valid) {
        for (uint8_t i = 2; i < 8; ++i) phy_i2c_writeReg(0x67, 1, 6 + i, s_filter_base[i]);
        s_filter_skirt = 0;
    }
    lock_give();
    return ok;
#else
    (void)offset;
    return false;
#endif
}

int phy_filter_code(void) { return s_filter_code; }
int phy_filter_skirt(void) { return s_filter_skirt; }

int phy_filter_calibrated_code(void)
{
#ifdef C5VRX_PHY_PINNED
    return s_filter_base_valid ? (int)(s_filter_base[0] & 63u) : -1;
#else
    return -1;
#endif
}

/* ---- RX DC DACs ----------------------------------------------------------
 * Recovered from the pinned libphy: phy_pbus_set_dco() writes PBUS
 * (block,bank) (2,1),(3,1),(2,2),(3,2); phy_pbus_force_test() owns them in
 * debug mode. Bank 2 is the pair ESPARGOS esp-sdr corrects; the response
 * matrix is measured, not assumed. */
#ifdef C5VRX_PHY_PINNED
extern uint16_t phy_pbus_rd(uint32_t block, uint32_t bank);
extern void phy_pbus_force_test(uint32_t block, uint32_t bank, uint32_t value);
extern void phy_pbus_debugmode(void);
extern void phy_pbus_workmode(void);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);
#define DCO_PROBE 16
#define DCO_RANGE 96
#define DCO_TRIAL 32
#define DCO_DONE_MCELLS 100
#define PBUS_BLOCKS 11u
/* Only the gain/DC words the hold owns: BB (0), fine (1), DC (2, 3) and the
 * 5 GHz RF code (8). Copying all eleven blocks' read-back coincided with
 * dead/railing IQ on the board (2026-10-07). */
static inline bool pbus_owned(unsigned b) { return b <= 3u || b == 8u; }
static int s_dco_codes[2];
static bool s_dco_held;

static int dco_cost(const int dc[2]) { return dc[0] * dc[0] + dc[1] * dc[1]; }
static int dco_clamp(int v, int base)
{
    if (v < base - DCO_RANGE) v = base - DCO_RANGE;
    if (v > base + DCO_RANGE) v = base + DCO_RANGE;
    return v < 0 ? 0 : v > 511 ? 511 : v;
}
static void dco_apply(int i, int q)
{
    phy_pbus_force_test(2, 2, (uint32_t)i);
    phy_pbus_force_test(3, 2, (uint32_t)q);
}

/* phy_pbus_workmode() forces gain 50 for ~2 us and then clears the force
 * bit (disassembly, 2026-10-07): force the gain that was active again. */
static void pbus_workmode_keep_gain(void)
{
    uint32_t gain = *(volatile uint32_t *)0x600A702Cu;
    phy_pbus_workmode();
    if (gain & (1u << 23)) phy_force_rx_gain(true, (uint8_t)(gain >> 24));
}

static void read_live(uint16_t live[PBUS_BLOCKS][2])
{
    for (unsigned b = 0; b < PBUS_BLOCKS; ++b) if (pbus_owned(b))
        for (unsigned k = 0; k < 2; ++k) live[b][k] = phy_pbus_rd(b, k + 1);
}

static void enter_debug(uint16_t live[PBUS_BLOCKS][2])
{
    phy_pbus_debugmode();
    for (unsigned b = 0; b < PBUS_BLOCKS; ++b) if (pbus_owned(b))
        for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
}
#endif

/* Baseline, 2x2 response, up to four closed-loop steps, exact rollback.
 * The caller pauses gain control; measure() returns the I/Q centre in
 * milli-cells of the current lane. */
esp_err_t phy_dco_search(bool (*measure)(int dc[2]), phy_dco_result_t *res)
{
    memset(res, 0, sizeof(*res));
#ifndef C5VRX_PHY_PINNED
    (void)measure;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!measure) return ESP_ERR_INVALID_ARG;
    phy_begin();
    uint16_t live[PBUS_BLOCKS][2];
    read_live(live);
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    int before[2] = {0, 0};
    bool debug = false;
    if (measure(before)) {
        debug = true;
        enter_debug(live);
        int base[2] = {live[2][1], live[3][1]}, cur[2] = {base[0], base[1]};
        int best[2] = {base[0], base[1]}, best_dc[2] = {before[0], before[1]};
        int di = base[0] > 511 - DCO_PROBE ? -DCO_PROBE : DCO_PROBE;
        int dq = base[1] > 511 - DCO_PROBE ? -DCO_PROBE : DCO_PROBE;
        int mi[2] = {0, 0}, mq[2] = {0, 0};
        dco_apply(base[0] + di, base[1]);
        bool ok = measure(mi);
        if (ok) { dco_apply(base[0], base[1] + dq); ok = measure(mq); }
        dco_apply(base[0], base[1]);
        float j[4] = {0, 0, 0, 0};
        if (ok) {
            j[0] = (float)(mi[0] - before[0]) / di; j[1] = (float)(mq[0] - before[0]) / dq;
            j[2] = (float)(mi[1] - before[1]) / di; j[3] = (float)(mq[1] - before[1]) / dq;
        }
        int err[2] = {before[0], before[1]};
        for (unsigned it = 0; ok && it < 4 && dco_cost(best_dc) > DCO_DONE_MCELLS * DCO_DONE_MCELLS; ++it) {
            int sa, sb;
            if (!dsp_dco_step(j, (float)err[0], (float)err[1], DCO_TRIAL, &sa, &sb)) break;
            cur[0] = dco_clamp(cur[0] + sa, base[0]);
            cur[1] = dco_clamp(cur[1] + sb, base[1]);
            dco_apply(cur[0], cur[1]);
            if (!measure(err)) { ok = false; break; }
            if (dco_cost(err) < dco_cost(best_dc)) {
                best[0] = cur[0]; best[1] = cur[1];
                best_dc[0] = err[0]; best_dc[1] = err[1];
            }
        }
        if (ok) {
            res->measured = true;
            res->codes[0] = best[0]; res->codes[1] = best[1];
            res->residual_mcells[0] = best_dc[0]; res->residual_mcells[1] = best_dc[1];
            printf("DCO search before=%d/%d codes=%d/%d residual=%d/%d\n", before[0], before[1],
                   best[0], best[1], best_dc[0], best_dc[1]);
            result = ESP_OK;
        }
    }
    if (debug) {
        for (unsigned b = 0; b < PBUS_BLOCKS; ++b) if (pbus_owned(b))
            for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
        pbus_workmode_keep_gain();
    }
    phy_end();
    return result;
#endif
}

/* FLOOR rows (receiver noise below one quantizer step) used to keep the
 * chip's own DC: (-1.5, +3) cells at G59 on the board, a fixed IQ point that
 * Direct Gain read as a healthy carrier and parked on (board walk
 * 2026-10-13, docs/FINDINGS.md). Without noise dither the mean only moves in
 * whole cells, so the Jacobian search above sees no slope. Bisection on the
 * sign of each axis' mean finds the code where the fixed code flips from
 * cell -0.5 to +0.5, i.e. DC 0 within half a cell. Two rounds absorb the
 * small I/Q cross-coupling. */
static int dco_flat_axis(bool (*measure)(int dc[2]), int cur[2], int axis, int base, bool *ok)
{
    int lo = dco_clamp(base - DCO_RANGE, base), hi = dco_clamp(base + DCO_RANGE, base), m[2];
    cur[axis] = lo; dco_apply(cur[0], cur[1]);
    if (!measure(m)) { *ok = false; return base; }
    const bool lo_pos = m[axis] > 0;
    const int lo_abs = m[axis] < 0 ? -m[axis] : m[axis];
    cur[axis] = hi; dco_apply(cur[0], cur[1]);
    if (!measure(m)) { *ok = false; return base; }
    if ((m[axis] > 0) == lo_pos) {          /* no flip in range: keep the nearer end */
        const int hi_abs = m[axis] < 0 ? -m[axis] : m[axis];
        *ok = false;
        return hi_abs < lo_abs ? hi : lo;
    }
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        cur[axis] = mid; dco_apply(cur[0], cur[1]);
        if (!measure(m)) { *ok = false; return mid; }
        if ((m[axis] > 0) == lo_pos) lo = mid; else hi = mid;
    }
    return lo;
}

esp_err_t phy_dco_search_flat(bool (*measure)(int dc[2]), phy_dco_result_t *res)
{
    memset(res, 0, sizeof(*res));
#ifndef C5VRX_PHY_PINNED
    (void)measure;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!measure) return ESP_ERR_INVALID_ARG;
    phy_begin();
    uint16_t live[PBUS_BLOCKS][2];
    read_live(live);
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    int before[2] = {0, 0};
    bool debug = false;
    if (measure(before)) {
        debug = true;
        enter_debug(live);
        const int base[2] = {live[2][1], live[3][1]};
        int cur[2] = {base[0], base[1]}, err[2] = {before[0], before[1]};
        bool ok = true;
        for (unsigned round = 0; round < 2u && ok; ++round)
            for (int axis = 0; axis < 2 && ok; ++axis) {
                bool axis_ok = true;
                cur[axis] = dco_flat_axis(measure, cur, axis, base[axis], &axis_ok);
                if (!axis_ok) ok = false;
            }
        dco_apply(cur[0], cur[1]);
        if (ok && measure(err) && err[0] >= -1000 && err[0] <= 1000 && err[1] >= -1000 && err[1] <= 1000) {
            res->measured = true;
            res->codes[0] = cur[0]; res->codes[1] = cur[1];
            res->residual_mcells[0] = err[0]; res->residual_mcells[1] = err[1];
            result = ESP_OK;
        }
        printf("DCO flat before=%d/%d codes=%d/%d residual=%d/%d %s\n", before[0], before[1], cur[0], cur[1],
               err[0], err[1], result == ESP_OK ? "ok" : "no_flip");
        dco_apply(base[0], base[1]);
    }
    if (debug) {
        for (unsigned b = 0; b < PBUS_BLOCKS; ++b) if (pbus_owned(b))
            for (unsigned k = 0; k < 2; ++k) phy_pbus_force_test(b, k + 1, live[b][k]);
        pbus_workmode_keep_gain();
    }
    phy_end();
    return result;
#endif
}

esp_err_t phy_dco_hold(int code_i, int code_q)
{
#ifndef C5VRX_PHY_PINNED
    (void)code_i; (void)code_q;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (code_i < 0 || code_i > 511 || code_q < 0 || code_q > 511) return ESP_ERR_INVALID_ARG;
    lock_take();
    if (s_dco_held) { lock_give(); return ESP_ERR_INVALID_STATE; }
    uint16_t live[PBUS_BLOCKS][2];
    read_live(live);
    enter_debug(live);
    s_dco_codes[0] = code_i;
    s_dco_codes[1] = code_q;
    dco_apply(code_i, code_q);
    s_dco_held = true;
    lock_give();
    return ESP_OK;
#endif
}

bool phy_dco_release(void)
{
#ifndef C5VRX_PHY_PINNED
    return false;
#else
    if (!s_dco_held) return false;
    lock_take();
    pbus_workmode_keep_gain();
    s_dco_held = false;
    lock_give();
    return true;
#endif
}

bool phy_dco_held(void)
{
#ifdef C5VRX_PHY_PINNED
    return s_dco_held;
#else
    return false;
#endif
}

bool phy_dco_nudge(int di, int dq, int out[2])
{
#ifndef C5VRX_PHY_PINNED
    (void)di; (void)dq; (void)out;
    return false;
#else
    lock_take();
    bool ok = s_dco_held;
    if (ok) {
        int i = s_dco_codes[0] + di, q = s_dco_codes[1] + dq;
        s_dco_codes[0] = i < 0 ? 0 : i > 511 ? 511 : i;
        s_dco_codes[1] = q < 0 ? 0 : q > 511 ? 511 : q;
        dco_apply(s_dco_codes[0], s_dco_codes[1]);
        if (out) { out[0] = s_dco_codes[0]; out[1] = s_dco_codes[1]; }
    }
    lock_give();
    return ok;
#endif
}
