# Phase8 range: Q4 origin collapse and native AGC (issue #119)

Status (2026-09-30): **superseded -- Direct Gain V4 is the default gain
owner.** Native hardware AGC was the default after the walk test below; later
per-sample measurements (docs/native-agc-v2.md) showed it re-acquiring every
~21 us on a different gain, and the walk-test advantage came from Direct Gain
V3 parking at G62 without a carrier, which V4 fixes. Native AGC is opt-in
(`N` / RF page). The walk-test and bench-session findings below remain valid.

## Walk test, 2026-09-29 (Phase8 build, A1 5865 MHz, one VTX)

Raw rows are in the PR discussion. `E` readings, operator picture rating:

| Position | Gain owner | p50 / p95 | central | origin | clip | hard | picture |
|---|---|---|---|---|---|---|---|
| close | Direct Gain V3 (G35-38) | 19-31 / 27-41 | 0 | 0 | 0 | 0 | good |
| close | native | 1 / 3 | 61-73% | 95-98% | 0 | 19-27% | **perfect** |
| step 1 | native | 5-7 / 33-57 | 15-49% | 19-50% | 2-4% | 1.4-5% | great |
| step 2 | native | 7 / 27-49 | 1-4% | 21-29% | 0.6-1.8% | 0.1-0.3% | great |
| step 3 | native | 5 / 13 | 10-13% | 44-49% | 0 | 1.8-3.5% | quite bad |
| step 3 | Direct Gain V3 (stuck G62, 30 s) | 1 / 1 | **100%** | 100% | 0 | 12-22% | (worse) |

Findings:

- **Native AGC runs without Wi-Fi packets.** With zero firmware writes, Q4
  amplitude *rose* as the VTX moved away (p50 1 -> 7) and held p50 ~7 over two
  steps. It targets roughly a 2-2.5 cell radius, well below Direct Gain V3's
  4-5 cells. Near the edge p50 fell to 5: it ran out of gain.
- **Native AGC reaches more gain than forced gain 62.** At step 3 forced G62
  never left the four centre cells, while native still produced ~50% of samples
  beyond radius 1. Direct Gain V3 also made zero writes for 30 s while fully
  collapsed. Its ceiling or no-carrier handling needs a separate look.
- **Central occupancy alone is not failure.** At close range, 61-73% central
  occupancy with high SNR gave a perfect picture: the "hard" steps there are
  real quadrant crossings of the signal, not noise. The picture broke when
  central occupancy rose *together with* low SNR. At step 3 every hard pair had
  a central endpoint and outer pairs had none, as the hypothesis predicts.
- **Amplitude spread.** At steps 1-2 one ~100 ms capture spanned radius 0-7
  plus rail clipping, although analog FM is constant-envelope. Either gain
  switching or fading happens within a capture. The picture showed no visible
  pumping.
- `sync_q` / `std_valid` read 0 while the operator saw a perfect picture. They
  are not reliable video indicators for this analysis. Rate the picture by eye
  per step.
- The top byte of `0x600A702C` is the forced-gain index (it tracked firmware
  G35-38 exactly). Under native AGC it stays at the last forced value and does
  **not** reveal the gain the vendor loop chose. `0x600A7030` was identical in
  both modes. The Q4 radius is currently the only view of the native loop.

## Bench session 2, same day: known native-AGC defects

- **Lower resolution, structured noise lines, blacks shifting to blue.** Raw
  `Q` captures (VTX on) show each 1.6 us run is clean FM: steps are 95-100%
  within 45 degrees, and the amplitude is flat for long stretches. But the
  amplitude sits at a few discrete levels (radius ~0.7, 2.5, 2.9, 3.5, 5.1,
  5.7, rail). It steps between them within 1-2 samples, often mid-run, and
  probes ~25 us apart regularly differ. A VTX or fading cannot change
  amplitude that fast, so the vendor loop is switching gain several times per
  video line. Each switch plausibly shows up as a Phase8 disturbance on that
  line. This is not the random static seen with forced firmware gain.
- **White screen.** Captured live: the menu was closed and there were no
  transport faults. The Q4 vector had collapsed to radius ~1.6 (80% near the
  origin, no clipping), leaving no usable sync, so the monitor showed white.
  It recovered by itself this time. Earlier occurrences needed a restart.
- **The saturation-threshold registers (`0x600A7064` / `0x600A7114`) are not
  the target-level control.** They each hold four ascending bytes
  (20/36/53/77 and 18/30/40/46). Shifting all bytes by +4 in a reversible
  bench A/B changed nothing measurable, so the test knob was removed.
- DC is centred: a mean code of about -0.45 on I and Q is exactly what a
  zero-mean signal gives, because code n represents n+0.5.

Next: stop the native loop from switching gain within a line and from parking
low (find its re-trigger / hold controls), then tune its target level and ceiling (#117 Phase 4, e.g.
`phy_wifi_agc_sat_gain`) toward a larger Q4 radius at range, and find a
register that exposes the native gain decision.

## Hypothesis

The live path quantizes fixed signed `I[9:6]` / `Q[9:6]` to one Q4/I4 byte.
Cell `n` represents `n + 0.5`, so the four cells that touch the origin sit at
roughly +45, +136, -135 and -46 degrees:

