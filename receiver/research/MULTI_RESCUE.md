# Multi-Rescue proposal disposition

C5VRX by Twotoz and the C5VRX contributors. Proposal:
[PR #196 comment 6123654319](https://github.com/Twotoz/C5VRX/pull/196#issuecomment-6123654319),
with [cliff investigation #201](https://github.com/Twotoz/C5VRX/issues/201).

The useful objective is recognisable moving video with safe sync/colour and
output levels, rather than minimum luma noise. A minimal bank of existing
U85Q kp regimes and optional LP is sufficient for research; no evidence yet
justifies four new hardware programs or live exploration of unseen candidates.

Two hardware premises in the proposal are obsolete. The tested host access
follows LUT16 mode (1024 addressed 16-bit entries, low-half readback after
reselect), not a fixed 512-word CPU mapping. RUN/PAUSE LUT access failed on
the board, so **kp changes also use a pipeline restart**, with the same five
second backend guard as LP/OUTPUT. Phase-token compatibility of tables is
an arithmetic property, not proof that a live RAM rewrite is safe.

Update (2026-10-12): the MATCHED restriction is reconciled. The kp0.5/0.6
programs have the U85Q instructions with ladder LUT levels 7/5, so a
MATCHED profile now keeps its narrow filter and starts the generic U85Q/LP
geometry at that level; AUTO then runs with narrow-filter curves
(research README section 17). Tested against the best fixed profile in
simulation only; the transition/output state still needs the bench gate.

Current evidence:

- Host recovery coordinator tests cover every candidate/rollback failure
  stage, cleanup and publication. Previous board tests cover P bursts and
  three kp levels; current follow-up expands manual stepping in both directions.
  SDK failure injection, goggles and scope acceptance remain distinct gates.
- OUTPUT feasibility compares identical synthetic Q4 samples across manual
  levels and an ideal-reference gain/pedestal. Both U85Q and LP are evaluated.
  It measures nominal loaded sync/swing separately from amplitude-normalised
  goggle metrics. Source-level oracle inputs are explicitly not a live servo.
- #199 regression runs matching synthetic DC/noise through the exact quiet
  and filter measurement helpers. No physical reporter snapshot is attached.
- AUTO kp+LP defaults on for fresh settings by explicit operator decision;
  an existing saved OFF and manual P/k/j OFF survive boot. This default is
  an operator setting, not a declaration of flight qualification.

Next evidence required for the proposal's phases A-E: real IQ above/below the
cliff and conducted 0.25-0.5 dB sweeps, same-IQ ranking of all eight kp levels
and LP against the matched baseline and floating Q4 reference, hard gates on
sync/colour/loaded output headroom, repeated seeds/worst cases, then physical
A/B/A of recognisable moving picture. The 8190-byte snapshot at 40 MS/s
contains only 204.75 microseconds, roughly three scan lines; it cannot prove
seconds of moving-video retention. Model-derived C/N and AFC sync do not
measure unexecuted candidates or displayed picture.

The initial same-Q4 proposal study has now run: `rescue_study.py` records
1088 results across the matched-filter baseline and all eight kp values,
U85Q/LP and a floating same-Q4 reference. `data/rescue_study.json` includes
worst-seed hard-gate decisions, including NONE, and standalone PNG/SVG plots.
This is synthetic repeated-seed evidence, not physical attenuation margin.

Keep automatic OUTPUT as research until a reliable non-oracle estimator and
physical load/goggle acceptance exist. RF spatial nulls that destroy the Q4
information require RF/antenna improvements rather than another noise filter.
