# Native AGC acquisition mask

C5VRX by Twotoz and the C5VRX contributors. Extends the project's native-AGC
research (`docs/native-agc-v2.md`, `docs/native-agc-paced.md`, the PR #154
`native-agc-analog-patch.md` audit and the PR #122 AGC GUARD lab) and the
MODEM_DIAG tap map (`tools/phy_phase_tap_probe.md`). The RF dump word layout
(gain index in bits 20..27, AGC state machine in bits 28..31) is from
[ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr). Hardware acceptance is
pending for everything below.

## Problem

Native C5 AGC is an 802.11 packet AGC. On a continuous FM carrier it detects a
"packet" every ~25-50 us, gives up, and acquires again: a ~2-3 us gain walk
from the start gain (82) in ~0.6 us steps, e.g. 16 -> 18 -> 58 -> 34 -> 10, that
traps on a different gain each time. During the walk the IQ is saturated or
starved, which is the line noise and grain seen with native AGC. There is no
in-packet tracking control: stopping detection freezes the gain. Register
profiles, start gain, compensation offsets, the paced gate and firmware
pinning did not remove it. Direct Gain V5 holds a clean gain, but it can only
correct after a window has already gone bad (0.2-1 ms, several lines).

## Mechanism

Native AGC keeps tracking at its own speed (< 1 line); only the acquisition
samples are hidden.

1. **Witness.** DIAG[20..31] mirror dump bits 20..31. The PR #122 probe saw
   them static and bit-exact (gain forced to G52: pattern 52, state 1). One
   AGC state bit is expected to mark the walk. Which bit and polarity is
   measured, not assumed.
2. **Calibration (`*`, or automatically at the first native carrier).**
   - Native AGC, pacing off.
   - For each state bit DIAG[28+n], the eight PARLIO lanes capture DIAG[20..26]
     (gain index) plus that bit, 24 4092-sample windows each (~0.2 s garbage
     video).
   - Gain changes closer than 4 us form one acquisition. Among the bits and
     polarities active on at most 5 % of trapped samples (guard and settle
     zones excluded), the one covering most acquisition samples is chosen if:
     - it is active on more than 50 % of acquisition samples;
     - at least 8 acquisitions were seen.
   - Board, 2026-10-06 (A1, VTX near): 13.2 % of samples in 3.1 us walks, walk
     minimum G19, trapped G32..44. DIAG[31] inverted separates widest (95 % of
     walks) but is set on 20 % of trapped samples; DIAG[30] covers 57.5 % of
     walks and is never set while trapped. The first rule (largest separation,
     then >= 80 % coverage) therefore picked DIAG[31] and stored nothing.
   - The result goes to NVS `c5vrx4/agc_flag`, and the board reboots to apply
     it.
   - The report also gives the acquisition rate and duration, the lowest gain
     reached during walks, the trapped gain range, and the flag lead/lag
     relative to the gain changes.
3. **Lane.** PARLIO data bit 0 (the fine Q LSB, DIAG5) carries the witness,
   inverted in the GPIO matrix if needed. Q keeps {9,7,6} and is decoded at the
   centre of its two-cell step; I keeps {9,7,6,5}.
4. **Program** (`c5vrx4_phase8_static_mask*.bsasm`, STATIC decode, six of
   eight slots):

   | Slot | Bundle | Work |
   | --- | --- | --- |
   | 0 | init | `ldctdb 0`: counter B is the all-clear reference |
   | 1 | accumulate | unchanged (ADDCTIAH) |
   | 2 | map_delta | unchanged, writes one DAC byte; bits 8/9 = flag(C), flag(M1') |
   | 3 | decode_next | emits the span's first DAC byte; `if BL=O8 accumulate` |
   | 4 | hold_reseed | repeat last DAC; A = -phase of the span's own endpoint (LDCTIAH) |
   | 5 | hold_next | repeat last DAC; identity lookup in bank 3; `jmp decode_next` |

   - A span holds when its P or its first middle sample is flagged. The
     reseed makes the first clean span exact again.
   - A run starting on M2 or C is caught one span later. The C5 assembler
     forbids reading input bits 32..63 in a bundle that also uses counter
     bits, which limits the look-ahead.
   - The calibration reports the flag lead. With a lead of 2 or more samples
     no walk sample leaks.
   - Every path is three bundles per three IQ bytes and three DAC bytes, so the
     40 MS/s cadence and `[D,D,D]` output are unchanged.
   - The even banks (DAC plane and trajectory) are unchanged, so the level
     servo still works. Bank 3 is the hold identity plane, so DC recentring
     refuses to run while masking.

## Evidence (host)

- `test_agc_mask.py` runs the generated programs and a Q3 reference through
  `tools/bs_model.py`, which now supports `if`/`ifn` and B-counter
  conditions. It covers STD150, CVBS150 and legacy, with runs of 1..136
  samples at every phase and back-to-back:
  - no flags: bit-exact with the unmasked pipeline (one byte earlier);
  - flagged spans: exact hold;
  - clean spans after a run: exact;
  - at most 3 bundles per span.
- The ESP32-C5 bsasm accepts the program (6 instructions).
- `tools/test_agc_witness.c` covers:
  - finding the bit and its polarity, the lead, walk depth and rate;
  - refusing a flag that is set on trapped samples;
  - no decision under a forced gain.

## Operation

| Key | Action |
| --- | --- |
| `N` | native AGC (existing; reboot) |
| `*` | witness calibration (VTX on, native); stores and reboots |
| `\|` | toggle the acquisition mask (NVS `agc_mask`, default on), reboot |
| `!` | `AGC_MASK` line: native/enabled/active, flag, last calibration, live flag share |

- The mask is active only with native AGC, a stored witness and STATIC
  decode.
- Without a witness, the first native boot with ~3 s of carrier calibrates
  automatically (at most three tries, one a minute) and reboots once.
- The pacing gate stays off while masking, because every acquisition must
  be visible to the mask.
- The live flag share should sit near the measured acquisition share (~6-13 %).
  Much more means the picture is mostly held: switch off with `|`.

## Hardware gates

1. `*` with the VTX on.
   - A state bit with clean separation must exist; otherwise nothing is
     stored.
   - Record the acquisition rate and duration, the walk minimum, the trapped
     range, and the lead/lag.
2. Native with mask vs native without (`|`) vs Direct Gain V5:
   - dashes, grain and line-to-line contrast;
   - sync stability and colour;
   - fast level changes (close passes, rotation).
3. Live flag share vs the calibration share; no frozen picture.
4. Lag > 0: settling after the flag falls may still leak. Note visible dots
   at the end of acquisitions.
5. Q3 cost: compare fixed-gain picture detail with and without the flag lane.

## Limits

- Holding 2-3 us every 25-50 us repeats one value across those samples. Fine
  detail and colour cycles inside the hold are lost, and a hold during H-sync
  shortens its visible edge. This is a concealment, not a recovery.
- One Q bit is spent on the witness.
- DC recentring is off while masking.
- The CPU snapshot observers (sync/standard, AFC, level servo, `J`) decode Q3
  at its cell centre while masking, like the program; the V5 power/coherence
  observer still reads the flag bit as Q LSB, a half-cell Q error in
  their statistics.
