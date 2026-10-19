> Donor research record. Current integration defaults and scope are in README.md and INTEGRATION.md.

# Fixed ultrafine comparison

The operator prioritizes range over grain/video quality and explicitly asked
to keep the finest available IQ lanes on at every distance, to evaluate the
picture. This replaces the unfinished adaptive-lane proposal for this build.
Version: `4.0.0-exp-ultrafine`, based on PR #145's unwrap75 pipeline.

## Implemented

- Default: permanently select verified lane2, ADC bits {9,6,5,4} on both I/Q.
  Its ADC step is16 codes, with an unfurled window of [-128,127]. Coarse uses
  step64 and window[-512,511]. These are quantizer units, not added RF gain.
- Select it before PARLIO RX starts. Every later `rf_set_iq_lanes()` request
  resolves to lane2 while the fixed mode is selected. No ordinary lane
  changes, per-line switching or DMA-boundary resets occur in this mode.
- Works with Direct Gain V5, manual gain, and native AGC. Manual still holds
  the chosen analog gain; native still owns its analog gain exclusively.
- V5's internal lane state matches physical lane2. Disable its lane-changing
  decisions and lane fold guard. Its analog overload path remains available:
  disabling the lane drop must not accidentally short-circuit analog gain
  reduction. Existing BB-before-RF emergency reduction was already in this
  revision and is preserved.
- Noise statistics remain observable; their cap does not change the fixed
  lane. This deliberately allows more receiver-noise detail to reach video.
- Serial Z persists `c5vrx4/force_ultra` and reboots between fixed ultrafine
  and the PR #145 baseline policy. Missing key selects fixed ultrafine.
  Baseline: V5 automatically chooses lanes; manual/native use coarse.
- T reports the actual lane, ADC step, window and fixed/baseline policy.
- Unwrap75, STATIC/HISTORY selection, DAC transfer, eight capture pins and
  three-bundle / 2 KiB LUT cadence retain the PR #145 implementation.

## Why this experiment first

PARLIO RX has eight data lines. A 16-bit input setting cannot provide ten or
twelve simultaneous IQ lanes on ESP32-C5. Faster capture alone re-reads the
same eight selected signals. A broader-IQ architecture would need a verified
source-side serializer/multiplexer plus new bandwidth, timing and DSP proofs.
There is no such proven source in this build.

The verified fine and ultrafine sets already expose lower physical ADC bits.
The baseline restricts their use to active Direct Gain and enters finer
lanes only at maximum total analog gain. Manual and native experiments thus
can remain coarse even with a small IQ envelope. Fixing the geometry gives
a concrete comparison of that information loss, without an additional
policy or hardware dependency.

Finer quantization cannot restore carrier/noise ratio lost upstream. ADC
input noise and quantization noise are separate limitations; the existing
maximum-gain noise measurement already showed substantial ultrafine noise.
The fixed mode tests whether finer capture provides useful phase information
despite that noise. No predetermined dB/range improvement is claimed.

Sources: [Espressif C5 capabilities, IDF v6.0.2](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/include/soc/soc_caps.h),
[Analog Devices: ADC input noise](https://www.analog.com/en/resources/analog-dialogue/articles/adc-input-noise.html),
[Analog Devices: receiver sensitivity](https://www.analog.com/en/resources/technical-articles/improving-receiver-sensitivity-with-external-lna.html),
the physical lane mapping in `main/rf.c`, and `docs/range-max.md`.

## Distinguish two kinds of wrapping

Unwrap75 resolves phase endpoint winding within its stated adjacent-step
bound. It does not reconstruct the ADC bits omitted by ultrafine capture.
Outside [-128,127], the selected nibble can fold and report a different IQ
angle. Phase8's tolerance of ordinary amplitude clipping does not prove
tolerance of this folding. The operator asked to keep this lane enabled
even there: the build intentionally has no automatic coarse fallback.

## Comparison

Use the original Wi-Fi antenna and the same VTX/channel/setup. Operator
reported usable PR #145 video, including some picture at gain-index2, but
poor usable range. A separate 5.8 GHz dipole performed worse than the stock
antenna. These observations do not isolate RF loss from DSP/gain behavior.

First compare fixed ultrafine versus baseline with Z at matched manual gain,
both close and near picture loss. Then compare V5 automatic gain and native
separately. With native active, the existing 20 us / 1 ms pacing is retained.
Record picture loss, grain, sparks, gain and T/p telemetry. Leave Z on the
better-performing policy afterward. Strong-signal folding, weak-signal
improvement and physical transport continuity remain hardware questions.

No new tests were added or run for the lane policy. The prior unwrap oracle
does not prove this RF/ADC experiment; compiler success is only build fit.

Build record: ESP-IDF v6.0.2 completed successfully on 2026-10-01;
application size `0x118d60`, version `4.0.0-exp-ultrafine`. Hardware assessment pending.
