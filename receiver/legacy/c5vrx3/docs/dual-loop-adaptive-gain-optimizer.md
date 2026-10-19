# Dual-Loop Self-Calibrating Adaptive Gain Controller & FM Phase Optimizer

## 1. Executive Summary

Continuous analog FM video (FPV) reception on digital Wi-Fi RF hardware (ESP32-C5) presents a fundamental impedance mismatch:
1. **Standard Wi-Fi AGC** relies on packet preambles and RSSI thresholds. In continuous analog FM, it never detects an 802.11 packet header; the internal state machine remains wide open or hunts erratically. With a 200 mW VTX at close range (10–20 cm), it severely overconverts the signal, driving the 4-bit ADC into hard saturation ($I, Q = \pm 7$) and tearing the video frame.
2. **Blind Clipping AGC (The "Noise Trap")**: A naive AGC that reduces gain whenever clipping is observed fails catastrophically during deep fades or when the transmitter is off. Pure thermal noise generates sporadic extreme peaks. Reducing gain upon seeing these noise peaks lowers receiver sensitivity, permanently trapping the gain at the minimum limit ($G \approx 20$) exactly when maximum gain ($G = 62$) is needed.
3. **AHB Bus Stalls & Black Screens**: Triggering frequent cache writebacks/invalidations (`esp_cache_msync()`) on the active video ring locks the internal AHB system bus. The real-time PARLIO TX peripheral (delivering 40 MS/s DAC output every 25 ns) experiences FIFO starvation/underflow. When the DAC drops to 0V, the analog monitor loses horizontal and vertical sync, causing a 1–2 second black screen.

This document formalizes the **Dual-Loop Self-Calibrating Adaptive Gain Controller (FM Phase Optimizer)** implemented in `C5VRX-3/main/video.c`, proven live on Seeed Studio XIAO ESP32-C5 hardware (`COM10`).

---

## 2. Mathematical Formulation

### 2.1 Q4/I4 Complex Baseband Representation
The ESP32-C5 `MODEM_DIAG` bus streams packed 8-bit bytes representing Cartesian coordinates $(I, Q)$ at 40 MS/s:
* $I \in [-8, +7]$ (bits 7..4)
* $Q \in [-8, +7]$ (bits 3..0)

Instantaneous vector power:
$$P[n] = I[n]^2 + Q[n]^2 \quad \in [0, 128]$$

Effective carrier envelope radius:
$$R[n] = \sqrt{P[n]}$$

Quantization phase noise variance is inversely proportional to radius:
$$\sigma_\phi \approx \frac{\sigma_q}{R} \quad \text{where} \quad \sigma_q \approx \frac{1}{\sqrt{12}} \approx 0.289$$
- $R = 2 \implies \sigma_\phi \approx 8.3^\circ$ (high noise)
- $R = 5 \implies \sigma_\phi \approx 3.3^\circ$ (optimal sweet spot: $P \in [20, 30]$)
- $R \ge 7 \implies$ rail clipping plateaus, non-linear harmonic distortion.

### 2.2 Integer FM Phase Coherence ($Q_{\text{phase}}$)
In FM video (NTSC/PAL), baseband video voltage modulates carrier frequency with a maximum deviation typically bounded by $\pm 3.5\text{ MHz}$. At a 40 MHz sampling rate ($T_s = 25\text{ ns}$), the phase step $\Delta\phi[n] = \phi[n] - \phi[n-1]$ between consecutive samples satisfies:
$$|\Delta\phi_{\text{max}}| = 2\pi \frac{\Delta f}{f_s} \approx 2\pi \frac{3.5}{40} \approx 0.55\text{ rad} \approx 31.5^\circ$$

Even with subcarrier and pre-emphasis, valid FM video samples almost never exceed $|\Delta\phi| = 45^\circ$.

Using vector dot and cross products between consecutive vectors $\mathbf{v}[n] = (I[n], Q[n])$ and $\mathbf{v}[n-1] = (I[n-1], Q[n-1])$:
$$\text{Dot}[n] = I[n]I[n-1] + Q[n]Q[n-1] = R[n]R[n-1] \cos(\Delta\phi[n])$$
$$\text{Cross}[n] = Q[n]I[n-1] - I[n]Q[n-1] = R[n]R[n-1] \sin(\Delta\phi[n])$$

