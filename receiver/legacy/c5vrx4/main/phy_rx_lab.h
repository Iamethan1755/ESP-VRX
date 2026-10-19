#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Observation only: no UART or PHY calls from OSI wrappers. */
void phy_rx_lab_osi_event(bool enable);
void phy_rx_lab_begin(const char *reason);
void phy_rx_lab_end(void);
void phy_rx_lab_capture_vendor(void);
uint32_t phy_rx_lab_generation(void);
bool phy_rx_lab_busy(void);
/* Task-only nonblocking actuator ownership. Successful acquire must be paired
 * with end_actuator. Does not change profiles or tune generation. */
bool phy_rx_lab_try_actuator(uint32_t expected_generation);
void phy_rx_lab_end_actuator(void);
void phy_rx_lab_poll(void);
void phy_rx_lab_toggle_monitor(void);
void phy_rx_lab_dump(bool analog_i2c);
void phy_rx_lab_mark(void);
/* Single isolated, ten-second override. Every transaction cancels it first. */
void phy_rx_lab_next_profile(void);
void phy_rx_lab_stock(void);
bool phy_rx_lab_profile_active(void);

/* Synchronous task-only A/B. Callback observes fresh samples without RF writes.
 * Caller pauses all controllers; ESP_FAIL means rollback failed, reboot required. */
esp_err_t phy_rx_lab_run_11p_probe(void (*observe)(const char *stage));

/* Explicit native-only laboratory exception: reversible BB gate, never RF-AGC
 * destruction or forced gain. Observer must yield; 1 or 100 cycles only. */
esp_err_t phy_rx_lab_run_native_hold(unsigned cycles,
    void (*observe)(const char *stage, unsigned cycle));

/* Range labs (2026-10-04). Facts from FPVGateC5RX's provenance notes, checked
 * in the pinned libphy: phy_check_sigrssi_en(1) + phy_get_sigrssi() give a
 * continuously measured signal RSSI (phy_get_rssi only updates on receive
 * events), and phy_param_track_tot(1,0) is their post-tune calibration. */
typedef struct {
    unsigned samples;
    int min_dbm, max_dbm, p10_dbm, p50_dbm, p90_dbm, mean_dbm_x10;
} phy_rx_lab_rssi_stats_t;
/* sigRSSI mode A/B: saves the eleven AGC words the enable rewrites (0x7030,
 * the BB-AGC gate, included), observes, enables, samples ~1 s at 1 ms,
 * observes, restores and verifies every word. Refused while the BB-AGC gate
 * is held (native hold/pacing). ESP_FAIL: restore unverified, reboot. */
esp_err_t phy_rx_lab_run_sigrssi_probe(void (*observe)(const char *stage),
                                       phy_rx_lab_rssi_stats_t *stats);
/* Temperature tracking A/B: phy_param_track_tot(1,0) = TX-power tracking,
 * phy_i2c_correct and phy_cal_param_track (temperature-triggered RX DC/IQ and
 * gain-table recalibration, then phy_chip_set_chan on the stored frequency,
 * which phy_set_freq also stores). Not reversible: a normal PHY maintenance
 * call the Wi-Fi driver would make periodically. */
esp_err_t phy_rx_lab_run_track_probe(void (*observe)(const char *stage));
/* Digital RX filter / ADC-rate lab: digital filter mode 0x600A0430[21:18] =
 * 0..15 with the ADC rate unchanged, then the other ADC rate through the
 * vendor phy_adc_rate_set (current mode, then the vendor-paired mode 0/8).
 * Restores both words and the ADC I2C byte and verifies them. Answers whether
 * a digital filter sits ahead of the MODEM_DIAG tap. ESP_FAIL: reboot. */
esp_err_t phy_rx_lab_run_dfilt_probe(void (*observe)(const char *stage, int arg));

/* Pre-demodulation labs (#165). Read-only status; the two A/B labs below
 * require the pinned PHY archive, pause nothing themselves (the caller pauses
 * all controllers), restore every owned field and return ESP_FAIL only when
 * that restore cannot be verified (reboot required). */
void phy_rx_lab_predemod_status(void);
/* RX DC DACs (PBUS blocks 2/3): baseline, 2x2 response, bounded closed-loop
 * correction, then exact restore and PBUS work mode. measure() returns the
 * I/Q centre in milli-cells of the current lane. */
esp_err_t phy_rx_lab_dco_set(bool on);
/* Range-edge hardware DC correction (PBUS debug mode, max gain only). */
bool phy_rx_lab_dco_release(void);
bool phy_rx_lab_dco_held(void);
bool phy_rx_lab_dco_valid(void);
void phy_rx_lab_dco_invalidate(void);
esp_err_t phy_rx_lab_run_dco_probe(bool (*measure)(int dc[2]),
                                   void (*observe)(const char *stage));
/* BBTOP 0x67 registers 6..13 (RX RC filter capacitors): calibrated baseline,
 * +4/+8/+16/+24 codes and the 11p-equivalent 60, then exact restore. */
esp_err_t phy_rx_lab_run_filter_sweep(void (*observe)(const char *stage, int offset));
/* Fixed analog bandwidth (after ESPARGOS esp-sdr): capture_base reads the
 * calibrated regs 6..13 once after PHY init. set_code writes an absolute 6-bit
 * code into regs 6/7 (RX0 capacitor DAC, upper bits kept; regs 8..13 untouched)
 * or, with PHY_RX_LAB_FILTER_CALIBRATED, the calibrated bytes; it runs inside
 * the caller's phy_rx_lab transaction or its own and verifies the read-back.
 * False: unpinned PHY, no base, bad code, or a read-back mismatch (then the
 * calibrated bytes are written back). */
#define PHY_RX_LAB_FILTER_CALIBRATED (-1)
bool phy_rx_lab_filter_capture_base(void);
bool phy_rx_lab_filter_set_code(int code);
int phy_rx_lab_filter_code(void);
/* Second stage: regs 8..13 = calibrated bytes + offset (0..60, saturating at
 * 60, upper bits kept; 0 = the calibrated bytes), verified; on a mismatch the
 * calibrated bytes are written back. */
bool phy_rx_lab_filter_set_skirt(int offset);
int phy_rx_lab_filter_skirt(void);
/* Lab only: regs 6/7 low six bits over the current (channel-mode) bytes,
 * verified. No bookkeeping; end with a normal retune. */
bool phy_rx_lab_filter_poke_live(int code);
int phy_rx_lab_filter_calibrated_code(void);
