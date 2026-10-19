# Comprehensive Phase5-360 & Baseband Tap Engineering Findings

**Author:** C5VRX Architectural & DSP Engineering Team  
**Date:** September 2026  
**Status:** Canonical Reference Document  
**Scope:** pull requests #84, #86, #87, #88, #90, #91, #92, #94 and issues #84, #93, #95

---

## 1. Executive Summary & Core Engineering Lessons

Across late September 2026, the C5VRX project conducted an extensive, deep-dive architectural investigation into **True 360° FM Demodulation (Phase5-360)**, alternative **MODEM_DIAG ADC lane selection** (`Q[6:3]/I[6:3]` vs. `Q[9:6]/I[9:6]`), **Direct Gain AGC stability**, and **impulsive click noise suppression** on the ESP32-C5 WLAN SDR platform.

This investigation yielded five foundational, hardware-verified conclusions:

1. **The FPV Video Physics Invariant:**  
   Standard analog PAL/NTSC composite video (CVBS) transmitted over 5.8 GHz WBFM has an analog baseband bandwidth of $B \approx 6.0\text{ MHz}$ and a maximum carrier frequency deviation of $\Delta f_{\text{peak}} \approx \pm 4.0\text{ to } \pm 4.5\text{ MHz}$. At the 20 MS/s sample period ($T_s = 50\text{ ns}$), the maximum continuous phase rotation across a sample pair is:
   $$|\Delta\theta_{\text{video}}| \le 2\pi \times 4.5\text{ MHz} \times 50\text{ ns} \approx 81^\circ \quad (\text{absolute extreme with chroma burst: } \le 124^\circ)$$
   **100% of all legitimate, valid camera video signals lie strictly within $[-124^\circ, +124^\circ]$.** Any measured phase jump $|\Delta\theta| > 124^\circ$ (especially near the $180^\circ$ pole) is physically impossible for a camera carrier and is guaranteed to be thermal noise or a multi-path destructive fade.

2. **The "Primum Non Nocere" Rule of Demodulator Design:**  
   In PR #92 (`Q2 adjacent-50`), a learned 5-bit trajectory token was introduced into the 1024-word LUT to unwind rare 360° winding clicks. While this resolved 60/60 synthetic edge winding events, the 5-bit token quantization (~32 states) injected ~5% approximation noise across **100% of normal, clean video pixels**, degrading overall picture clarity (*"niet echt clean"*).  
   *Rule:* **Never degrade the 98% of clean video to fix the 2% of rare noise clicks.** Clean video must remain 100% bit-exact to Golden Phase5.

3. **MODEM_DIAG Lane Selection (`Q[9:6]` is Mandatory):**  
   The ESP32-C5 WLAN baseband outputs 10-bit signed two's complement ADC samples:
   - `Q[9:0]` on `DIAG[9:0]` (`DIAG[9]` is the Sign bit of Q).
   - `I[9:0]` on `DIAG[19:10]` (`DIAG[19]` is the Sign bit of I).  
   In PR #94, dropping the PARLIO RX tap to `Q[6:3]` (`DIAG[3:6]`) and `I[6:3]` (`DIAG[13:16]`) discarded the true sign bits. Bits 6 and 16 acted as unanchored magnitude bits that wrapped modulo 16 whenever the signal exceeded $\pm 8$ LSBs. This caused permanent AGC clip vetoes (`clip_permille > 20‰`), locked Direct Gain in endless `SEEK`, and destroyed Phase5 phase calculation. The MSB nibble `Q[9:6] / I[9:6]` is mathematically mandatory.

4. **Sign Bits as the Middle-Sample Quadrant Oracle:**  
   Because PARLIO samples at 40 MS/s, an intermediate sample $S_1$ (at +25 ns) is captured between endpoint $S_0$ and endpoint $S_2$. Under `Q[9:6] / I[9:6]`, Bit 3 of every byte is `Sign(Q)` and Bit 7 is `Sign(I)`. These two bits alone identify the exact quadrant ($Q_0 \dots Q_3$) of the middle sample. Because in 25 ns the phase rotation is $< 45^\circ$, knowing the quadrant of $S_1$ resolves 100% of trajectory winding without needing any middle-sample amplitude bits or token compression.

