#!/usr/bin/env python3
"""C5VRX-3 build validation script.

Checks that the production source tree meets all architectural constraints.
Run from the C5VRX-3 project root.

Exit 0: all checks pass.
Exit 1: one or more checks failed (details printed).
"""
import sys
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main"

failures = []
passes = []


def check(name, condition, detail=""):
    if condition:
        passes.append(name)
    else:
        failures.append(f"FAIL: {name}" + (f" -- {detail}" if detail else ""))


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


# ---- BitScrambler checks ----
bsasm_files = list(MAIN.glob("*.bsasm"))
check("BitScrambler source artifacts, including historical Trajectory, remain available",
      {f.name for f in bsasm_files} == {"fm.bsasm", "fm_relative_golden.bsasm", "fm_phase5_360.bsasm",
                                      "fm_phase5_fsm_capture.bsasm", "bs_relative_worker_probe.bsasm",
                                      "bs_relative_middle_probe.bsasm", "bs_addctia_probe.bsasm", "fm4.bsasm", "fm_traj.bsasm",
                                      "fm_phase8_hr_live.bsasm", "fm_phase8_8bit.bsasm", "fm_hc.bsasm"},
      f"found {[f.name for f in bsasm_files]}")

for bsasm_file in bsasm_files:
    bsasm = read(bsasm_file)
    check(f"{bsasm_file.name}: cfg eof_on downstream", "cfg eof_on downstream" in bsasm)
    expected_trailing = 10 if bsasm_file.name in {"bs_relative_worker_probe.bsasm", "bs_relative_middle_probe.bsasm", "bs_addctia_probe.bsasm"} else 0
    check(f"{bsasm_file.name}: cfg trailing_bytes {expected_trailing}",
          f"cfg trailing_bytes {expected_trailing}" in bsasm)
    check(f"{bsasm_file.name}: cfg prefetch true", "cfg prefetch true" in bsasm)
    check(f"{bsasm_file.name}: cfg lut_width_bits 16", "cfg lut_width_bits 16" in bsasm)
    check(f"{bsasm_file.name}: NO eof_on upstream", "cfg eof_on upstream" not in bsasm)
    check(f"{bsasm_file.name}: NO trailing_bytes 9", "trailing_bytes 9" not in bsasm)

# ---- Production .c file checks ----
c_files = list(MAIN.glob("*.c"))
all_c = "\n".join(read(f) for f in c_files)
c_names = [f.name for f in c_files]
video_c = read(MAIN / "video.c")
menu_lifecycle = video_c.split("static void video_set_menu_mode", 1)[1].split("static void menu_cycle_standard_mode", 1)[0]

check("production receiver and dedicated menu/auto-lab modules", set(c_names) == {"main.c", "board_config.c", "bs_relative_worker_probe.c", "bs_relative_middle_probe.c", "bs_addctia_probe.c", "phy_phase_tap_probe.c", "arc_phy.c", "arc_v3_controller.c", "arc_v5_autotune.c", "rx_auto_lab.c", "rf.c", "video.c", "direct_gain.c", "direct_gain_v2.c", "direct_gain_v3.c", "analog_video_detect.c", "menu_raster.c"},
      f"found: {c_names}")
check("main.c present", "main.c" in c_names)
check("rf.c present", "rf.c" in c_names)
check("video.c present", "video.c" in c_names)
check("menu bypasses the demodulator", "bitscrambler_disable(s_flight_bs)" in all_c)
check("no synthetic menu IQ", "s_black_iq" not in all_c and "get_white_word" not in all_c)
check("no live ring splice", not re.search(r"s_tx_dscr_nodes\[.*?->next\s*=", all_c))
check("serialized menu commands", "xQueueSend(s_menu_commands" in all_c and "xQueueReceive(s_menu_commands" in all_c)

check("no continuous_iq in production", "continuous_iq" not in all_c,
      "RF dump engine must not be present")
check("no telemetry_task in production", "telemetry_task" not in all_c,
      "periodic telemetry task must not be present")
check("no calibration subsystem in production",
      "calibration_get" not in all_c and "calibration.h" not in all_c,
      "runtime calibration must not be present")
check("no RF dump engine in production", "continuous_iq" not in all_c and "s_rf_dump" not in all_c,
      "RF dump subsystem must not be present")
check("no startup_trace in production", "startup_trace" not in all_c)
check("no snapshot infrastructure", "live_snapshot" not in all_c)
check("legacy trajectory/M2M code stays out of production",
      "c5vrx2_wbfm_q4_trajectory" not in all_c and
      "trajectory_reference" not in all_c and
      "BITSCRAMBLER_ATTACH_MEM2MEM" not in all_c)
check("no true40 in production", "true40" not in all_c)
check("no wbfm_q4.h in production", "wbfm_q4.h" not in all_c)

# Fixed constants
check("RAW_RING_BYTES == 32768",
      bool(re.search(r"RAW_RING_BYTES\s+32768", all_c)))
check("DAC idle uses the board-scaled six-bit pedestal 20",
      bool(re.search(r"DAC_IDLE_CODE\s+C5VRX_DAC_CODE\(20u\)", all_c)))
check("IQ_RATE_HZ == 40000000",
      bool(re.search(r"IQ_RATE_HZ\s+40000000", all_c)))

check("AGC uses one complete 4092-byte descriptor",
      bool(re.search(r"CONTROL_SAMPLE_BYTES\s+4092", all_c)) and
      "get_completed_rx_sample_window(CONTROL_SAMPLE_BYTES)" in all_c)
check("gain transition hot path has no AGC printf",
      "[AGC:GAIN]" not in all_c)
check("periodic runtime telemetry disabled",
      bool(re.search(r"PERIODIC_TELEMETRY\s+0", all_c)))
