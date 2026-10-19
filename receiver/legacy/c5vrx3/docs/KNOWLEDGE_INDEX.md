# C5VRX knowledge index

Use this page to find evidence before changing the RF or video dataplane.
Current physical results override historical assumptions; negative experiments
remain valuable because they prevent repeated dead ends.

## Project lineage

```text
Original C5VRX
  -> RF/PHY reverse engineering
  -> receiver-console and finite IQ experiments
  -> AV/DAC, USB, LP-core, REGDMA, and restart diagnostics
  -> direct recovered-CVBS architecture
        |
        v
Modern C5VRX
  -> one-start pre-trigger writer proof
  -> live MODEM_DIAG Q4/I4 mapping
  -> PARLIO RX at 40 MS/s
  -> TX BitScrambler WBFM and 20 MS/s DAC
  -> first locked live NTSC
  -> phase5 color/static improvements
  -> Zero-EOF C5VRX-3 continuous dataplane
  -> vendor-table ARC + PRE-Q4 gain characterization
  -> ARC V3/V5 control research on raw-Q4 evidence
  -> Golden Phase5 remains the live quality reference
  -> exact-adjacent / LIFT / Phase6 / Polar research remains hardware-gated
```

## Current authority

| Subject | Start here |
|---|---|
| Cross-PR canonical findings, corrected assumptions, hardware failures, and experiment disposition through PR #77 | [pr-derived-findings.md](pr-derived-findings.md) |
| PRs #190/#192: scoring corrections, shared-LUT limits, frontend/controller faults, U85Q captures, sequence inference and rejected V4 experiments | [pr190-key-findings.md](../../../docs/pr190-key-findings.md) |
| Other recent PRs: immediate DC hold, staged gain recovery, build identity, U85Q AFC placement and direct-IQ timing limits | [recent-pr-receiver-findings.md](../../../docs/recent-pr-receiver-findings.md) |
| Phase5-360 comprehensive findings, MODEM_DIAG lane analysis, Counter-A subtraction, and PR #83-#94 resolution | [phase5-360-comprehensive-findings.md](phase5-360-comprehensive-findings.md) |
| Phase5-360 architecture specification, 2-bit quadrant oracle, and 16-bit word packing | [phase5_360_architecture.md](../tools/phase5_360_architecture.md) |
| Golden360 / Adjacent50 exact-capacity proof, one-middle-bit impossibility result, and reproducible oracle | [golden360-feasibility.md](golden360-feasibility.md) |
| ARC V5 predictive local-gain model and persistence rules | [arc-v5-autotune.md](arc-v5-autotune.md) |
| Phase8 range regression, Q4 origin-collapse oracle (`E`/`P8ENV`) and native hardware AGC experiment (`N`), issue #119 | [phase8-range-envelope.md](phase8-range-envelope.md) |
| Native AGC findings (PR #122): ~21 us re-acquisition per sample from the RF dump, register sweep, offsets, paced native, why Direct Gain V4 is the default | [native-agc-v2.md](native-agc-v2.md), [native-agc-paced.md](native-agc-paced.md), [pre-native-noise-audit.md](pre-native-noise-audit.md) |
| Proven RF writer, SRAM visibility, MODEM_DIAG mapping, rates | [continuous-iq-findings.md](continuous-iq-findings.md) |
| Realtime contracts and source abstraction | [realtime-iq-plan.md](realtime-iq-plan.md) |
| Current image-quality path and proof gates | [image-quality.md](image-quality.md) |
| Corrected issue #6 winding/static measurements | [issue-6-static-analysis.md](issue-6-static-analysis.md) |
| Static root causes, digital filtering limits, analog capacitor de-emphasis | [static-reduction-and-filtering.md](static-reduction-and-filtering.md) |
| Issue #11 20 MS/s digital CVBS stream measurement and timing proof | [issue-11-cvbs-analysis.md](issue-11-cvbs-analysis.md) |
| Fix for CVBS horizontal line jitter, trajectory wrap, and static | [fix-cvbs-jitter-and-static.md](fix-cvbs-jitter-and-static.md) |
| Diagnostic LED firmware, empirical findings, 9-line raster beat, and 40 MS/s DAC | [diagnostic-led-firmware.md](diagnostic-led-firmware.md) |
| True 40 MS/s DAC reconstruction and 80 MS/s decoupled rate expansion roadmap | [issue-11-cvbs-analysis.md](issue-11-cvbs-analysis.md) |
| Issue #17 True 40 MS/s cadence, adjacent25 failure modes, and interleaved demodulation | [issue-17-true40-cadence-and-interleaved-phase5.md](issue-17-true40-cadence-and-interleaved-phase5.md) |
| Dual-loop self-calibrating AGC, FM phase coherence ($Q_{\text{phase}}$), and noise trap immunity | [dual-loop-adaptive-gain-optimizer.md](dual-loop-adaptive-gain-optimizer.md) |
| ESP32-C5 RF/BB/filter characterization, FFT placement probe, IQ centering and zero-write TRACK architecture | [esp32c5-rf-range-architecture.md](esp32c5-rf-range-architecture.md) |
| ARC vendor gain-table reconstruction, corrected private-PHY ABI, IQ calibration state and production freeze policy | [arc-receive-chain.md](arc-receive-chain.md) |
| PRE-Q4 self-noise, full vendor-table FAR sweep and fresh-calibration lab procedure | [pre-q4-lab.md](pre-q4-lab.md) |
| C5VRX-3 independent PAL/NTSC menu, waveform tests, decoder hypotheses and remaining hardware validation | [c5vrx3-menu-and-raster-architecture.md](c5vrx3-menu-and-raster-architecture.md) |
| Wiring and remaining physical tests | [hardware-test.md](hardware-test.md) |
| Accepted historical donor primitives | [proven-donors.md](proven-donors.md) |
| Licensing and contributor evidence | [licensing.md](licensing.md) |

For exact behavior, read `/main` together with these documents. A document may
describe a diagnostic mode or future proof gate rather than the default build.

## Historical evidence

- [Original repository README](../legacy/c5vrx1/README.md) — preserved visual
  and project history; its old status claims are not current.
- [Archive research](../legacy/c5vrx1/research/) — RF dump format, PHY reverse
  engineering, CVBS/DAC work, USB receiver-console sessions, and architecture
  experiments.
- [Archive diagnostics](../legacy/c5vrx1/diagnostics/) — bounded hardware probes
  and reproduction projects.
- [Archive firmware profiles](../legacy/c5vrx1/firmware_profiles/) — historical
  build configurations only.
- [Archive hardware](../legacy/c5vrx1/hardware/) — early BOM and board research;
  verify pin/resistor values against current docs.
- [Preserved GitHub issues](legacy-issues/README.md) — all archive issues and
  their later disposition.
- [High-signal archive PR discussions](legacy-issues/pull-requests.md).

## Search guide

- **ESP32-C5 RF / PHY / dump RAM:** current continuous-IQ findings, then archive
  `research/adc-dump-format.md` and `research/reverse-engineering.md`.
- **MODEM_DIAG / Q4-I4:** current continuous-IQ findings and `/main` mappings;
  archive material predates the final live-source proof.
- **WBFM / adjacent phase / filtering:** current image-quality and realtime
  docs, then legacy issues #27 and #28.
- **CVBS / PAL / NTSC / DAC:** current hardware test, then archive analog-first,
  CVBS proof, and resistor-model material.
- **PARLIO / GDMA / BitScrambler:** current source/findings, then legacy issues
  #20, #22, #24 and the preserved PR notes.
- **Continuous capture:** distinguish writer-pointer continuity, readable sample
  continuity, and coherent RF-time continuity. They are separate claims.
- **Old failed approaches:** start with legacy issue/PR dispositions and
  [PR-derived findings](pr-derived-findings.md) before reviving finite rearm,
  active dump-SRAM reads, RX-attached BitScrambler, simultaneous RX+TX
  BitScrambler, >40 MB/s live TX overclocking, raw-Q4 observers on transformed
  rings, or a synthetic raster as the normal receiver.
| Range max: dB budget, noise-referenced lanes, BW gear, sync flywheel + colour killer, two-bundle demod limits, hardware plan | [range-max.md](range-max.md) |