Because $\tan(45^\circ) = 1$:
$$|\Delta\phi[n]| \le 45^\circ \iff \text{Dot}[n] > 0 \quad \text{and} \quad |\text{Cross}[n]| \le \text{Dot}[n]$$

A sample is classified as **Coherent FM** if:
$$P[n] \ge 8 \quad \text{AND} \quad \text{Dot}[n] > 0 \quad \text{AND} \quad |\text{Cross}[n]| \le \text{Dot}[n]$$

Coherence Metric over $N = 256$ samples:
$$Q_{\text{phase}} = \frac{N_{\text{coherent}} \times 100}{N} \quad (0 \dots 100\%)$$

* **Pure Thermal Noise**: Phase is uniformly distributed over $[-\pi, \pi]$. Less than 25% of noise samples have $|\Delta\phi| \le 45^\circ$, and power is low. $Q_{\text{phase}}$ measures **$0\% - 10\%$**.
* **Locked FM Carrier**: $Q_{\text{phase}}$ measures **$95\% - 100\%$**.

---

## 3. Architecture & State Machine

```mermaid
stateDiagram-v2
    [*] --> SEARCH: Boot (Default G=32)
    
    SEARCH --> LEARN: Carrier Detected\n(Q_phase >= 55% OR P_med >= 18)
    SEARCH --> SEARCH: Pure Noise\nParked at G=32
    
    LEARN --> TRACK: Converged\n(20 <= P_med <= 30 AND N_clip <= 2)
    LEARN --> LEARN: Overload Fast Attack\n(drop -4 / -6 if clip >= 4 AND P_med > 18)
    LEARN --> LEARN: Step Down (P_med > 30: -2)\nStep Up (P_med < 20: +2/+4)
    
    TRACK --> LEARN: Overload Spike (clip >= 4 AND P_med > 18)\nOR Drift (P_med < 18 or > 32 for 150ms)
    TRACK --> SEARCH: Carrier Lost\n(Q_phase < 35% AND P_med < 12 for 400ms)
    TRACK --> TRACK: Deadband [18, 32]\nZERO Register Writes (Frozen)
```

### 3.1 Full-descriptor control window

The production controller now evaluates one already-completed **4092-byte GDMA descriptor** every 50 ms instead of a 256-byte / 6.4 us peek.

At 40 MS/s this represents about **102.3 us of raw Q4/I4**, or roughly 1.6 analog-video lines. The larger coherent window makes `P_median`, phase coherence, clipping and near-origin occupancy far less dependent on one small piece of burst/sync/active video.

The control task still touches only about 82 kB/s of ring data on average; the 40 MB/s RX -> BitScrambler -> TX path remains hardware paced.

### 3.2 Fast attack, slow release, quiet transitions

Gain changes are now deliberately rare:

* moderate clipping must persist across two complete descriptors before a normal `-2` gain move;
* only severe rail occupation can trigger an immediate emergency cut, limited to `-4`;
* weak-signal gain increases require five consecutive weak/coherence observations and move only `+2`;
* after every physical gain write the controller holds decisions for 500 ms;
* the gain-write path emits **no serial printf**, avoiding USB/CPU/bus activity at the exact moment the PHY is changing state;
* `TRACK` continues to use a wide deadband and performs zero gain-register writes while the signal remains acceptable.

This does not claim `phy_force_rx_gain()` itself is glitch-free. The purpose is to reduce how often C5VRX exposes the downstream CVBS decoder to a PHY gain transient.

### 3.3 SEARCH no longer treats power alone as a carrier

High-gain thermal noise can have significant raw power, so `P_median >= threshold` by itself is no longer sufficient to leave `SEARCH`. A candidate carrier must show meaningful phase coherence.

When no carrier is verified, production does **not** park permanently at maximum gain. After one second without lock it alternates between the normal acquisition point (`G52`) and a max-sensitivity probe (`G62`). Because no usable sync is present during this state, sensitivity can still be probed without continuously amplifying noise at G62.

The exact best acquisition and weak-signal gain indices remain a hardware-characterization task in Issue #27; these values are not claimed to be globally optimal RF/LNA stages.

