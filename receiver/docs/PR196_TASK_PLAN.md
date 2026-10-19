# PR #196 active work and acceptance list

C5VRX by Twotoz and the C5VRX contributors. Agreed operator goal,
2026-10-12. VTX remains OFF until the operator explicitly says otherwise.
Never record a VTX-on, moving-video, HDZero, scope or range test as passed
without that equipment/evidence.

- [x] Checked pipeline teardown, full program/LUT verification, one checked
  rollback, terminal fault latch; host stage/rollback/cleanup fault injection.
- [x] Shared 5 s cooldown and 2 s stable-evidence AUTO confirmation;
  manual OFF persistence; fresh settings AUTO kp+LP, saved OFF respected.
- [x] Manual OUTPUT g/menu, DAC-only mapping, safe 1x boot, clipping counters.
- [x] Eight kp levels in both directions/wrap, P spam, OUTPUT preservation
  across kp/LP/menu and manual-OFF reboot on the physical board.
- [x] #199 matching synthetic DC/noise through production quiet/filter helpers.
- [x] #199 real VTX-off 100-setting sweep, filter NVS and subsequent DCO.
  The test caught and repaired RX being stopped during menu calibration.
- [x] OUTPUT ideal-reference simulation: PAL/NTSC, narrow/normal/wide and
  issue #200 source-level stress; clipping/sync/swing and uncertainty guards.
- [x] Multi-Rescue same-Q4 simulation: 1088 results, 8 kp x U85Q/LP plus a
  floating Q4 reference; matched filters/baselines, repeated seeds, CFO/DC/fades,
  worst-case lock/relative colour/headroom gates and explicit NONE outcomes.
- [x] Passive non-oracle OUTPUT probe using production AFC/Direct Gain
  helpers, synthetic strong/weak/noise cases and a real VTX-off snapshot.
- [ ] Sampling phase: diagnose `unsettled`, distinguish intrinsic noise
  spectrum from mixed-transition reads, repair only on valid evidence,
  host tests and repeated VTX-off boots/scans; VTX-on qualification later.
  Host part done: scan-contrast verdict with `noise_limited`, DC held during
  scans, final live-context qualification, whiteness/DC/contrast logging
  (`main/sphase_logic.h`, `test/test_sphase.c`). Board repeats pending.
- [x] U85-CVT (comment 6124064221), negative result, research archive only: investigate a static nonlinear
  confidence-aware 1024-entry LUT in one feasible two-bundle program.
  Compare 28+4 and 24+8 token layouts and a small analytical transition-law
  search on identical Q4 against U85Q/MATCHED/LP and the fixed-kp oracle.
  Test assembler/emulator, state/wrap recovery, DAC bounds, CFO/filter/DC/gain,
  noise-only and 1-20 ms fades, PAL/NTSC sync/chroma; plot -2..20 dB with dense
  threshold points and repeated-seed/worst-case gates. Keep experimental;
  report negative results rather than silently activate an inferior tracker.
- [ ] Final build/checks, checked app-only flash, capture evidence, commit/push
  to PR #196 and reopen CMD monitor with the operator's AUTO setting.
- [x] Unified CVT follow-up (comment 6124244391), concluded: observation-dependent phase
  adaptation first; separate cycle/instruction feasibility for true variable
  post-LP. Keep fixed-LP alternatives experimental and reject periodic bypass.
  Current negative candidate results and remaining work: `../research/CVT_STATUS.md`.
- [ ] #200 physical moving-video acceptance: narrow/wide VTX, U85Q/LP,
  PAL/NTSC, LCD and HDZero with actual 75-ohm loaded sync/white/burst/swing.
- [ ] #201 / Multi-Rescue / CVT physical pre/on/post-cliff IQ, conducted
  0.25-0.5 dB attenuation and outdoor antenna-height A/B/A against best prior
  profile. A 204.75-us snapshot is not seconds of recognisable moving video.

Automatic OUTPUT remains passive research until its white/burst headroom
estimator and physical load/goggle acceptance are sufficient. Invalid or weak
observations must hold the last known-good setting. The current prototype's
conservative inferred white reserve and model C/N proxy are not calibrated
guarantees, particularly for wide VTXs. Hardware fault injection and real
picture continuity remain separate from the completed host coordinator tests.