check("video standard defaults to AUTO semantic detector",
      "VIDEO_STD_MODE_AUTO" in all_c and
      "video_semantic_observe" in all_c and
      "phase5_pair_is_sync" in all_c and
      "fresh_sync = sync_quality >= 70" in all_c)
check("menu resolves detected PAL/NTSC before raster start",
      "s_video_std = resolved_menu_standard();" in all_c)

check("modern menu raster is SRAM-safe 384x56 logical pixels, centred at 4x NTSC / 5x PAL vertical scale",
      "MENU_UI_WIDTH 384u" in read(MAIN / "menu_raster.h") and
      "MENU_UI_LINES 56u" in read(MAIN / "menu_raster.h") and
      "MENU_UI_X_SCALE_NUM 208u" in read(MAIN / "menu_raster.h") and
      "MENU_UI_X_SCALE_DEN 50u" in read(MAIN / "menu_raster.h") and
      "MENU_NTSC_UI_Y_REPEAT 4u" in read(MAIN / "menu_raster.h") and
      "MENU_PAL_UI_Y_REPEAT 5u" in read(MAIN / "menu_raster.h") and
      "MENU_NTSC_PREFIX_BYTES 628u" in read(MAIN / "menu_raster.h") and
      "MENU_PAL_PREFIX_BYTES 652u" in read(MAIN / "menu_raster.h") and
      "MENU_UI_BYTES 1600u" in read(MAIN / "menu_raster.h") and
      "MENU_FIELDS 2u" in read(MAIN / "menu_raster.h") and
      "MENU_MAX_NODES 1840u" in read(MAIN / "menu_raster.h") and
      "s_menu_raster.ui" in all_c)
check("native menu enabled with safe defaults",
      bool(re.search(r"MENU_RUNTIME_ENABLED\s+1", all_c)) and
      "s_menu_boot_btn_enabled = true" in all_c and
      "RF_BW_MODE_BW40" in all_c and "VIDEO_OUTPUT_6BIT_40" in all_c)
check("three-second BOOT recovery cannot be blocked by persisted menu state",
      "open_recovery_menu" in all_c and
      "btn_ticks >= 60" in all_c and
      "btn_recovery_fired" in all_c and
      "s_menu_boot_btn_enabled = true;" in all_c and
      "s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;" in all_c and
      "s_output_mode = VIDEO_OUTPUT_6BIT_40;" in all_c and
      "apply_rx_profile(RX_PROFILE_DIRECT_GAIN);" in all_c and
      "[RECOVERY] GOLDEN + 6BIT@40 + DIRECT GAIN V2 restored" in all_c)
check("experimental BW auto and 4-bit@80 remain opt-in",
      "AUTO EXP" in all_c and "VIDEO_OUTPUT_4BIT_80" in all_c and
      "DAC4_RATE_HZ     80000000u" in all_c)
check("4BIT@80 remains reachable with a valid GOLDEN pairing",
      's_output_mode = s_output_mode == VIDEO_OUTPUT_6BIT_40 ?' in all_c and
      "s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;" in all_c and
      "s_fm4_program" in all_c)
check("TRAJ V2 is absent from the live demod selector",
      "cycle_demod_mode" not in all_c and
      "LONG:DAC - APPLIES ON EXIT" in all_c and
      "bitscrambler_load_program(s_flight_bs, s_fm_traj_program)" not in all_c)
check("menu lifecycle does not double-disable BitScrambler",
      menu_lifecycle.count("bitscrambler_disable(s_flight_bs)") == 1)
check("large menu descriptor chain is transient DMA heap, not static BSS",
      "dma_descriptor_t s_menu_nodes[MENU_MAX_NODES]" not in all_c and
      "static dma_descriptor_t *s_menu_nodes;" in all_c and
      "menu_count_segment" in all_c and
      "heap_caps_aligned_alloc" in all_c and
      "MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL" in all_c and
      "menu_free_nodes();" in all_c and
      "s_menu_node_capacity" in all_c)
check("menu allocation failure preserves live video instead of rebooting",
      "esp_err_t menu_err = menu_init_buffers();" in all_c and
      all_c.index("esp_err_t menu_err = menu_init_buffers();") <
      all_c.index("ESP_ERROR_CHECK(parlio_tx_unit_disable(s_tx));", all_c.index("static void video_set_menu_mode")) and
      "menu unavailable: %s" in all_c and
      "if (!s_menu_nodes) return ESP_ERR_NO_MEM;" in all_c)
check("legacy seven-line text menu removed",
      "MENU_TEXT_BYTES" not in all_c and "MENU_ROWS" not in all_c)
check("lag diagnostics poll PARLIO GDMA and BitScrambler",
      "poll_transport_faults" in all_c and
      "GDMA_IN_FAULT_MASK" in all_c and
      "GDMA_OUT_FAULT_MASK" in all_c and
      "LAG_EVT_BS_EOF_OVERLOAD" in all_c)
check("lag events correlate against gain writes",
      "near_gain_event_count" in all_c and
      "s_last_gain_write_us = esp_timer_get_time();" in all_c)

check("issue 27 fixed-gain characterization sweep present",
      "C5VRX_GAIN_SWEEP_BEGIN" in all_c and
      "C5VRX_LAB_ROW" in all_c and
      "LAB_GAIN_SETTLE_MS 700u" in all_c and
      "LAB_GAIN_DWELL_MS  1000u" in all_c and
      "lab_gain_sweep_tick" in all_c)
check("issue 28 quiet fixed baseline and reset present",
      "C5VRX_LAB_BASELINE_READY" in all_c and
      "lab_reset_correlation" in all_c and
      "s_lab_quiet" in all_c and
      "ANALOG_AGC_MANUAL" in all_c and
      "RF_BW_MODE_BW40" in all_c and
      "AFC_MODE_OFF" in all_c)
check("vendor PHY timer inventory is reachable on demand",
      "rf_dump_tracked_timers();" in all_c)
