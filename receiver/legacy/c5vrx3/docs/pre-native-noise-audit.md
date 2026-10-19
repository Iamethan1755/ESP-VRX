# Noise history before native AGC (2026-09-30)

Question: did a change merged into main introduce noise before native AGC?
This is a source/history review, not a new hardware comparison.

## Confirmed changes

- Main merge `83780a0` (PR #110, 2026-09-28) enabled both
  `C5VRX_PHASE8_HR_LIVE_TEST` and `C5VRX_DIRECT_GAIN_V3_EXPERIMENT` by default.
  Before the merge, the 6BIT path selected Phase5-360 when enabled. After it,
  it selected the new full-range adjacent Phase8 program. Thus this merge
  changed both the demodulator and firmware gain controller before native AGC.
- Phase8 uses a 256-bin endpoint phase estimate and maps
  `(128 + current_phase - previous_phase) mod 256` to six DAC bits.
  It does not use the old pair LUT's DAC mapping. This is a changed transfer
  function, not evidence that more phase bins inherently cause noise.
  Commit `fe40f45` changed the earlier Phase8 experimental slope from
  `76 + 2*delta` to `128 + delta`, removing early arithmetic wrap.
- The pre-native V3 implementation has a specific weak-signal recovery
  candidate: `direct_gain_v3_observe()` classifies P50 <= 4, origin >= 650 pm
  and coherence < 20 as no carrier, then requests `survival_gain` immediately.
  `start_write()` returns without writing when that gain is already selected.
  Repeated observations satisfying this condition cannot progress to the
  later gain-up logic. This matches the recorded G62, zero-write, 30-second
  collapse in `phase8-range-envelope.md`. It does **not** establish a hard
  G62 ceiling: V3 accepts the gain table's maximum index.
- `phase8-range-envelope.md` already labels Phase8 + V3 as the regression
  relative to its Golden + V3 comparison protocol. That protocol remains
  open; the label alone is not a completed controlled A/B result.

## What native introduction changed

`f46c4ba` introduced native AGC as opt-in; `04a405e` made it the default.
The native path stops forcing firmware gain and leaves the PHY AGC enabled.
The latter default-switch commit does not change the live demodulator LUT.
The main comparison `6d36a5d..fdc32a1` (PR #120) has no changes in
`fm.bsasm`, `fm_phase8_hr_live.bsasm`, `gen_phase8_hr_live.py` or
`sdkconfig.defaults`.

## Scope of the conclusion

PR #110 is a concrete pre-native regression boundary to investigate. The V3
no-carrier branch can explain failure to recover a weak signal while V3 owns
gain. It cannot explain ongoing gain cycling when native AGC owns gain.
Low-amplitude Q4 phase quantization remains a separate mechanism capable of
turning small input changes into large phase steps. Central occupancy alone
is not proof of failure: the earlier native walk test also had a perfect
picture with high central occupancy.

The operator has already reported that Phase5 did not suppress the current
noise. Do not present a Phase5 rollback or the V3 recovery fix as a proven
cure for native grain. Isolating a historical regression requires a matched
comparison with the same gain owner, RF conditions and output mode, changing
only the demodulator/transfer under investigation. No firmware or board
settings were changed for this audit.

## Operator follow-up: native cadence request (2026-09-30)

The operator reports rainbow grain concentrated at particular picture colours,
also with Phase5. This makes a Phase8-only regression insufficient to explain
the observation. The shared RF/IQ and output paths remain candidates. Native
gain transitions may disturb phase/DC/settling; ideal common I/Q amplitude
scaling alone would not change the FM phase. No causal conclusion has been
established from the visual report alone.

The 8020 +32 trial was verified on COM10 at 416 with native AGC, acquisition
profile 10 (7034=127), P8 FULL and coarse IQ. The operator reported no visible
difference. The later 352 trial was verified, but its snapshot had state 83,
P50=3 and strength=2; it cannot establish a matched carrier-present result.
The field restored to 384. A subsequent FINE selection was verified; no
unambiguous FULL/FINE operator comparison has been obtained. An earlier
unchanged-picture report immediately after flashing preceded activation of
the policy trial, so it is not evidence against the active trial.

The operator requires FULL Phase8 and native AGC, and rejects HOLD, sample
replacement and DSP masking. The new request is to determine the best native
update speed and build it, considering roughly 1 ms as a candidate.

### Cadence research result

- Re-disassembled the exact Docker IDF v6.0.2 C5 libphy archive, SHA-256
  `dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb`.
  AGC initializer, BB update, packet detector, sense and saturation setters
  expose register policies, not a decoded periodic AGC scheduler.
- No unit, direction or period has been established for 7034[30:24],
  7158[6:0] or 71B0[27:21]. Profile 10 remains the operator-selected 127,
  **not** a measured 0.1 us, 0.6 us or 1 ms native update interval.
- Watchdog timer configuration cannot be assumed to pace acquisition:
  7C40[31] was already clear on the carrier-present baseline while switching
  continued. The 8020 policy trial does not establish a cadence control.
- Reviewed both legacy trees and the current
  [ESP-SDR C5/C6/C61 receiver](https://github.com/ESPARGOS/esp-sdr/tree/main/main/families/c5_c6_c61)
  and [gain control](https://github.com/ESPARGOS/esp-sdr/blob/main/main/common/burst_gain.h).
  ESP-SDR uses native gain or explicit forced gain, not an exposed periodic
  native update setting. The
  [independent C5 register map](https://github.com/Jarvis-2-0-kit/esp32c5-csi-unlock/blob/main/docs/register_map.md)
  also does not decode a native AGC update-period field. These reviews do not
  prove that silicon has no such control.

One millisecond is a research candidate, not a determined optimum. It spans
about 16 video lines; disturbed gain writes at that interval could still be
visible. Acquisition completion time, restart spacing, and useful gain-change
spacing are different quantities. Existing status polls are proxies, not
synchronized per-sample gain events. A single 8192-word window at assumed
80 MS/s covers only 102.4 us and cannot independently verify a 1 ms period.

Do not emulate a cadence by periodically disabling/reenabling AGC, forcing
gain, or pacing live IQ. Do not ship an arbitrary register setting under a
millisecond label. A rate-changing native build remains unimplemented pending
identification and measurement of the hardware control. The existing normal
image can be rebuilt, but this is not completion of the requested cadence
change and must not be advertised as one.

Subsequent explicit user authorization changed the implementation request:
pause BB AGC between brief native tracking windows rather than locate a native
period register. See `native-agc-paced.md`. This is now built as a GPTimer gate
experiment; it does not establish a decoded silicon cadence control.