5. **Counter-A Hardware Subtraction Milestone (PR #90):**  
   Using the ESP32-C5 BitScrambler's `ADDCTIA` / `ADDCTIAL` hardware accumulator, modulo-32 subtraction $(C - P) \pmod{32}$ executes in a single 25 ns clock cycle in the ALU. This reduces the DAC lookup requirement from 1,024 words to just 64 words, freeing **960 entries** in the 1024-word RAM for exact multi-step trajectory resolution without sacrificing Golden fidelity.

---

## 2. Chronological Analysis of Investigated PRs & Milestones

### PR #83: Pedestal-Gated Back-Porch AFC
- **Branch:** `feat/back-porch-afc`
- **Objective:** Measure Carrier Frequency Offset (CFO) strictly during the horizontal blanking back-porch (immediately following verified horizontal sync pulses) to eliminate scene luminance bias.
- **Outcome:** Highly effective in software benchmarks; accurate to $\pm 3.5\text{ kHz}$. Superseded as an autonomous background observer; manual AFC centering remains default on production `main` to avoid PHY writes during live flight.

### PR #86: Direct Full Phase5-360 FPGA Demodulator & Oracle
- **Branch:** `feat/phase5-360-adjacent-oracle`
- **Objective:** Build a 3-stage hardware pipeline in RTL and standalone C oracle evaluating $(P, M, C)$ triplets across simulated DMA boundaries.
- **Outcome:** Validated that $(M - P) + (C - M)$ resolves 8,192 out of 32,768 triplets where Golden wraps wrongly. Proved that an FPGA reference can run at fixed 1-pair latency. Confirmed that early middle-bit compression is sub-optimal compared to full interval evaluation.

### PR #87: C5 BitScrambler M2M Speedlab
- **Branch:** `feat/c5-bs-speedlab`
- **Objective:** Benchmark memory-to-memory (M2M) BitScrambler execution to test whether multiple bundles per 16-bit word could be sustained without PARLIO pacing.
- **Outcome:** Highlighted that the C5 public loopback API does not expose GDMA channel weights. Demonstrated that PARLIO TX-driven streaming remains the only verified zero-drop 40 MHz transport.

### PR #88: Exact Endpoint-Conditioned One-Bit Middle Reduction
- **Branch:** `feat/phase5-360-conditioned-middle-bit`
- **Objective:** Test if a single boolean condition:
  $$\text{middle\_bit} = [M \ge (P \oplus 16)] \oplus [M > (C \oplus 16)]$$
  can decide whether to emit Golden or rail.
- **Outcome:** Oracle verified 100% truth table match across 262,144 combinations. Confirmed that a single half-plane discriminator can resolve $>180^\circ$ wrapping if computed after endpoints are established.

### PR #90: Counter-A Hardware Subtraction & 960-Entry LUT Optimization (MERGED TO MAIN)
- **Branch:** `feat/phase5-360-counter-a` (Merged at `91a6730`)
- **Objective:** Execute modulo subtraction in the BitScrambler ALU using `ADDCTIAL`.
- **Silicon Result:** Verified on physical XIAO ESP32-C5:
  ```text
  BS_ADDCTIA status=PASS written=256 mismatches=0 err=ESP_OK
  ```
- **Significance:** Solved the 1024-entry LUT capacity bottleneck on ESP32-C5 forever.

### PR #91: Compact Phase5-360 Winding Lookup Model
- **Branch:** `feat/live-phase5-360`
- **Objective:** 1024-word table keyed by two differences $(M - P)$ and $(C - P)$.
- **Outcome:** Showed that 24,576 no-winding triples suffered slight deviations (up to 2 DAC codes) in a non-linear compressed table. Kept as a draft reference; established that 2D delta tables should retain exact Golden centroids.

### PR #92: Live Q2 Adjacent-50 Two-Stage Trajectory Demodulator
- **Branch:** `feat/phase5-360-q2-live`
- **Objective:** Two-stage 1024x16 LUT mapping current raw IQ + 2 middle sign bits to a 5-bit token, then combining with previous Phase5 to emit 6-bit CVBS.
- **Measured Negative:** In live bench testing, the 5-bit token loss added visible quantization grain (*"niet echt clean"*). Closed as an instructive experimental benchmark: proved 2-stage execution without stalls, but ruled out 5-bit learned tokens for production video.

### PR #94: Q[6:3]/I[6:3] MODEM_DIAG Tap & Max Gain Lock
- **Branch:** `feat/true-fm-360`
- **Objective:** Evaluate lower-order ADC bits for higher baseband dynamic range and test locked maximum RF gain.
- **Measured Negative:**
  1. Without `DIAG[9]` and `DIAG[19]`, bit 6 and bit 16 wrap around modulo 16 on signals $> \pm 8$ LSBs.
  2. Direct Gain AGC is locked in permanent `SEEK` due to high `clip_permille`.
  3. Max RF gain saturates the front-end mixer on typical transmitters, causing severe non-linear distortion.
  4. Closed in favor of standard `Q[9:6]/I[9:6]` with adaptive Direct Gain.

---

## 3. Analysis of Open Issues

### Issue #84: Residual Static / Desync & Double Gain Slew Limiting
- **Resolution:** Static was traced to:
  1. Premature promotion of experimental demodulators (`Q2 adjacent-50` or unanchored taps).
  2. Aggressive gain slew rate changes causing momentary ADC saturation.
  Mitigated on `main` via `Phase5c Static Squelch` (forcing delta $\ge 12$ to neutral pedestal 20) and damped Direct Gain slew limiting (`DIRECT_GAIN_MAX_SLEW_DOWN`).

### Issue #93: Close-Range Layer Tearing & Baseband Headroom
- **Resolution:** Tearing at close transmitter proximity was caused by baseband ADC clipping when RF gain was forced too high. Resolved by allowing Direct Gain to drop gain down to survival/minimum indices when $P > 44$.

### Issue #95: Direct Gain V2 Physical RF/BB/Fine Target Control
- **Resolution:** Identified 25 specific measurement and calibration gaps in AGC telemetry. The core finding is that AGC must prioritize front-end mixer linearity over raw LNA amplification, and that `clip_permille` must distinguish true ADC rail saturation from sign-bit transitions.

---

## 4. The Canonical Production Baseline on `main`

The authoritative production configuration on `main` is:
- **RF Tap:** `s_iq_diag[] = {6, 7, 8, 9, 16, 17, 18, 19}` (Full `Q[9:6]` & `I[9:6]` with sign bits intact).
- **Demodulator:** `Phase5c` ([fm_phase5_360.bsasm](../main/fm_phase5_360.bsasm)):
  - Exact Golden Phase5 1:1 mapping for all legitimate video transitions ($|\Delta\theta| \le 90^\circ$).
  - Static Squelch (clamping to pedestal 20) for impossible delta transitions ($|\Delta\theta| \ge 123.75^\circ$) to eliminate salt-and-pepper noise flits.
- **AGC:** Direct Gain with damped slew rate and calibrated feed-forward table.
- **Transport:** 40 MHz PARLIO RX -> 32K cyclic ring -> BitScrambler -> 40 MHz DAC ([D, D] duplicated 20 MS/s unique CVBS).