check("manual gain cannot step below production lower bound",
      "s_current_gain > LAB_GAIN_MIN" in all_c and
      "LAB_GAIN_MIN       2u" in all_c)

check("PHY lab exposes read-only filter/ADC snapshot",
      "rf_get_phy_snapshot" in all_c and
      "RX_GAIN_STATUS_REG" in all_c and
      "ADC_RATE_REG" in all_c and
      "rx_filter_mode" in all_c)
check("FFT placement probe is bounded and restores automatic FFT scaling",
      "C5VRX_FFT_PROBE_BEGIN" in all_c and
      "s_lab_fft_values[] = {16, 24, 32, 40}" in all_c and
      "rf_set_fft_scale_force(false, 0)" in all_c)
check("fixed-gain BW40/BW20 A/B probe present",
      "C5VRX_BW_PROBE_BEGIN" in all_c and
      'lab_print_row("BW_SWEEP"' in all_c and
      "LAB_BW_SETTLE_MS" in all_c)
check("PRE-Q4 self-noise probe physically removes TX and restores live pipeline",
      "C5VRX_PREQ4_TXNOISE_BEGIN" in all_c and
      'lab_print_row("PREQ4_TX_ACTIVE"' in all_c and
      'lab_print_row("PREQ4_TX_QUIET"' in all_c and
      "parlio_del_tx_unit(s_tx)" in all_c and
      "gpio_set_level((gpio_num_t)s_dac_gpio[i], 0)" in all_c and
      "lab_restore_live_tx_pipeline" in all_c and
      "s_lab_tx_quiet" in all_c)
check("PRE-Q4 far-gain probe uses complete valid vendor table",
      "C5VRX_PREQ4_FAR_BEGIN" in all_c and
      "rf_get_arc_survival_gain()" in all_c and
      "table->max_index" in all_c and
      'lab_print_row("PREQ4_FAR_GAIN"' in all_c and
      "lab_apply_vendor_gain" in all_c)
check("fresh PHY calibration is next-boot only through official ESP-IDF API",
      "esp_phy_erase_cal_data_in_nvs()" in all_c and
      "rf_prepare_fresh_phy_calibration" in all_c and
      "C5VRX_PREQ4_FULLCAL_ARMED" in all_c and
      "esp_restart();" in all_c)
check("Q4 IQ-centering metrics present",
      "dc_i_x100" in all_c and "dc_q_x100" in all_c and
      "iq_skew_permille" in all_c and "iq_cross_permille" in all_c)
check("shadow exact-adjacent endpoint winding observer present",
      "demod_phase5_endpoint_loses_winding" in all_c and
      "winding_permille" in all_c and
      "winding_triplets" in all_c and
      "production_first" in all_c and
      "strong_winding_permille" in all_c and
      "DEMOD_STRONG_POWER_MIN" in read(MAIN / "demod_quality.h"))
check("semantic CVBS sync score requires pulse width plus line period",
      "video_semantic_observe" in all_c and
      "s_last_sync_quality" in all_c and
      "fresh_sync = sync_quality >= 70" in all_c and
      "width_score" in all_c and "best_period_score" in all_c)
check("range trials penalize endpoint winding instead of amplitude-only scoring",
      "demod_winding_penalty" in read(MAIN / "range_control.h") and
      "DEMOD_STATIC_HEAVY_WINDING_PM" in read(MAIN / "demod_quality.h") and
      "sync_quality_avg >= 70" in read(MAIN / "range_control.h"))
fusion_header = read(MAIN / "fusion_receiver.h")
fusion_optimizer = read(MAIN / "fusion_optimizer.h")
fusion_temporal = read(MAIN / "fusion_temporal.h")
check("IQ fusion combines adjacent, lag-2, lag-4 and robust slope evidence",
      "fusion_shadow_push" in fusion_header and
      "lag2_disagreement_permille" in fusion_header and
      "lag4_disagreement_permille" in fusion_header and
      "fusion_robust_delta3" in fusion_header and
      "consensus_outlier_permille" in fusion_header)
check("fusion learner is contextual, bounded and high-gain biased at loss",
      "fusion_optimizer_tick" in fusion_optimizer and
      "FUSION_CONTEXT_WEAK" in fusion_optimizer and
      "FUSION_CONTEXT_BLOCKER" in fusion_optimizer and
      "Loss at the range edge means maximum known sensitivity" in fusion_optimizer and
      "FUSION_OPT_SETTLE_TICKS" in fusion_optimizer)
check("Range v2 uses distributed temporal observation without extra PHY writes",
      "fusion_temporal_update" in fusion_temporal and
      "fade_score" in fusion_temporal and
      "recovery_score" in fusion_temporal and
      "FUSION_FAST_SAMPLE_BYTES" in all_c and
      "FUSION_FAST_PERIOD_MS" in all_c and
      "fusion_observer_task" in all_c)
check("fusion separates catastrophic phase risk from average quality",
      "catastrophic_risk" in fusion_header and
      "fusion_catastrophic_risk_score" in fusion_header and
      "baseline_risk" in fusion_optimizer and
      "risk_win" in fusion_optimizer)
check("fusion learns local ordered gain transitions and forgets stale certainty",
      "fusion_edge_cell_t" in fusion_optimizer and
      "fusion_record_trial_edge" in fusion_optimizer and
      "fusion_optimizer_decay" in fusion_optimizer and
      "FUSION_OPT_DECAY_TICKS" in fusion_optimizer)
check("Range v2 exposes centering and read-only ARC characterization probes",
      "C5VRX_AFC_PROBE_BEGIN" in all_c and
      "C5VRX_ARC_ORACLE" in all_c and
      "lab_run_frequency_probe" in all_c and
      "lab_print_arc_oracle" in all_c)
