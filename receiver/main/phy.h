/* C5VRX-5 by Twotoz and the C5VRX contributors: owned access to the pinned
 * ESP32-C5 PHY (from legacy/c5vrx4/main/phy_rx_lab.c). Writes outside the public
 * API run only on the libphy whose SHA256 the build verified
 * (C5VRX_PHY_PINNED); on any other PHY they refuse and return false. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Recursive PHY transaction. Every end publishes a new generation, so a
 * control decision taken before a retune is never applied after it. */
void phy_begin(void);
void phy_end(void);
uint32_t phy_generation(void);
bool phy_busy(void);
/* Non-blocking gain/offset ownership without a new generation. */
bool phy_try_actuator(uint32_t expected_generation);
void phy_end_actuator(void);

/* Analog RX filter, after ESPARGOS esp-sdr (GPL-3.0, commit ac627b0b):
 * BBTOP 0x67 regs 6/7 hold the RX0 capacitor code (absolute low six bits),
 * regs 8..13 the second stage (calibrated bytes + skirt offset). The
 * calibrated bytes are captured once after PHY init. */
#define PHY_FILTER_CALIBRATED (-1)
bool phy_filter_capture_base(void);
bool phy_filter_set_code(int code);
bool phy_filter_set_skirt(int offset);
int phy_filter_code(void);
int phy_filter_skirt(void);
int phy_filter_calibrated_code(void);

/* RX DC DACs (PBUS blocks 2/3 bank 2). A hold forces one pair in PBUS
 * debug mode with every owned live word re-asserted; any gain write or PHY
 * restore releases it first (rf.c) and the post-gain hook re-holds. */
typedef struct {
    bool measured;
    int codes[2];
    int residual_mcells[2];
} phy_dco_result_t;
esp_err_t phy_dco_search(bool (*measure)(int dc[2]), phy_dco_result_t *res);
/* The same codes for a row whose receiver noise is below one quantizer step:
 * the IQ is one fixed code, so the DC is found per axis by bisection on the
 * code where that fixed code flips (DC within half a cell). measured=false
 * when an axis has no flip within the search range. */
esp_err_t phy_dco_search_flat(bool (*measure)(int dc[2]), phy_dco_result_t *res);
esp_err_t phy_dco_hold(int code_i, int code_q);
bool phy_dco_release(void);
bool phy_dco_held(void);
bool phy_dco_nudge(int di, int dq, int out[2]);
