# U85-CVT investigation checkpoint

C5VRX by Twotoz and the C5VRX contributors; extends the U85Q generator
and counter pipeline already recorded in the project research ledger.

The 2026-10-12 search evaluates 20 analytical, immutable transition laws
on 36 identical-Q4 cases (720 results). Layouts use 28+4 angular/origin
tokens, 24+8 angular/low-radius tokens, or 32 angular tokens with a
phase-error-dependent law. Confidence here is a radius cue, not measured
RF C/N. No CPU per-sample processing or live LUT write is added.

No candidate passes every search gate. The 32-token `sync_bias` candidate
has one lock gate violation and mean -1.04 dB / worst -2.87 dB SINAD versus
the appropriate existing MATCHED baseline. Selecting the fewest violations
does not qualify a candidate for production. The generated CVT and CVT with
fixed LP programs are archived research (`u85q_cvt.bsasm`,
`u85q_cvt_lp.bsasm` in this directory; `u85_cvt.py check` keeps them
bit-exact and current). The firmware bench build (Kconfig option, console
V) was removed on 2026-10-12 after the negative validation; git history
keeps it.

The fixed post-detection LP still consumes both spare opcode slots:
LDCTIA loads the previous half output and ADDCTIAH adds the new half output
every span. Observation-dependent phase gain in a LUT is **not** a variable
post-detection IIR. An exact variable-alpha implementation fitting the
current two bundles has not been established. The previously rejected
periodic LP/bypass filter remains rejected.
See `CVT_HARDWARE.md` for the schedule and the limits of the direct encodings.

Held-out validation is now complete: 160 two-field captures and 3360
same-Q4 results. Plain 24+8 CVT averages -0.89 dB versus MATCHED; plain
32-token CVT averages -1.09 dB. Their fixed-LP alternatives average +0.81
and +0.60 dB on the weak/stress subset, but cost -2.07/-2.54 dB on the
strong subset. Against the eligible fixed-kp/LP oracle they average
-1.11/-1.35 dB. These do not satisfy the proposed universal acceptance.
See `data/cvt_validation_summary.json` and `data/cvt_validation.svg`.

Five further 64-state/16-token variants test the alternative allocation of
the same ten address bits (180 search results). This plain two-bundle
tracker differs from the older four-bundle Phase6 detector. All five have
strong/lock gate violations, with mean -0.94 to -1.66 dB versus MATCHED.
No current fixed-LP implementation is claimed for that six-bit state layout:
the existing controller packs five phase bits and five x/2 bits into the
ten bits left beside y. See `cvt_precision.py` and `data/cvt_precision.json`.

Status:

- [done] Bit-exact ESP-IDF assembler/emulator check of 20 expanded candidates,
  plus original U85Q/LP whole-stream and both-direction switch checks.
- [done] Held-out two-field study: seeds 211..214, dense -2..6 dB and 10/20 dB,
  PAL/NTSC, four deviations, CFO +/-1 MHz, DC, and 1/5/20 ms fades; all eight
  fixed kp values with/without LP and floating same-Q4 reference.
- [done] Bit-exact check of the five alternative 64-state layouts.
- [removed] Firmware bench integration (Kconfig, V, CI build), 2026-10-12.
- [concluded, negative] Further analytical laws: the held-out data do not
  support another law. The radius-aware 24+8 layout already is the
  uncertainty-aware transition; its weak-signal gain comes from the fixed
  LP, which U85Q-LP already provides. New laws need new evidence (real
  pre/on/post-cliff Q4), not a wider search on the same model.
- [concluded, documented] Variable post-LP: no exact two-bundle schedule
  (`CVT_HARDWARE.md`); CVT + fixed LP stays an experimental comparator.
- MATCHED-profile ladder/LP hardware tests; retain the profile RF filter.
- Sampling phase (`main/sphase_logic.h`, `test/test_sphase.c`): the
  absolute gate alone cannot tell intrinsic broadband noise from a
  mid-transition read. In the host model, 0.6-cell white noise already lifts
  the cleanest position to an excess of ~725 (limit 300), which is the
  board's repeated `unsettled` pattern. Above the limit only a scan whose
  nine positions differ (worst-best > twice the margin) is evidence; then the
  read must be within the margin of the best position. Positions that read
  alike are `noise_limited`: no slip, not a phase fault for FIELD_DIAG. The
  stored DC row is held while the gain worker is paused for a scan, and a
  final qualification (with repair) runs after the status screen has handed
  TX back to live video. `T` and the SPHASE lines log the absolute excess,
  HF4/HF2 whiteness (per mille of white), DC centre and the scan contrast.
  A normalized gate (whiteness; model clean ~364, mid-transition ~938 per
  mille) is not used: phasefix6 tested one and failed repeat-scan
  acceptance on the board, and its Kconfig experiment was removed.
  Board acceptance is pending: repeated VTX-off boots and F scans must show
  `settled` or `noise_limited` with the logged contrast, never a slip
  without contrast.
- Final production build (CI and local ESP-IDF 6.0.2 build pass), app-only flash and reopened console on the board.
- Physical moving-video, colour, 75-ohm voltage, HDZero and conducted
  attenuation acceptance remain unavailable while the operator's VTX is OFF.

See `../docs/PR196_TASK_PLAN.md` for the full agreed work and remaining
acceptance criteria. Automatic OUTPUT remains passive research.