check("offline demod benchmark gates adjacent/PLL experiments",
      (ROOT / "tools/range_demod_bench.py").exists() and
      "phase5_endpoint_winding_disagree_permille" in read(ROOT / "tools/range_demod_bench.py") and
      "trajectory_v2_hard_ge16_permille" in read(ROOT / "tools/range_demod_bench.py") and
      "pll_lite_pair_codes" in read(ROOT / "tools/range_demod_bench.py") and
      "pll_demod" in read(ROOT / "tools/range_demod_bench.py"))

traj_asm = read(MAIN / "fm_traj.bsasm")
traj_gen = read(ROOT / "tools" / "train_trajectory_v2.py")
check("Trajectory v2 preserves no-rewrap adjacent trajectory target",
      "groups[address].append((previous, scale_rad(d0 + d1)))" in traj_gen and
      "target is the clean two-adjacent trajectory d0+d1" in
          read(MAIN / "trajectory_v2_lut.h") and
      "no second wrap" in read(MAIN / "trajectory_v2_lut.h"))
check("Trajectory v2 live loop stays two-bundle and quiet 20M->40M",
      "trajectory:" in traj_asm and
      "emit:" in traj_asm and
      "jmp trajectory" in traj_asm and
      "write 16" in traj_asm and
      "cfg eof_on downstream" in traj_asm and
      "cfg trailing_bytes 0" in traj_asm)
check("Trajectory v2 is excluded from live firmware and Golden remains default",
      "DEMOD_MODE_GOLDEN_PHASE5 = 0" in all_c and
      "DEMOD_MODE_TRAJECTORY_V2 = 1" in all_c and
      "s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5" in all_c and
      "s_fm_traj_program" not in all_c)
check("Trajectory v2 remains a research artifact outside live selection",
      "DEMOD_MODE_TRAJECTORY_V2 = 1" in all_c and
      "cycle_demod_mode" not in all_c and
      "s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;" in all_c)
check("Trajectory v2 supervisor mirrors two-stage token LUT and uncertainty",
      "trajectory_v2_stage1_address" in all_c and
      "trajectory_v2_stage2_address" in all_c and
      "trajectory_v2_code" in all_c and
      "trajectory_uncertainty_permille" in all_c and
      "c5vrx_trajectory_v2_token" in all_c and
      "c5vrx_trajectory_v2_confidence" in all_c and
      "traj_uncert_pm=%d" in all_c and
      "pll_slip_pm=%d" in all_c)
check("Trajectory v2 live two-stage address contract is mirrored everywhere",
      "middle_raw >> 7u" in all_c and
      "set 24 7" in traj_asm and
      "set 25 O30" in traj_asm and
      "set 21 L6" in traj_asm and
      "set 25 L15" in traj_asm and
      "middle raw-I sign" in traj_gen and
      "middle_raw >> 7" in read(ROOT / "tools/range_demod_bench.py") and
      "c5vrx_trajectory_v2_token" in read(MAIN / "trajectory_v2_lut.h"))
check("live demod is fixed Golden with no menu switch",
      'return "GOLDEN";' in all_c and
      "cycle_demod_mode" not in all_c and
      "2S:DEMOD" not in all_c)
check("stored legacy demod choice migrates safely to Golden",
      "SETTINGS_VERSION 4u" in all_c and
      ".demod_mode = (uint8_t)s_demod_mode" in all_c and
      "legacy_v3 = settings.version == 3u" in all_c and
      "sizeof(persisted_settings_t) == 14u" in all_c and
      "s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5" in all_c)
check("Trajectory-only uncertainty does not contaminate Golden A/B",
      "active_demod_shadow" in all_c and
      "s_demod_mode != DEMOD_MODE_TRAJECTORY_V2" in all_c and
      "shadow.trajectory_uncertainty_permille = 0" in all_c)
check("PLL-lite remains observation-only and risk-gated",
      "pll_predictor_delta" in fusion_header and
      "pll_lite_slip_permille" in fusion_header and
      "pll_lite_hold_permille" in fusion_header and
      "catastrophic_risk" in fusion_header)

check("Fusion/Range supervisor remains independent from Golden live demod",
      "RX_PROFILE_FUSION_EXP" in all_c and
      "RX_PROFILE_RANGE_V2_EXP" in all_c and
      "fusion_optimizer_tick" in all_c and
      "fusion_make_observation" in all_c and
      'target_bitscrambler_add_src("fm.bsasm")' in read(MAIN / "CMakeLists.txt") and
      'target_bitscrambler_add_src("fm_traj.bsasm")' not in read(MAIN / "CMakeLists.txt"))
check("lag correlation covers any tracked PHY write",
      "near_phy_event_count" in all_c and
      "s_last_phy_write_us" in all_c and
      "PHY_WRITE_BW" in all_c and "PHY_WRITE_OFFSET" in all_c and
      "PHY_WRITE_FFT" in all_c)
check("TRACK freezes automatic bandwidth and AFC writes",
      "TRACK is a hard no-write zone" in all_c and
      "AUTO AFC is acquisition-only" in all_c and
      "s_agc_state != AGC_STATE_TRACK" in all_c)
check("unsafe undocumented gain/filter ROM controls remain out of production",
      all(symbol not in all_c for symbol in (
          "phy_pbus_set_rxgain(",
          "phy_bb_gain_index(",
          "phy_wifi_agc_sat_gain(",
          "phy_chan_filt_set(",
          "phy_rx_filter_mode(",
          "phy_rfrx_rxdc_cal(",
          "phy_rfrx_rxdc_cal_new(",
          "phy_pbus_rx_dco_cal(",
          "phy_dc_iq_est_new(",
          "phy_set_cal_rxdc(",
          "phy_rxiq_set_reg(",
      )))

