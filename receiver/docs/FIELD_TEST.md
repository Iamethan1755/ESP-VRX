# C5VRX-5 field and range test protocol

PR #196 review follow-up (2026-10-12), still requiring external hardware:

- At fixed strong video, step all eight kp levels through repeated wraps,
  then repeat under controlled weak signal. Save T before/after every change;
  verification/restart and TX/RX/DMA/BS counters must not increase. Verify
  moving picture, colour and sync visually; no console-only range claim.
- Repeat P spam and simultaneous K/k/P/g requests. Accepted live changes
  must be at least 5 s apart. AUTO requires 2 s uninterrupted usable evidence;
  invalid windows and context changes discard pending candidates.
- P/k must show `kp_auto=0` after reboot; fresh NVS defaults to 2. Do not
  erase the board's calibration just to test the fresh-settings default.
- OUTPUT g/menu: verify 1x and 2x survive kp/LP changes and menu return,
  while every reboot starts 1x. On failed changes verify explicit rollback
  status and restored program/kp/OUTPUT; failed rollback must latch OFF while
  USB download remains usable. Host fault injection is not hardware coverage.
- #199: VTX OFF, cold boot/manual F, complete the actual 100-setting sweep,
  successful filter NVS and then DCO; retain FILTER_CAL/DCO/GAIN/IQ evidence.
  Synthetic matching-DC tests exist; reporter snapshot/board replay is pending.
- #200: same narrow and wide VTX/channel/camera/cable/power, LCD vs HDZero,
  U85Q vs LP, PAL/NTSC, properly terminated 75-ohm scope/capture. Measure sync
  tip, porch, black, white, burst and loaded swing at each OUTPUT level.
  Automatic OUTPUT has only ideal-reference simulation evidence and remains
  disabled; neither its fitted goggle score nor nominal DAC model qualifies
  real AV-input compatibility.
- Measure the recognisable-picture cliff in 0.5-1 dB attenuation increments
  against fixed U85Q and the already matched profile, followed by field A/B/A.
  Retain negative results; quieter static does not demonstrate extra range.

C5VRX-5 by Twotoz and the C5VRX contributors. This is the test protocol for PR #196
(operator report, 2026-10-11): there is much less static than before, but
usable range has barely increased. Video was good indoors through walls but
poor in an open field. Fewer visible artifacts are not the same as a lower
usable-video C/N threshold. This protocol separates five things:
propagation, antenna, receiver sensitivity, chain configuration, and a real
demodulator cliff. The diagnostics only observe. AUTO kp+LP (the default
for fresh settings) may briefly restart video on C/N changes; set `K` OFF
for an A/B that must exclude it.

## What the firmware reports

