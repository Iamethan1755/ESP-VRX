# C5VRX Range v2

Range v2 separates **RF sensitivity**, **FM threshold**, and **usable CVBS
survival**. The goal is to stop treating every loss of picture as "not enough
gain".

Long-form engineering knowledge is preserved in:

- `docs/range-v2-knowledge.md` — architecture, Fusion behavior, control theory,
  demodulation findings, numerical references and known limitations.
- `docs/range-v2-research-notes.md` — RF-chain research, BW/AFC/LNA/diversity
  hypotheses, validation discipline and the next hardware test sequence.
- `docs/trajectory-v2.md` — the experimental two-bundle adjacent-trajectory demodulator and PLL-lite slip research.

This branch is built on the PR #43 Fusion Engine. The proven 40 MS/s live
MODEM_DIAG -> PARLIO -> BitScrambler -> DAC path stays intact unless a new
demodulator has first passed offline and hardware throughput gates.

## What Range v2 changes now

### 1. Distributed shadow observation

The old controller analyzed one 4092-byte descriptor every 50 ms:

- 4092 / 40 MS/s = ~102.3 us observed;
- ~0.205% time coverage.

Range v2 adds a read-only 512-byte observer every 6 ms. It never writes PHY
state and never paces DMA. Fast and slow integer EWMAs estimate:

- quality trend;
- Q_phase trend;
- near-origin trend;
- endpoint/lag-4 winding trend;
- clipping trend;
- fade score;
- recovery score;
- stability.

The physical gain actuator remains slow.

### 2. Local gain learning instead of eight unrelated arms

G62..G34 are an ordered chain, but Range v2 does **not** assume that quality is
globally monotonic with the gain index.

The optimizer learns the measured local response of neighboring transitions,
for example G58 <-> G62. Ordinary exploration is restricted to one local edge
at a time. NO_CARRIER and hard overload retain bounded safety jumps.

Old visit counts decay so a state learned on another channel/environment does
not remain permanently "certain".

### 3. Catastrophic-risk objective

Average quality is no longer allowed to hide a dangerous phase-error tail.
Fusion separately estimates catastrophic risk from:

- near-origin IQ;
- production endpoint winding;
- strong-IQ winding;
- lag-4 trajectory disagreement;
- consensus outliers;
- low-confidence intervals;
- rail clipping.

A gain trial is accepted when it materially improves quality without making
risk much worse, or when it materially reduces risk without a large quality
regression.

### 4. Fade-aware decisions

A persistent, high-risk WEAK state with a strong negative fast/slow trend may
make one local sensitivity move earlier than the normal decision interval.

This does **not** mean faster gain hunting. Clean/high-confidence IQ remains a
zero-write state and contradictory transient observations suppress ordinary
trials.

## New lab commands

The existing lab tools remain:

- `g`: G2..G62 fixed-gain sweep;
- `F`: FFT/Q4 scaling probe;
- `W`: BW40/BW20 fixed-gain probe;
- `p`: machine-readable snapshot.

Range v2 adds:

- `A`: acquisition-only frequency-centering sweep from -1000 to +1000 kHz.
  It scores Fusion quality, sync, winding and catastrophic risk, prints the
  best offset, and restores the original offset.
- `H`: vendor-AGC oracle. Espressif AGC is allowed to settle temporarily,
  the real RX gain/filter/ADC/source registers are captured, then the previous
  deterministic C5VRX state is restored.

The oracle is characterization, not a production AGC mode.

## Demodulation: the main remaining range gate

The current live Phase5 path still discriminates selected samples at 20 MS/s,
so the middle 40 MS/s IQ trajectory is not part of the branch decision.

Issue #23 already measured that endpoint winding disagreement becomes much more
common when IQ is weak. That can create a usable-video cliff before the RF
front end has actually lost the carrier.

The target architecture remains:

```
Q4/I4 @ 40 MS/s
    -> phase/confidence for every sample
    -> exact adjacent FM @ 40 MS/s
    -> confidence-aware phase-slip handling
    -> low-pass/combine
    -> 2:1 decimation
    -> CVBS @ 20 MS/s
    -> proven [D,D] DAC transport
```