check("only V2, V1 and ARC V3 are selectable automatic gain profiles",
      "RX_PROFILE_DIRECT_GAIN_V1" in all_c and
      "RX_PROFILE_ARC_V3_EXP" in all_c and
      "s_rx_profile == RX_PROFILE_DIRECT_GAIN ?" in all_c and
      "RX_PROFILE_DIRECT_GAIN_V1 :" in all_c and
      "RX_PROFILE_ARC_V3_EXP : RX_PROFILE_DIRECT_GAIN" in all_c and
      "apply_rx_profile(RX_PROFILE_ARC_V5_AUTOTUNE_EXP);" not in all_c)
check("stored legacy gain profiles migrate to default V2",
      "settings.rx_profile == RX_PROFILE_DIRECT_GAIN_V1" in all_c and
      "settings.rx_profile == RX_PROFILE_ARC_V3_EXP" in all_c and
      "RX_PROFILE_ARC_V3_EXP : RX_PROFILE_DIRECT_GAIN" in all_c)

check("RF menu offers only native AGC plus BW control; no on-screen profile selector",
      '"NATIVE HW AGC"' in all_c and
      '"LONG: BANDWIDTH"' in all_c and
      "2S:PROFILE" not in all_c and
      "btn_profile_fired" not in all_c)
check("VIDEO menu has only the Golden live demodulator",
      "LONG:DAC - APPLIES ON EXIT" in all_c and
      "cycle_demod_mode();" not in all_c and
      "btn_demod_fired" not in all_c)
check("experimental PHY environment reads stay out of the range default",
      "s_rx_profile == RX_PROFILE_AUTO_EXP && ++phy_metric_ticks >= 5" in all_c and
      "rf_try_get_noise_floor_dbm" in all_c and
      "rf_try_get_wideband_rssi_dbm" in all_c)
check("AUTO FFT promotion requires same-boot raw-Q4 evidence",
      "s_fft_q4_effect_known" in all_c and
      "s_fft_q4_effective" in all_c and
      "best_score >= baseline_score + 12" in all_c and
      "s_rx_profile == RX_PROFILE_AUTO_EXP && s_fft_q4_effective" in all_c)
check("AUTO profile uses Q4 IQ quality and bounded environment bias",
      "metrics.iq_skew_permille > 260" in all_c and
      "metrics.iq_cross_permille > 260" in all_c and
      "s_last_phy_rssi_dbm - s_last_noise_floor_dbm" in all_c)
arc_phy = read(MAIN / "arc_phy.c") + read(MAIN / "arc_phy.h")
arc_controller = read(MAIN / "arc_controller.h")
check("ARC reconstructs only vendor-generated gain tuples",
      "59c1234e929212aec0fdda75769b759951235536" in arc_phy and
      "PHY_PARAM_RX_SPANS_OFFSET 0x422u" in arc_phy and
      "64u, 100u, 93u, 94u, 107u, 119u, 124u, 125u, 127u" in arc_phy and
      "tuple->packed_state" in arc_phy and
      "phy_pbus_set_rxgain(" not in all_c)
check("ARC preserves clean LOCK and uses maximum RF-stage survival",
      "ARC_LOCK" in arc_controller and
      "LOCK invariant: clean IQ causes no PHY writes" in arc_controller and
      "arc_gain_highest_rf_stage_start" in arc_controller and
      "s_rx_profile == RX_PROFILE_ARC" in all_c)
check("incorrect one-argument AGC maximum call is absent",
      "phy_agc_max_gain_set" not in all_c and
      "rf_set_experimental_hw_agc" not in all_c)

arc_v3 = read(MAIN / "arc_v3_controller.c") + read(MAIN / "arc_v3_controller.h")
check("ARC V3 EXP is an explicit gain-only Q4 test profile",
      "RX_PROFILE_ARC_V3_EXP" in video_c and
      'return "ARC V3 EXP"' in video_c and
      "} else if (c == 'Y') {" in video_c and
      "arc_v3_controller_tick" in video_c and
      "s_rf_bw_mode = RF_BW_MODE_BW40" in video_c and
      "s_afc_mode = AFC_MODE_OFF" in video_c)
check("ARC V3 does not require semantic sync to escape a starved G62",
      "hard_starved" in arc_v3 and
      "step_gain(arc, hard ? 4 : 1)" in arc_v3 and
      "arc->table.max_index" in arc_v3 and
      "bool sync" not in read(MAIN / "arc_v3_controller.h"))
check("ARC V3 filters ordinary gain decisions over five raw-Q4 windows",
      "ARC_V3_FILTER_SAMPLES 5u" in read(MAIN / "arc_v3_controller.h") and
      "filter_push" in arc_v3 and
      "median_int" in arc_v3 and
      "if (!arc->filtered_valid)" in arc_v3 and
      "const arc_v3_observation_t *f = &arc->filtered" in arc_v3)
check("ARC V3 gain-up is slower than gain-down and guards direction reversal",
      "ARC_V3_UP_CONFIRM_TICKS          5u" in arc_v3 and
      "ARC_V3_OVERLOAD_CONFIRM_TICKS    2u" in arc_v3 and
      "ARC_V3_UP_GUARD_TICKS           20u" in arc_v3 and
      "arc->up_guard_ticks != 0u" in arc_v3 and
      "G66 -> G81" in arc_v3)
check("ARC V3 preserves zero-write Q4 lock and overload protection",
      "Zero-write clean LOCK invariant" in arc_v3 and
      "lock_hold_good" in arc_v3 and
      "arc->severe_ticks >= 2u" in arc_v3 and
      "step_gain(arc, -4)" in arc_v3)
arc_v5 = read(MAIN / "arc_v5_autotune.c") + read(MAIN / "arc_v5_autotune.h")
check("ARC V5 research implementation is not selectable",
      "RX_PROFILE_ARC_V5_AUTOTUNE_EXP" in video_c and
      'return "ARC V5 AUTOTUNE"' in video_c and
      "} else if (c == 'Z') {" not in video_c and
      "arc_v5_autotune_tick" in video_c and
      "arc_v3_controller_tick" in arc_v5)
