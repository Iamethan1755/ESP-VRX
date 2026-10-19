> Donor research record. Current integration defaults and scope are in README.md and INTEGRATION.md.

# Historical PR #142 design

The current firmware extends this baseline with [trajectory unwrap](UNWRAP75.md).
The table layout, history default and modulo limit below describe PR #142.

# Three-bundle Phase8 with bounded phase history

Implemented as a comparison experiment on top of PR #133 (Direct Gain V5).
The 2026-10-01 request explicitly authorizes this three-bundle experiment;
the unmerged target specification in PR #141 remains a different architecture.
Neither document establishes additional usable range.

## Resource and arithmetic layout

Each program has three instruction bundles and one 512x32-bit LUT: exactly
2048 bytes. The table's independent bit planes are:

| Field | Bits | Address meaning |
| --- | --- | --- |
| Biased current phase `(128 + phase) mod 256` | 0..7 | raw IQ + history semicircle |
| Negative current phase `(-phase) mod 256` | 8..15 | same decoder address |
| Nominal DAC voltage mapping | 16..21 | biased delta; identical in both banks |

In 32-bit LUT mode its address comes from output bits 16..24. Counter A's
high byte can be loaded/added from bits 24..31 independently of its low byte.
The overlapping address bit 24 is harmless for the DAC lookup because both
DAC planes are identical. Decoder planes can differ across that bit.

| Bundle | Counter operation | LUT operation | Stream work |
| --- | --- | --- | --- |
| accumulate | ADDCTIAH: `-previous + 128 + current` modulo 256 | result unused | read16, write8 previous DAC |
| map_delta | LDCTIAH: retain `-current` | biased delta -> DAC | read8, write16 same DAC twice |
| decode_next | jump to accumulate | next raw byte + retained `-current` MSB -> phase | retain new DAC |

Each iteration consumes three IQ bytes and emits three identical DAC bytes.
Endpoint spacing is 75 ns, with 13.333 MS/s unique output and 40 MHz DAC
transport. The first pipeline iterations prime state; no DMA wrap resets the
decoder, counters or iteration. A 32768-byte ring need not divide by three.
Hardware FIFO timing and high-byte counter behaviour have not been proved by
this build; the repository's prior low-byte counter probe is not that proof.

## Decoder and comparison control

`generate_phase8.py` emits two programs with the same transport and DAC plane:

- STATIC: exact centred-cell Phase8 formula from `tools/gen_phase8_hr.py`.
- HISTORY: only cell-centre radius squared <= 2.6 is eligible for correction.
  Compute a circular posterior mean using Gaussian cell likelihoods and one
  previous-phase semicircle. Corrections are capped at eight Phase8 bins
  (11.25 degrees); all other cells keep the exact static phase.

The prior assumes uniform previous phase within the retained semicircle and
uniform frequency increments within +/-6 MHz over 75 ns. Likelihoods average
carrier radii 0.5/0.75/1/1.5/2/2.5 lane cells and axis noise sigma 0.56/0.95
cells. These are explicit design assumptions informed by the repo's coarse
noise and fine-lane cap observations, not a fitted board/VTX distribution.
No picture training, gain writes, runtime table fitting or output-sample repair
is introduced. The model is sensitive to CFO/deviation and noise mismatches.

History is initially enabled. Serial `H` persists `c5vrx4/phase8_hc` and reboots
between HISTORY and STATIC. Gain ownership, profile, lanes and DAC mapping
stay the same across that decoder comparison. `T` reports the selected mode.
Existing `N` / RF-profile controls still select native versus firmware gain;
do that in a separate comparison. V5 is the default inherited from PR #133.

## Lessons from PR #141

- The original nominal 64-to-64 resistor inversion is an identity map. It is
  not a measured calibration or evidence of reduced grain. This program maps
  256 frequency states to 64 codes using the nominal network, also not a
  measured loaded calibration.
- Longer endpoint averaging and the zero-order DAC hold both attenuate chroma.
  PR #141 quotes combined PAL losses of 3.28 dB at 75 ns versus 1.43 dB at
  50 ns. The third instruction does not remove that trade-off.
- Per-sample gain/state lanes and PHY filter/ENBW characterization are useful
  research directions, but are not implemented here or established live APIs.
- The HC5 bench's click reduction is not evidence for this HC8 semicircle
  decoder. No 1.5-2 dB result or 6 dB objective is transferred to this build.

## Limits

Phase resolution is 1.40625 degrees (52.083 kHz per bin at 75 ns), versus
5.625 degrees (208.333 kHz) in the prior Phase6 prototype. It remains an I4/Q4
source with V5's existing finer-lane policy. Finer phase codes do not create
additional ADC information. The intermediate IQ bytes are still skipped.
Frequency wrapping remains +/-6.667 MHz; output aliasing, chroma attenuation,
feedback bias and fade recovery require hardware assessment. The mapping uses
the full signed discriminator span, not a measured VTX-deviation calibration.
Ordinary Phase5/Phase8 software diagnostics do not mirror the HC8 DAC path.

No tests or simulations were added or run for this implementation. A compiler
and assembler success establishes encoding/resource fit, not picture quality,
counter correctness, noise reduction, or range gain.

Build record (2026-10-01): ESP-IDF v6.0.2 Docker build completed successfully,
version `4.0.0-exp-phase8-hc75`, application size `0x118a80` bytes. Both
programs assembled as three instructions and 512 32-bit LUT words. This
firmware has not been flashed or assessed on hardware.

Sources: [PR #141](https://github.com/Twotoz/C5VRX/pull/141),
[PR #133](https://github.com/Twotoz/C5VRX/pull/133),
[BitScrambler assembly](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-reference/peripherals/bitscrambler.html),
ESP32-C5 TRM v1.1 section 44.5.1 (LUT addressing) and tables 44.5-10/11
(LDCTI/ADDCTI), plus the repository's Phase8 LUT generator and range research.