The repository already contains legacy exact-adjacent/pair-sum BitScrambler
research. It is not promoted directly because the historical problem was
realtime scheduling/throughput, not the DSP equation.

### Offline range benchmark

Use:

```sh
python3 tools/range_demod_bench.py raw-q4.bin --json
```

It compares on the same capture:

- current Phase5 endpoint winding;
- exact adjacent pair-sum trajectory;
- conservative low-IQ confidence repair;
- full-Q4 adjacent discriminator tail;
- second-order PLL phase tracking.

Synthetic regression:

```sh
python3 tools/range_demod_bench.py --self-test
```

This benchmark is the gate before attempting a live PLL/phase-slip guard.

## Why a PLL-like path is interesting

A sample-to-sample discriminator converts a noise-driven phase wrap into a
large instantaneous FM impulse. A stateful PLL can reject short inconsistent
phase events outside its loop bandwidth.

A separate analog-FPV SDR implementation has experimentally added a second
order PLL demodulator for weak-signal threshold extension:

https://github.com/isaacbentley/orecchiette-fpv-drone-analog-rs

Its results are useful evidence for an experiment, **not** a claimed C5VRX dB
improvement. C5VRX must reproduce any benefit on real packed Q4 captures.

## RX-chain characterization

ESP32-C5 ROM exposes a wider receive-control surface than the single forced
gain index, including symbols such as:

- `phy_pbus_set_rxgain`;
- `phy_bb_gain_index`;
- `phy_gen_rx_gain_table`;
- `phy_rx_sense_set`;
- `phy_agc_max_gain_set`;
- `phy_wifi_agc_sat_gain`;
- `phy_read_hw_noisefloor`;
- `phy_wifi_fbw_sel`.

See the ESP-IDF C5 ROM symbol map:

https://github.com/espressif/esp-idf/blob/master/components/esp_rom/esp32c5/ld/esp32c5.rom.eco3.ld

A symbol name is **not** sufficient evidence for its ABI or safe semantics.
Range v2 therefore does not blindly call the undocumented BB/PBUS controls.

Required hardware characterization:

1. controlled RF source/VTX + step attenuation;
2. sweep G2..G62;
3. log Q4 origin, clipping, Q_phase, winding, sync and PHY registers;
4. run the vendor-AGC oracle at the same RF level;
5. identify register/stage discontinuities;
6. only then expose proven pre-Q4 controls to Fusion.

The important distinction is whether a control improves information **before
Q4 quantization**. A downstream/digital scale cannot restore phase information
that has already collapsed into a few Q4 codes.

## BW20 and carrier centering

Halving noise bandwidth can theoretically reduce integrated thermal noise, but
analog FPV is wideband FM. BW20 is therefore not declared "+3 dB range".

The `W` probe must demonstrate improvement in:

- phase-slip/winding rate;
- sync survival;
- useful video detail/chroma;

under controlled attenuation.

Likewise, continuous AFC is avoided. The `A` probe characterizes the optimum
center first; production retuning should happen only during acquisition/loss,
not while clean video is locked.

## Hardware range extensions

Software cannot create RF energy that did not reach the chip.

After demod and gain-chain characterization, useful hardware A/B tests are:

- known-good 5.8 GHz antenna and feed loss;
- optional low-noise preamp ahead of the C5;
- blocker/compression testing before keeping an LNA enabled;
- low-loss preselection only when blockers justify its insertion-loss cost;
- dual-receiver selection diversity for spatial multipath.

Dual-C5 coherent combining is not part of this PR. A first useful diversity
implementation should simply choose the receiver with lower phase risk / better
sync using hysteresis; coherent MRC would require shared/estimated phase,
frequency and sample timing.

## Acceptance gates

Do not call a Range v2 idea a range improvement until the same controlled
attenuation test shows a lower usable-video threshold.

For demod work, record at minimum:

- input attenuation / estimated RF level;
- origin and clip permille;
- endpoint and adjacent winding;
- hard-error/impulse tail;
- H-sync survival;
- decoder relock events;
- output image quality;
- transport faults.

For RF controls, prove that the improvement exists in raw Q4/phase quality,
not only in a reported gain/RSSI number.