- **`D`: RANGE_DIAG health report.** It shows:
  - channel, plus a warning above 5885 MHz (R8, E6-E8 are outside
    Espressif's documented C5 range);
  - the requested and read-back filter, the filter class and the
    calibration result;
  - the active profile with its reason, plus the pending and provisional
    deviation;
  - AFC state;
  - the gain tuple (index, RF stage/code, BB, fine, top-stage start,
    at-max);
  - sampling phase, and whether DC is held for the current gain;
  - IQ statistics and transport faults;
  - verdict lines and the dropouts of this boot.
- **`R`: one `FIELD_DIAG` line per second.** `sync_valid_pct` means
  burst-confirmed AFC sync / **all** control observations.
  `measurement_coverage_pct` means measurable / all observations;
  `sync_given_measured_pct` is the conditional success rate. Ten measurable
  successes in twenty observations reports 50/50/100, not 100% video.
  Failed snapshot copies count as missing evidence. None measures displayed
  picture, moving luma, chroma quality or SINAD.
- **`FIELD_TRACE`: automatic bounded onset capture.** The first observation
  losing sync evidence after a confirmed one freezes up to 16 observations
  before, the onset, and 16 afterwards. Actual `t_ms` timestamps and a
  zero-based `trigger` index are printed. This reuses the **existing nominal
  50-ms AFC cadence**; it can capture one-window losses that 1-Hz summaries
  hide, but cannot resolve every 5–20-ms fade or detect a picture cliff
  that retains AFC sync. Never infer continuous IQ coverage from this log.
  The recorder uses fixed memory, no extra IQ copies/FFT/PHY writes, and
  console-only output. Slow USB preserves the oldest unread capture and
  increments `skipped` for new onsets; no producer waits for the console.
  Menu, calibration, channel changes and >200-ms observation gaps cut the
  history; a partial event is then printed without joining different contexts.
  Each point includes frequency/profile, requested filter/BW, filter readback
  status, RF/BB/fine tuple, gain writes/fade holds/overloads, p50/p95,
  origin/rail, coherence, model C/N validity, CFO/requested offset, AFC,
  sampling/DC state and transport-fault deltas. These are asynchronous
  observer summaries, not an atomic or calibrated raw-power measurement.
- **`FIELD_DIAG_EVENT`: 1-Hz evidence-loss categories**, not physical causes:
  `CALIBRATION_OR_PHY`, `FAST_EVIDENCE_LOSS`, `AT_RF_FLOOR`,
  `DEMOD_CLIFF`, `UNKNOWN`. Missing coverage or unknown model C/N stays
  UNKNOWN. Coherent IQ with missing sync takes precedence over gain headroom.
  FAST_EVIDENCE_LOSS can include fading, shadowing, VTX changes or other
  failures; it does not diagnose ground reflection. DEMOD_CLIFF is a replay
  candidate, not proof that U85Q caused the loss.

Capture goggles or terminated CVBS separately: firmware does not know whether
recognisable moving image information remains. Read-back verifies code/skirt,
**not independently BW20/BW40 analog response**. C/N is a simulation-derived
envelope model, not dBm, measured noise figure or independently validated RF C/N.

## Open-field physics, before blaming the receiver

Two-ray/Fresnel fading, antenna clearance and polarisation are possible
explanations, not established causes for this board. Models in
`research/README.md` and `field_antenna.py` use assumed ground, antennas
and receiver threshold. Their predicted distances and inferred noise figure
are **not measured C5 WBFM sensitivity**; Wi-Fi PER sensitivity cannot establish
that threshold. Identify the actual supplied FPC antenna before calling it
2.4-GHz-only. Test a known-good 5.8-GHz antenna with matching polarisation,
clear of the body, then repeat antenna-height/location A/B/A. A repeatable
change suggests propagation or antenna effects, which a conducted test separates.
A quantiser verdict is likewise a hypothesis requiring captured-IQ validation.

## Record every run

Write these down; the firmware cannot infer them: distance, RX and TX
antenna type and polarisation (linear/RHCP/LHCP), antenna heights, VTX
power and channel, whether line of sight is really clear, and the firmware
build. Capture the serial log with `R` on, then compare runs:

```
python3 tools/field_diag_compare.py run_a.log run_b.log run_a2.log
```

Repeat each comparison A/B/A (or ABBA) so that drift and fading average out.

## Steps

1. **Chain sanity before anything else.** Press `D` with the VTX on and
   check:
   - filter `applied=1` with class normal, narrow or wide;
   - `sphase=settled` or `noise_limited`, `dc_ok=1`, faults not increasing.

   If any of these fails, fix it first, by rebooting with the VTX off or
   using CALIBRATE on the ground. Comparing RF setups is meaningless
   otherwise.
2. **Indoor vs outdoor at the same distance.** Use the same VTX, power,
   channel and antennas.
3. **Antenna height (ground reflection / Fresnel).** Outdoors, keep the VTX
   still and run: receiver at normal height, then raised 1-2 m, then normal
   again. Also move the receiver sideways 0.25-1 m.
   - A large, repeatable change with height or position points to a
     spatial null.
   - The fix is placement and clearance, and later spatial diversity with a
     second receive path. A different LUT cannot remove a null.
4. **Antenna A/B.** Compare the stock XIAO antenna with a known-good 5.8 GHz
   antenna of matched polarisation, in the same place. Linear vs circular
   polarisation alone costs about 3 dB, and RHCP vs LHCP costs much more.
5. **Channel A/B.** Compare a channel at or below 5885 MHz (e.g. 5.6-5.8
   GHz) with an upper channel above it.
6. **Conducted attenuation (the only dB result).** Connect the VTX through
   enough fixed attenuation and a stepped attenuator into the receiver
   using 50-ohm connections. Never connect a VTX directly.
   - For each firmware (C5VRX-3 main, C5VRX-5 default, PR #196 matched) and
     a reference receiver, find the attenuation where the picture stops
     being usable.
   - Sweep the last 8–12 dB in 0.5–1 dB steps up and down, with repeated,
     randomised runs and the same moving test pattern. Record actual
     attenuation, raw IQ (`I`), `D/R/T`, and filmed goggles or properly
     75-ohm-terminated CVBS. Use the same recognisable-moving-picture
     criterion, measuring luma if available; sync evidence is separate.
   - Track carrier/IQ, recovered video-information, and output/goggle-loss
     thresholds independently. Static remaining visible is not NO SIGNAL.
   - Repeat low- and wide-deviation VTXs, DEFAULT/MATCHED/WIDE profiles
     and carrier offsets ±0.5/±1 MHz during explicit bench setup only.
     Replay the same pre/post-cliff IQ with bit-exact U85Q and a floating
     FM reference before choosing another real-time DSP implementation.
   - Randomise the order and repeat each setting.
7. **IQ lane bench test (optional, on the ground).** `L` compares raw IQ on
   the ultrafine, fine and coarse lanes at the same gain, filter and
   channel (ABBA order, video stops ~3 s).
   - These are raw statistics only: U85Q is made for ultrafine.
   - Production stays ultrafine unless another lane reproducibly wins on
     the conducted attenuation threshold without losing picture quality.

## Reading the result

- **Height or position changes the picture** → possible spatial propagation or antenna effect. Fix placement; long term, use antenna
  diversity.
- **Same loss in the conducted test, independent of height** → receiver
  sensitivity or configuration. Look at the `D` gain tuple at the edge,
  and compare raw IQ (`I` snapshots) with a reference receiver.
- **Dropouts are mostly `DEMOD_CLIFF`** (coherent IQ, no sync) → a
  demodulator, AFC or sync problem that IQ captures can show. Only this
  case justifies more DSP work.
- **Dropouts are mostly `AT_RF_FLOOR`** → possible receiver-floor evidence; verify with controlled RF/IQ. Improve the RF link (antenna, height, polarisation, VTX power),
  not the LUT.
- **The other antenna fixes it** → antenna mismatch, polarisation, or a
  connector problem.

Claim a range improvement only from a reproducible conducted attenuation
margin, or from a lower dropout rate under comparable logged conditions.

## Merge evidence still required

Host tests and compilation verify diagnostic semantics and recorder bounds;
they do not prove extra attenuation tolerance or cure the reported picture
cliff. Flash and check VTX-off/on startup, fresh/valid calibration, correct
active profile and zero later disruptive calibration. Measure observer timing,
stack headroom and transport faults with slow/disconnected USB. Complete a
repeatable conducted attenuation A/B plus matched outdoor A/B.

Bench-verify RF offset in both directions and actual BW/passband/ENBW, wide
VTX sync-tip/white margins, CVBS levels/loading, and per-gain DC validity after
filter/profile changes (`dco_same_context` currently ignores filter/BW fields).
No intrusive DC search, sampling slip or lane switch is permitted as an
automatic in-flight correction (the bounded startup phase may slip RX once
more after the status screen, before flight). The only in-flight tracker
replacement is the optional live profile switch (`A`, default on): set it
OFF for an A/B that must keep one profile. There are no physical measurements
attached to this diagnostic fix; the underlying RF/FM/DSP/output cause remains
unresolved until those captures exist.