check("ARC V5 persistence is confidence-gated and versioned",
      "ARC_V5_MODEL_MAGIC" in arc_v5 and
      "table_fingerprint" in arc_v5 and
      "arc_v5_export_model" in arc_v5 and
      'nvs_set_blob(h, "model"' in arc_v5 and
      "ARC_V5_SAVE_INTERVAL_MS" in arc_v5)
check("ARC V5 never learns NO_CARRIER and falls back to V3",
      "ARC_V5_CONTEXT_NO_CARRIER" in arc_v5 and
      'Never "learn" pure noise' in arc_v5 and
      "Stable/unknown territory falls back to the proven ARC V3 controller" in arc_v5)
check("ARC V5 prediction is confidence bounded",
      "confidence >= 64u ? 12" in arc_v5 and
      "confidence >= 24u ? 8" in arc_v5 and
      "confidence < 8u" in arc_v5 and
      "ARC_V5_VERIFY_MIN_TICKS" in arc_v5 and
      "ARC_V5_CONTROL_PERIOD_MS" in arc_v5)

check("ARC V3 exposes an explicit RF limit at the vendor-table ceiling",
      "ARC_V3_RF_LIMIT" in arc_v3 and
      "arc->gain >= arc->table.max_index" in arc_v3 and
      "ARC_V3_RF_LIMIT_CONFIRM_TICKS" in arc_v3)

rx_auto = read(MAIN / "rx_auto_lab.c") + read(MAIN / "rx_auto_lab.h")
check("ARC V3 RX AUTO LAB is explicit opt-in console instrumentation",
      "lab_run_rx_auto" in video_c and
      "} else if (c == 'U') {" in video_c and
      "C5VRX_RX_AUTO_RESULT" in video_c and
      "C5VRX_RX_AUTO_PROOF" in video_c and
      "C5VRX_RX_AUTO_REF" in video_c)
check("RX AUTO uses reference-guarded candidate search and repeated proof",
      "rx_auto_measure_against_reference" in video_c and
      "RX_AUTO_PROOF_ROUNDS      5u" in video_c and
      "proof_stable >= 4u" in video_c and
      "proof_wins >= 4u" in video_c and
      "rx_auto_reference_stable" in rx_auto)
check("RX AUTO separates gain, bandwidth and centering stages",
      "RX_AUTO_GAIN" in video_c and
      "RX_AUTO_BW" in video_c and
      "RX_AUTO_CENTER_COARSE" in video_c and
      "RX_AUTO_CENTER_FINE" in video_c and
      "top_gain[RX_AUTO_TOP_COUNT]" in video_c)
check("RX AUTO hard-rejects clipping/transport instead of maximizing P",
      "o->transport_faults != 0 || o->clip_permille > 30" in rx_auto and
      "p_distance" in rx_auto and
      "return iabs(p - 24)" in rx_auto and
      "RX_AUTO_SWEET" in rx_auto)
check("RX AUTO has explicit RF_LIMIT and overload classifiers",
      "rx_auto_is_rf_limit" in rx_auto and
      "o->p_median <= 4" in rx_auto and
      "o->origin_permille >= 800" in rx_auto and
      "rx_auto_is_overload" in rx_auto and
      'status = "RF_LIMIT"' in video_c and
      'status = "OVERLOAD"' in video_c)
check("RX AUTO freezes only a proven winner and restores failed experiments",
      "rx_auto_freeze_winner" in video_c and
      "s_agc_mode = ANALOG_AGC_MANUAL" in video_c and
      "s_afc_mode = AFC_MODE_HOLD" in video_c and
      "rx_auto_restore_saved(&saved)" in video_c and
      "No setting is persisted" in read(ROOT / "docs" / "pre-q4-lab.md"))

# ---- Web flasher / release safety ----
web_app = read(ROOT / "web" / "app.js")
workflow = read(ROOT / ".github" / "workflows" / "build.yml")
web_workflow = read(ROOT / ".github" / "workflows" / "deploy-web.yml")
readme = read(ROOT / "README.md")
check("web flasher selects the application image explicitly",
      "findApplicationAsset(assets)" in web_app and
      "!name.includes('bootloader')" in web_app and
      "!name.includes('partition')" in web_app and
      "!name.includes('merged')" in web_app)
check("full firmware fallback rejects incomplete release assets",
      "Incomplete full firmware package" in web_app)
check("PR builds have a separate warned flasher tab",
      "PR_BUILD_TAG_PATTERN" in web_app and
      "githubPrBuilds = prBuilds" in web_app and
      "activeSource === 'pr'" in web_app and
      "window.confirm(" in web_app and
      'id="tabPr"' in read(ROOT / "web" / "index.html") and
      'id="panePr"' in read(ROOT / "web" / "index.html") and
      'id="selectPrBuild"' in read(ROOT / "web" / "index.html"))
check("web flasher prefers same-origin Pages firmware mirror",
      "firmware/releases.json" in web_app and
      "asset.local_url" in web_app and
      "same-origin Pages firmware mirror" in web_app and
      "corsproxy.io" not in web_app)
check("Pages deploy builds firmware mirror from trusted main",
      'workflow_run:' in web_workflow and
      '"C5VRX-3 Production CI"' in web_workflow and '"C5VRX-5 Build"' in web_workflow and
      "ref: main" in web_workflow and
      "tools/prepare_pages_site.sh pages-site" in web_workflow and
      "path: pages-site/" in web_workflow)
prepare_pages = read(ROOT / "tools" / "prepare_pages_site.sh")
check("Pages mirror includes versioned releases and the C5VRX-5 channel",
      "gh release download" in prepare_pages and
      "local_url" in prepare_pages and
      "^c5vrx5-pr-[0-9]+$" in prepare_pages and
      ".[:20]" in prepare_pages)
