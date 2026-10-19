# V5 strong-signal radius boost

C5VRX by Twotoz and the C5VRX contributors. This extends Direct Gain V5 (the
Direct Gain V3 core) and the fixed fine lanes. Leon asked for it on
2026-10-04. Hardware acceptance is pending.

## Why

PARLIO gives 4 bits per axis (16 cells). The Phase8 decoder turns each I/Q
cell into a phase, so the phase step depends only on radius divided by cell
size. On a strong signal, quantization is the main noise in the picture, not
the receiver. A larger IQ ring therefore gives a finer phase: less grain and
more detail.

Host model: fine lanes {9,7,6,5}, ideal FM ring, Gaussian noise. "Bad samples"
are samples with a phase error of more than 20°.

| Noise sigma (codes) | r=150 | r=200 | r=220 | r=235 | r=245 |
|---|---|---|---|---|---|
| 3 | 3.5° | 2.8° | 2.5° | 2.4° | 2.3° |
| 6 | 4.1° | 3.1° | 2.9° | 2.7° | 4.3° (0.55 % bad samples) |
| 12 | 5.8° | 4.4° | 4.0° | 6.0° | 11.9° (4.9 % bad samples) |

The V5 healthy band (P50 13..32) puts the ring at about 110–180 codes. Above
about 230 codes the ring folds over the ±256 window. With a DC offset, the
outer cells (|v| ≥ 224) fill up earlier.

Lane sets with gaps such as {9,8,6,5} or {9,7,5,4} were also modelled. They
are always worse than fine: one code then stands for several values, which
gives large errors at some phase angles. Fine is already the one useful gap
set, because bit 8 equals the sign bit inside ±256.

## Mechanism (`main/direct_gain_v3.c`)

V5 already measures P50 as a power in cells², about r².

- **Entry.** All of the following must hold for 100 consecutive normal HOLD
  windows (~20 ms at 200 us):
  - coherence ≥ 75;
  - no rail codes (`clip_pm` 0), origin ≤ 100 per mille;
  - a tight ring: `P95 - P50 ≤ 8`, and P95 still ≤ 50 at P50 40;
  - not settling, not anti-hunt damped, not in the hold-off;
  - coarse lane or a fixed lane, so never on adaptive finer lanes.
- **Band.** Healthy is P50 30..46 (r ≈ 5.4..6.8 cells, ~175–217 codes on
  fine), with P95 ≤ 53 and rail codes < 10 per mille. It is a zero-write zone,
  like the normal band.
  - Moves stop short of their target by the transition hysteresis, so the
    up target is 43 and the down target 38.
  - In the host plant one move lands at P50 39 (~200 codes) from P50 22.
- **Exit, immediately, back to the normal band.** Any of:
  - rail codes ≥ 20 per mille, P95 ≥ 59 or P50 ≥ 50;
  - coherence < 60, carrier loss, no carrier or saturation;
  - a lane change, or boost disabled.

  The normal V5 logic then moves the gain back into 13..32 in the same
  window. Saturation keeps the existing emergency path.
- **Hold-off.** No re-entry for 200 ms after an exit. Each further exit
  within 10 s doubles it, up to 3.2 s.
- **Unchanged:**
  - the Phase8 decode and LUTs, because the phase does not depend on radius;
  - the level servo and DC recentring;
  - native AGC, the acquisition mask, and all other gain owners.

## Controls

| Key | Action |
|---|---|
| `y` | Toggle the boost (NVS `c5vrx4/radius_boost`, opt-in since 2026-10-04: off by default), reboot |
| `!` | `RADIUS_BOOST` line: enabled/active, entries/exits, streak, live P50/P95/rail/coherence |

## Evidence (host)

`tools/test_direct_gain_v3.c` uses a measured-gain plant at 0.5 dB per index:

- disabled: no boost and no writes;
- 20 ms strong ring, then one or two moves into 30..46 (P50 39), then 1 s
  steady with zero writes;
- rail codes: exit and a move back into 13..32 in the same window;
- no re-entry within 200 ms;
- a +3 dB level jump exits, and the hold-off doubles;
- saturation takes the emergency path;
- a wide ring, low coherence, rail codes or a too-high predicted P95 never
  enter;
- disabling drops an active boost.

The normal band reproduces the earlier V5 constants exactly; all earlier V3
tests are unchanged and pass.

## Hardware gates

1. On a strong, steady signal (bench, VTX near): `!` shows `active=1`.
   Compare grain and fine detail with `y` off on the same channel, gain
   situation and camera.
2. Rail and fold: record `RADIUS_BOOST` exits and `DG3_OBS` `clip_pm` while
   moving or turning the quad close in. There must be no visible sparkle at
   entry or exit.
3. Gain writes: the writes counter must not rise on a steady carrier (no
   pumping). Fades must not cause hunting; the hold-off must grow.
4. With a DC offset (`!` receiver DC), the boost should stay off or exit,
   not fold.

## Prior evidence (read before trusting the model)

`docs/range-max.md` records an earlier review model with only ~0.9 dB
chroma-noise benefit from radius 6.5 vs 5.5 cells at C/N 20 and almost none at
C/N 12, and a hardware L0/L1/L2 lane A/B (a lane change rescales the radius in
cells) with no visible difference. The boost's gain therefore exists only
where quantization clearly dominates (very strong, clean signal), and may be
invisible. If the bench A/B with `y` shows no difference, turn it off: it
works closer to the fold edge for nothing.

## Limits

- The gain is a model result. The real noise, DC and fading margin around
  ±256 decide whether the band holds without visible folds.
- The benefit exists only when quantization dominates, so on strong signals.
  It gives no range at weak signal, where V5 stays in its normal band.
