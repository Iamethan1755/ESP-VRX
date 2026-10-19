> Current output update: [CVBS_OUTPUT.md](CVBS_OUTPUT.md). The Phase8/winding
> oracle below still applies; final DAC scaling now defaults to CVBS150.
> Legacy scaling remains selectable with M. HISTORY toggles with lowercase h.

# Phase8 / 75 ns trajectory unwrap — issue #144

Implemented on PR #142's gain/RF/lane baseline. Direct Gain V5 remains the
default; PR #143's separate lane handover changes are not included in this
comparison. Version: `4.0.0-exp-unwrap75`. Default decoder: STATIC.

## What is different

Four consecutive IQ bytes P, M1, M2, C now contribute to every output.
The endpoint decoder is still full Phase8. Sign bits from all four samples
resolve the quadrant trajectory; the middle samples do not estimate the
final video amplitude. Three instructions consume three IQ bytes and emit
three repetitions of one six-bit DAC code. There is no CPU sample processing.

For a decoded adjacent step strictly below 90 degrees, quadrant motion is
-1, 0 or +1. Sum these three quadrant motions into n:

| Class | Meaning | Endpoint branch |
| --- | --- | --- |
| 0 | abs(n) <= 1 | Ordinary endpoint branch |
| 1 | n >= 2 | Positive trajectory; lift negative endpoint by 256 |
| 2 | n <= -2 | Negative trajectory; lower positive endpoint by 256 |
| 3 | Any opposite-quadrant adjacent pair | Ambiguous; neutral DAC voltage |

This removes the +/-180 degree **endpoint** wrap within the proved domain.
It does not make arbitrary noisy paths unambiguous. A true adjacent jump
outside the bound can alias without an opposite-quadrant indication; no
deterministic guarantee is claimed there. In particular this is not a full
40 MS/s arbitrary-adjacent-FM estimator under a +/-180 degree adjacent bound.

The exhaustive discrete domain is abs(adjacent step) <= 63 Phase8 bins:
88.59375 degrees, equivalent to 9.84375 MHz at 25 ns. Its maximum three-step
sum is +/-189 bins (265.78125 degrees). The old span75 limit was +/-128 bins.
The bound is on **decoded** phases, including noise and quantization; it is
not a guarantee for a physical VTX nominally below 10 MHz.

## Register and LUT fit

Use 1024 x 16 bits = 2048 bytes. LUT16 keeps Counter A accessible alongside
LUT data, and also allows FIFO bits32..47 alongside LUT data. LUT32 would
overlap both sources, so simply adding trajectory fields to PR #142 fails.

LUT address bits are output16..25. Banks 1/3 decode raw IQ into biased phi8
and minus phi8. Banks 0/2 contain independent DAC6 and trajectory2 planes.
The two even banks are identical.

| Bundle | LUT address | Counter operation | Read / write |
| --- | --- | --- | --- |
| accumulate | Four IQ sign pairs + even bank | ADDCTIAH | 16 / 8 bits |
| map_delta | Counter delta top6 + trajectory2 + even bank | LDCTIAH | 8 / 16 bits |
| decode_next | Next endpoint IQ byte4 + odd decode bank | JMP | 0 / 0 bits |

In accumulate, the endpoint currently being decoded is FIFO byte4. Its
previous endpoint and middle samples are bytes1/2/3. Reading three bytes
makes the following endpoint byte4 and preserves all four samples for the
next iteration. The upper FIFO and LUT sources coexist in LUT16 mode.

Output bit24 must be zero during both counter operations to select an even
bank. Clearing it removes one endpoint parity bit from each counter operand.
Those two bits are retained separately in O6/O7. They use the two physically
unconnected TX bits; the actual DAC still uses the existing pins0..5 only.

If e is Counter A.high after accumulation, the exact biased endpoint is
`(e + previous_parity + current_parity) & 255`. Applying the trajectory branch
to `e - 128`, then adding both parity bits, gives the exact adjacent reference
throughout the proved domain. This split representation retains all eight
endpoint bits; it is not a claim that A.high alone contains a full delta.
LDCTIAH/ADDCTIAH never involve the low counter byte, so LUT address bits cannot
introduce a low-byte carry into phase arithmetic.

## Final DAC transfer

The final LUT address drops e's low two bits and endpoint parity restoration.
It selects a four-bin midpoint. The difference from the full unwrapped
reference is at most two Phase8 bins (104.167 kHz at span75). Thus low endpoint
bits **can** change the ideal DAC choice: this is an explicit final-transfer
quantization trade-off, not bit-exact equivalence to PR #142's DAC LUT.
Full endpoint resolution is retained before this final transfer.

Corrected outer trajectories saturate at the appropriate voltage rail; they
are never reduced modulo into a six-bit output. Inside the original video
span, preserve the nominal span75 slope. Select the nearest nominal loaded
voltage of the existing 8.2k/3.9k/2k/1k/470R/240R network. No measured board
voltage calibration or fitted VTX deviation is available: those acceptance
items remain open. Saturation does not display additional extreme deviation;
it avoids the old positive-to-negative catastrophic inversion.

STATIC is selected when the new `c5vrx4/unwrap_hc` key is missing. Serial H
persists that key and reboots between STATIC and the existing optional
near-origin history decoder. This makes the first hardware test independent
of the speculative history prior. T identifies `pipeline=unwrap75_static`
and reports phase precision, bound and final DAC address resolution.

## Host evidence (2026-10-01)

Issue #144 explicitly requested these tests before live firmware:

- `unwrap_oracle.c`: all **524,386,048** allowed Phase8 trajectories;
  **zero wrong values, zero ambiguous collisions**. 503,554,048 ordinary
  trajectories match the wrapped endpoint exactly before DAC transfer;
  20,832,000 winding trajectories match the exact adjacent sum.
- `test_unwrap.py`: actual generated bit routing, LUT banking and partial
  counter operations, both STATIC and HISTORY; 24,000 iterations each,
  72,000 input/output bytes each, crossing the 32,768-byte ring twice without
  reset. All three physical DAC codes per iteration agree.
- 20,000 reproducible noisy Q4 paths, including near-origin radius0.5;
  12,597 meet the decoded-phase bound, zero errors within the bound.
  2,819 paths explicitly classify ambiguous. The remaining paths outside
  the bound are not correctness evidence or guaranteed safe.
- Both programs assemble to exactly three instructions and 2048 LUT bytes.
  ESP-IDF v6.0.2 firmware builds successfully, app size `0x118ac0`.
  Serialized LUT headers and bytes match the generated tables. No hardware test yet.

The assembler's console message calls its 1024 input LUT entries "32-bit
words" even for LUT16; the serialized binary packs them into 512 32-bit
words. Check its header, rather than that generic message, for resource size.

Run the exhaustive C oracle with a host compiler:

```sh
gcc -O3 -Wall -Wextra -Werror unwrap_oracle.c -o /tmp/unwrap-oracle
/tmp/unwrap-oracle
python test_unwrap.py
python generate_phase8.py
idf.py -DIDF_TARGET=esp32c5 build
```

## Hardware gates still open

Compare PR #142 STATIC with this STATIC at matched gain, lane and RF settings.
Cross the old +/-6.667 MHz boundary deliberately; examine PAL/NTSC colour,
detail and fade sparks, and measure TX FIFO/40 MHz cadence. Host models do
not establish physical FIFO behavior or sample-gapless RF capture. Streaming
state crosses ring boundaries in the program; physical continuity needs the
board measurement. Span75's chroma attenuation and 13.333 MS/s hold remain.
No additional RF sensitivity or useful-range improvement is claimed yet.