check("same-repo PR firmware is published only as an explicit prerelease",
      "publish-pr-build:" in workflow and
      "github.event.pull_request.head.repo.full_name == github.repository" in workflow and
      "--prerelease" in workflow and
      'tag="pr-${PR_NUMBER}"' in workflow and
      "cleanup-pr-build:" in workflow)
check("stacked development PRs publish webflasher firmware from PR events only",
      'branches: [main, "feat/**", "fix/**", "codex/**"]' in workflow and
      'types: [opened, synchronize, reopened, closed]' in workflow and
      "Publish Experimental PR Build" in workflow and
      "github.event_name == 'pull_request'" in workflow and
      "github.event.action != 'closed'" in workflow and
      "github.event.pull_request.head.repo.full_name == github.repository" in workflow and
      "github.event.pull_request.head.sha" in workflow and
      "gh pr list" not in workflow and
      "pull-requests: read" not in workflow)
check("CI concurrency separates merge push from PR-close cleanup",
      "github.event_name" in workflow and
      "github.event.pull_request.number || github.ref" in workflow and
      "cancel-in-progress: true" in workflow)
check("release build is gated by architectural validation",
      "needs: [version, validate]" in workflow)
check("firmware CI does not create Pages deployments",
      "actions/deploy-pages" not in workflow and
      "Deploy Web Flasher to GitHub Pages" not in workflow)
check("web deployment is GitHub Pages-only and refreshes after firmware CI",
      'paths:' in web_workflow and
      '"web/**"' in web_workflow and
      "workflow_run:" in web_workflow and
      "workflow_dispatch:" in web_workflow and
      "actions/deploy-pages@v4" in web_workflow and
      "Pages Mirror" not in web_workflow and
      "Pages mirror" not in web_workflow)
check("README documents GitHub Pages as the only production flasher host",
      readme.count("https://twotoz.github.io/C5VRX/") >= 3 and
      "hosted entirely by **GitHub Pages**" in readme and
      "There is no VPS" in readme and
      "c5vrx.com" not in readme)
agents = read(ROOT / "AGENTS.md")
check("AGENTS documents release, PR-build and trusted Pages mirror flow",
      "## Releases, PR builds, and web flasher deployment" in agents and
      "pr-<PR_NUMBER>" in agents and
      "PR Builds" in agents and
      "GitHub Pages" in agents and
      "always checks out trusted" in agents and
      "firmware/releases.json" in agents and
      "same-origin" in agents and
      "Debugging a PR build missing from the web flasher" in agents and
      "pull_request workflow is the sole owner" in agents and
      'branches: [main, "feat/**", "fix/**", "codex/**"]' in agents and
      "TRAJ V2 remains a research artifact" in agents)

check("gain transient classifier present",
      "gain_quality_drop_count" in all_c and
      "s_last_gain_drop_transition" in all_c)
check("visible lag marker present",
      "[LAG MARK]" in all_c and
      "user_lag_mark_count" in all_c)

# RX POS edge (not NEG)
check("PARLIO_SAMPLE_EDGE_POS in video.c",
      "PARLIO_SAMPLE_EDGE_POS" in all_c)
check("no PARLIO_SAMPLE_EDGE_NEG for RX",
      "PARLIO_SAMPLE_EDGE_NEG" not in all_c,
      "RX must use POS edge; NEG is for TX shift edge only (PARLIO_SHIFT_EDGE_NEG)")

# TX NEG shift edge
check("PARLIO_SHIFT_EDGE_NEG in video.c",
      "PARLIO_SHIFT_EDGE_NEG" in all_c)

# loop_transmission
check("loop_transmission present", "loop_transmission" in all_c)

# Zero-EOF descriptor patch
check("Zero-EOF descriptor patch present", "patch_descriptors_clear_eof" in all_c,
      "Zero-EOF circular descriptor patch must be present to prevent wrap bubbles")

# No periodic tasks
check("no periodic telemetry or timer tasks in production",
      "telemetry_task" not in all_c and "hw_diag_task" not in all_c,
      "periodic tasks must not be present")

# Default + experimental BS programs
cmake_main = read(MAIN / "CMakeLists.txt")
bs_srcs = re.findall(r'target_bitscrambler_add_src\("([^"]+)"\)', cmake_main)
check("only selectable and diagnostic BitScrambler programs are in CMakeLists",
      bs_srcs == ["fm.bsasm", "fm_relative_golden.bsasm", "fm_phase5_360.bsasm",
                  "bs_relative_worker_probe.bsasm", "bs_relative_middle_probe.bsasm",
                  "fm_phase5_fsm_capture.bsasm", "fm4.bsasm",
                  "bs_addctia_probe.bsasm", "fm_phase8_8bit.bsasm", "fm_phase8_hr_live.bsasm", "fm_hc.bsasm"], f"found: {bs_srcs}")

# Phase5-360 architecture and simulator validation
TOOLS_DIR = ROOT / "tools"
check("Phase5-360 architecture document present",
      (TOOLS_DIR / "phase5_360_architecture.md").exists())
check("Phase5-360 simulator present",
      (TOOLS_DIR / "sim_phase5_360.py").exists())
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))
import sim_phase5_360
check("Phase5-360 mathematical simulation passes all checks",
      sim_phase5_360.run_all_simulations())

# Direct Gain V2 is the active, single-owner physical actuator in this profile.
direct_gain_v2_h = read(MAIN / "direct_gain_v2.h")
direct_gain_v2_c = read(MAIN / "direct_gain_v2.c")
check("Direct Gain V2 module and default profile present",
      'static volatile rx_profile_t s_rx_profile = RX_PROFILE_DIRECT_GAIN;' in video_c and
      'direct_gain_v2_fast_tick(&metrics, &tm)' in video_c and
      'direct_gain_v2.c' in read(MAIN / "CMakeLists.txt"))