### 3.4 AUTO PAL / NTSC observation

The same completed descriptor is also used, only when the carrier is clean and gain is settled, for an observation-only video-standard detector.

The detector mirrors the production Phase5 state LUT and a compact mask of Phase5 transitions that produce sync-tip DAC codes. Valid H-sync runs are measured at the 20 MS/s logical CVBS cadence:

* NTSC: about 1271 samples per line;
* PAL: exactly 1280 samples per line.

Multiple votes with a dead zone between the two periods are required before declaring a standard. The result is cached for menu entry; the realtime BitScrambler is not replaced or interrupted.

## 4. Historical live walk-around baseline

> The log below predates the 4092-byte / slow-transition controller above. It remains useful evidence that adaptive gain can recover weak and overloaded links, but it is **not** hardware validation of the new transition policy. The updated controller still requires a fixed-gain vs ACTIVE vs SHADOW A/B and gain-transition scope capture.

Tested on live Seeed Studio XIAO ESP32-C5 (`COM10`) receiving a 200 mW 5.8 GHz VTX on channel 173 (5865 MHz) with NTSC CVBS output to an analog CRT/LCD monitor.

### 4.1 Telemetry Excerpt (`C5VRX-3/live_walkaround_log.txt`)

```text
17:31:55 | [AGC:ACTIVE] State=SEARCH | G_act=32 G_shd=32 | P_med=0  Q_ph=  0% | clip=0   orig=256
17:31:56 | [AGC:ACTIVE] State=LEARN  | G_act=34 G_shd=34 | P_med=8  Q_ph= 58% | clip=0   orig=37
17:31:58 | [AGC:ACTIVE] State=LEARN  | G_act=40 G_shd=40 | P_med=25 Q_ph=100% | clip=0   orig=0
17:32:02 | [AGC:ACTIVE] State=TRACK  | G_act=58 G_shd=58 | P_med=25 Q_ph= 99% | clip=0   orig=0
...
17:32:18 | [AGC:ACTIVE] State=LEARN  | G_act=62 G_shd=62 | P_med=1  Q_ph=  0% | clip=0   orig=256
...
17:32:38 | [AGC:ACTIVE] State=TRACK  | G_act=62 G_shd=62 | P_med=25 Q_ph=100% | clip=0   orig=0
...
17:32:50 | [AGC:ACTIVE] State=LEARN  | G_act=40 G_shd=40 | P_med=52 Q_ph=100% | clip=53  orig=0
17:32:55 | [AGC:ACTIVE] State=LEARN  | G_act=32 G_shd=32 | P_med=25 Q_ph=100% | clip=0   orig=0
17:32:57 | [AGC:ACTIVE] State=LEARN  | G_act=24 G_shd=24 | P_med=45 Q_ph=100% | clip=0   orig=0
17:33:02 | [AGC:ACTIVE] State=TRACK  | G_act=32 G_shd=32 | P_med=34 Q_ph=100% | clip=0   orig=0
```

### 4.2 Key Observations
1. **Immediate Carrier Lock (17:31:56)**: $Q_{\text{phase}}$ jumped from $0\%$ to $58\% \to 100\%$ within 100 ms of VTX power-up.
2. **Deep Fade Immunity (17:32:16 – 17:32:36)**: Behind thick concrete walls, the signal dropped below threshold. Rather than collapsing to minimum gain, the controller climbed to **maximum sensitivity ($G = 62$)**.
3. **Instant Re-lock (17:32:38)**: Stepping out of the shadow instantly restored $Q_{\text{phase}} = 100\%$ and converged back to $P_{\text{median}} = 25$ with zero frame drops.
4. **Near-Field Overload Protection (17:32:57)**: Approaching within 20 cm of the receiver dropped gain down to **$G = 24$**, completely eliminating ADC saturation tearing.
5. **Zero Frame Loss / Zero Black Screen**: Across the full 3-minute test, `PARLIO TX FIFO empty (udf)` remained strictly **0**.

---

## 5. Fixed BW40 & Gain Ceiling

### 5.1 Noise physics and acquisition probing

Pumping RF gain to the maximum during total signal absence can amplify thermal/interference noise into the limited Q4/I4 range, while a high raw amplitude does not prove useful carrier SNR.