```text
raw 0x00 -> LUT 32  (+45)     raw 0xF0 -> LUT 97  (+136)
raw 0xFF -> LUT 160 (-135)    raw 0x0F -> LUT 223 (-46)
```

Every move between two of these cells is a Phase8 step of 63..65 codes
(~88-91 degrees) or ~180 degrees. At weak RF the vector collapses into these
cells, so Phase8 turns quantizer noise into full-scale CVBS movement.
`tools/test_phase8_envelope.c` checks those LUT values and that every
central-cell move counts as a hard step.

## What was added

| Piece | Purpose |
|---|---|
| `main/phase8_envelope.h` | Read-only statistics over completed Q4/I4 bytes. Includes central-cell (4 cell) occupancy, DG3-compatible origin (power <= 4), clip, P50/P90/P95 (DG3 power units), radius histogram (whole cells 0..10), \|Phase8 delta\| histogram (16-code bins), hard tail (\|delta\| >= 60 codes), hard rate given a central endpoint vs given two outer endpoints, and a provisional annulus class. |
| Console `E` | Copies up to 32 completed-descriptor probes (128 x 64 adjacent samples) from the console task and prints one `P8ENV` row. It runs only on demand: no periodic task, no PHY write, no gain decision. |
| Console `N` | Switches between Direct Gain V4 (default) and opt-in native hardware AGC for the next boot (NVS key `c5vrx/native_agc`, 1 = native) and reboots. |
| `tools/p8env_sweep.py` | `capture` runs an interactive attenuation sweep (label a step, it sends `E` N times). `analyze` prints per-step medians, Spearman(central_pm, hard_pm), central/outer hard lift, first video-loss step, the empirical annulus, native gain-register movement and transport-fault movement. |

## Native AGC (default) and firmware fallback

The vendor AGC cannot be restored after `phy_disable_agc()` /
`phy_rfagc_disable()` (#117), so the gain owner is chosen per boot in
`rf_start()` before the PHY is used. Only NVS `c5vrx/native_agc` = 1 selects
native; missing or 0 means Direct Gain V4. In native mode:

- `phy_disable_agc()` / `phy_rfagc_disable()` are never called, not at boot
  and not on channel change.
- Forced gain and FFT scale are released once (`phy_force_rx_gain(false, 0)`,
  `phy_fft_scale_force(false, 0)`). No gain index is ever chosen.
- `rf_set_rx_gain()` and FFT force are refused and counted
  (`blocked=` in `P8ENV`). Offset retunes no longer reassert forced gain.
- `apply_rx_gain_tracked()` is a no-op, every profile is held in
  `ANALOG_AGC_MANUAL`, so no firmware controller makes gain decisions.
  Gain-owning console commands (`g F G R U S K + - k j a s m D I Y X`) are
  ignored. The persisted firmware AGC mode is not overwritten.
- `P8ENV` reports raw `gain_reg` (0x600A702C) and `agc_reg` (0x600A7030) plus
  `gain_reg_changes`. Neither shows the native gain decision (see the walk
  test). Evidence that the native loop is working comes from the Q4 radius
  following RF level while `blocked` and `fw_gain_epochs` stay flat. One
  `blocked` write at boot (settings restore) is expected.

The analyzer flags a native capture as **tainted** if `blocked` grows or any
firmware gain epoch occurs during it.

## Bench procedure

1. Flash the PR build. Use a controlled attenuator (or a fixed-geometry sweep)
   and the same VTX/channel for every run.
2. For each configuration below, run
   `python tools/p8env_sweep.py capture --port COMx --out <name>.log`,
   step the attenuation, and label each step with its dB value:
   - GOLDEN + Direct Gain V3 (baseline range; `N` for firmware gain)
   - Phase8 + Direct Gain V3 (the regression)
   - Phase8 with native AGC (default, zero firmware writes)
3. `python tools/p8env_sweep.py analyze <name>.log`.

Reading the result:

| Result | Meaning |
|---|---|
| `SUPPORTS_ORIGIN_COLLAPSE` (rho >= 0.6), large central/outer lift, Phase8 video loss at a step where GOLDEN survives | Pre-Q4 placement is the limiter (issue case A). Envelope control can recover range. |
| `DISPROVES_ORIGIN_COLLAPSE` (rho <= 0.2) | The hard tail is not driven by central cells. Look at multipath, AFC or demod mapping instead. |
| Native Q4 radius follows RF level with no taint | The vendor AGC free-runs (confirmed in the walk test). Next step is #117 Phase 4 tuning. |

## Deliberately not done here

- **Phase8 HOLD/RESEED guard in the BitScrambler.** The issue says not to pick
  a universal threshold before the envelope has been characterized on real
  hardware. `hard_central_pm` / `central_hard_share_pm` measure how often such
  a guard would fire. If the tail is dominated by central endpoints, the guard
  can reject exactly those pairs.
- **Annulus thresholds.** `P8ENV_PROVISIONAL` reuses the DG3 healthy window
  (P50 13..32, P95 <= 65, origin <= 250 pm, clip < 20 pm). Replace it with the
  `empirical annulus` from real sweeps.
- **FFT scale as a fine actuator.** Use the existing `F` probe with fixed RF and
  fixed gain. Only consider FFT scale if raw Q4 moves.
- **Dynamic bit-slice gears.** Out of scope per the issue decision.