check("Manual gain mode declared before profile_gain_max uses it",
      video_c.index("static volatile analog_agc_mode_t s_agc_mode") <
      video_c.index("static uint8_t profile_gain_max(void)"))
check("Direct Gain V2 decodes each physical vendor state",
      'arc_gain_tuple_decode(&v2->table' in direct_gain_v2_c and
      'rf_stage' in direct_gain_v2_c and 'bb_code' in direct_gain_v2_c and
      'fine_code' in direct_gain_v2_h + direct_gain_v2_c)
check("Direct Gain V2 owns fast gain writes and slow path has none",
      'direct_gain_v2_tick(&s_direct_gain_v2' in video_c and
      'direct_gain_v2_sync_applied(&s_direct_gain_v2' in video_c and
      'Fast observer owns all V2 decisions and PHY gain writes.' in video_c and
      's_rx_profile != RX_PROFILE_DIRECT_GAIN' in video_c)
check("Direct Gain V2 protects fresh IQ and clean zero-write lock",
      'V2_FRESH_US 12000u' in direct_gain_v2_c and
      'o->observed_us <= v2->write_us' in direct_gain_v2_c and
      'if (useful(o))' in direct_gain_v2_c and
      'DIRECT_GAIN_V2_FLOOR 20u' in direct_gain_v2_h)
check("Direct Gain V2 learns exact physical edges",
      'e->from == from && e->to == to' in direct_gain_v2_c and
      'e->count >= 3' in direct_gain_v2_c)
import gen_phase8_gain_lut
check("Direct Gain V3 uses the generated centered-cell Phase8 LUT",
      read(MAIN / "phase8_gain_lut.h") == gen_phase8_gain_lut.build() and
      'c5vrx_phase8_gain_lut' in video_c)
check("Direct Gain V3 is the sole experimental automatic gain writer",
      'direct_gain_v3_observer_task' in video_c and
      'direct_gain_v3_tick(&s_direct_gain_v3' in video_c and
      'direct_gain_v3_sync_applied(&s_direct_gain_v3' in video_c and
      'V3 is the sole actuator in this experimental build.' in video_c)
rf_c = read(MAIN / "rf.c")
check("Issue #119 P8ENV origin-collapse oracle is on-demand and read-only",
      '#include "phase8_envelope.h"' in video_c and
      "} else if (c == 'E') {" in video_c and
      "p8env_capture_report();" in video_c and
      "p8env_add(&acc" in video_c)
check("Native HW AGC experiment never disables vendor AGC and blocks firmware gain writes",
      re.search(r"if \(!s_native_agc\) \{\s*phy_disable_agc\(\);", rf_c) and
      re.search(r"void rf_set_rx_gain\(bool force, uint8_t gain_idx\)\s*\{[^}]*"
                r"if \(s_native_agc\) \{\s*\+\+s_native_agc_blocked_writes;\s*return;", rf_c) and
      "if (rf_native_agc_active()) return s_current_gain;" in video_c and
      "} else if (c == 'N') {" in video_c)
check("Direct Gain V4 is the default boot gain owner; native AGC needs explicit NVS 1",
      re.search(r"static bool native_agc_boot_requested\(void\)\s*\{\s*nvs_handle_t handle;\s*"
                r"uint8_t value = 0;", rf_c) and
      "return value == 1u;" in rf_c)
check("RSSI probe owns every AGC/BW/AFC write during its measurement",
      "s_rssi_probe_active = true;" in video_c and
      "if (s_rssi_probe_active) continue;" in video_c and
      "s_rssi_probe_active = false;" in video_c)

# Phase5c Static Correction validation
phase5_360_asm = read(MAIN / "fm_phase5_360.bsasm")
p360_words = [int(w) for w in phase5_360_asm.split("\nlut ", 1)[1].split("\n\n# Both workers", 1)[0].split()]
golden_words = [int(w) for w in read(MAIN / "fm.bsasm").split("\nlut ", 1)[1].split("\n\naddress_delta:", 1)[0].split()]
# In Phase5c, large delta pairs (|delta| >= 12) must output Golden pedestal 20 (bits 8..13), never rail-slam to 0 or 63!
p360_large_delta_dacs = [(p360_words[i] >> 8) & 63 for i in range(1024) if abs(((i & 31) - ((i >> 5) & 31) + 16) % 32 - 16) >= 12]
check("Phase5c static squelch eliminates harsh salt-and-pepper rails",
      len(p360_large_delta_dacs) == 288 and
      all(dac == 20 for dac in p360_large_delta_dacs))
check("Live Phase5c LUT matches Golden or pedestal for all endpoint pairs",
      len(p360_words) == len(golden_words) == 1024 and
      all(((p360_words[i] >> 8) & 63) ==
          (20 if abs(((i & 31) - (i >> 5) + 16) % 32 - 16) >= 12
           else golden_words[i] & 63)
          for i in range(1024)) and
      all((p360_words[i] & 31) == ((golden_words[i & 255] >> 8) & 31)
          for i in range(1024)))

workers = phase5_360_asm.split("worker_golden:\n", 1)[1].split("worker_360:\n", 1)
check("Live middle-sample branch remains endpoint Golden, not ideal 360",
      len(workers) == 2 and
      "set 0..5 L8..L13" in workers[0] and
      "set 0..5 L8..L13" in workers[1] and
      sum(1 for p in range(32) for m in range(32) for c in range(32)
          if (((m - p + 16) % 32 - 16) +
              ((c - m + 16) % 32 - 16)) !=
             ((c - p + 16) % 32 - 16)) == 8192)

# ---- Summary ----
print(f"\n{'='*50}")
print(f"C5VRX-3 build validation: {len(passes)} passed, {len(failures)} failed")
print(f"{'='*50}")
if failures:
    for f in failures:
        print(f)
    sys.exit(1)
else:
    print("All checks passed.")
    sys.exit(0)