Current production therefore separates **carrier acquisition** from **locked gain optimization**:

* `SEARCH` requires phase coherence rather than raw power alone;
* with no carrier it holds the current state and only probes `G52`/`G62` at one-second intervals;
* once a coherent carrier exists, `LEARN` moves in small steps;
* once safely locked, `TRACK` freezes the physical gain until degradation persists.

`G52` and `G62` are currently practical probe points, not a measured final RF gain map. Issue #27 remains responsible for identifying stage boundaries, plateaus, transient-heavy indices and the true weak-signal optimum in dB of input attenuation.

### 5.2 Why production is fixed to BW40
A later experiment introduced a runtime BW40/BW20 "gearbox" based on the theoretical noise-power reduction from narrowing bandwidth. That control path is no longer part of production C5VRX.

The earlier physical A/B test in `docs/fix-cvbs-jitter-and-static.md` showed that the narrower setting attenuated part of the required wideband-FM video spectrum. On live hardware this reduced horizontal detail and destabilized chroma. The nominal Wi-Fi sensitivity difference between channel widths is also not direct proof that the undocumented `phy_wifi_fbw_sel()` toggle gives the same link-budget improvement in the raw MODEM_DIAG analog-FM path.

Production therefore uses:
* `WIFI_BW40` as the public ESP-IDF channel-width configuration.
* `phy_wifi_fbw_sel(1)` as a fixed analog-front-end setting.
* The same BW40 setting re-applied after each RF channel retune.
* No automatic or manual BW20 switching while the realtime video pipeline is running.

This also removes a runtime RF-filter transient from the continuous MODEM_DIAG -> PARLIO -> Phase5 -> CVBS path.


---

## 6. Exact Channel Matching & Carrier Frequency Offset (CFO / AFC)

### 6.1 Baseband CFO Mathematics
In Cartesian baseband $(I[n], Q[n])$ at sampling rate $f_s = 40\text{ MHz}$, a carrier frequency error $\Delta f = f_{\text{carrier}} - f_{\text{LO}}$ induces continuous phase rotation:
$$\Delta\theta[n] = 2\pi \frac{\Delta f}{f_s}$$
From vector products:
$$\Delta f_{\text{kHz}} \approx \frac{\text{Cross}}{\text{Dot}} \times \frac{f_s}{2\pi} = \frac{\sum \text{Cross}}{\sum \text{Dot}} \times 6366.2\text{ kHz}$$

When the VTX is centered on the receiver's local oscillator, $\sum \text{Cross} = 0$. When the VTX drifts high (e.g. $+150\text{ kHz}$), $\sum \text{Cross} > 0$.

### 6.2 Hardware Frequency Fine-Tuning via RFPLL SDM
The ESP32-C5 PHY library exports:
* `phy_chip_set_chan_offset(int offset_khz)`: Directly updates the Sigma-Delta Modulator (SDM) of the RF synthesizer in fractional steps of $4\text{ kHz}$ in $\approx 300\ \mu\text{s}$ (less than 5 video scanlines).
* `phy_set_freq(uint16_t freq_mhz, int offset_khz)`: Full RF synthesizer retuning across all FPV bands (Boscam A/B/E/F, RaceBand R).

### 6.3 Automatic Frequency Control (AFC) & Safety Boundary
To prevent the receiver from roaming or hopping away during flight:
1. **Strict Safety Boundary**: All offset adjustments are hard-clamped to $[-1500, +1500]\text{ kHz}$ ($\pm 1.5\text{ MHz}$). Because adjacent FPV channels are separated by $\ge 19\text{ MHz}$, it is physically impossible for the receiver to jump channels.
2. **Lock-Gated Centering**: AFC only adjusts when the carrier is verified solid ($Q_{\text{phase}} \ge 75\%$, $P_{\text{median}} \ge 18$, offset persisting for 1.0 s).
3. **Frozen in Noise/Fade**: If the signal is lost or fades below threshold ($Q_{\text{phase}} < 30\%$), AFC is completely frozen to prevent drift.
4. **Deadband Lock**: When within $\pm 35\text{ kHz}$ of center, the controller executes zero register writes.
