# C5VRX-5 findings

C5VRX-5 keeps only what the C5VRX-4 research on PR #190 (branch
`codex/c5vrx4-pair-range`) showed to help the picture. The full study, its
scripts and data live in that branch under `v4/docs/PICTURE_CLIFF_STUDY.md`,
`v4/tools/dsp_search/` and `v4/docs/data/`. This page is the summary.
Synthetic results are host simulations; board results are labelled as such.
Measured range is not established.

## The problem: the whole picture dropped out

With slightly bad video the C5VRX-4 picture did not get noisy, it vanished.
A goggle model (sync low-pass, slicer, H-PLL flywheel, ideal and peak-clamp
AGC) over full fields showed why: the trained 32-phase trackers (PAIR,
RANGE32) slip on VTXs with a larger deviation (>= ~1.05) or an off-centre
carrier and never re-acquire. They lost the whole picture in 8-25 % of cases
even at 20-25 dB C/N. The board logs added two contributors: the DC search
running under weak video, and gain hunting at the range edge.

## Where information is lost, and where clicks come from

An information ledger added one loss at a time to identical signals:

- The 4-bit IQ lanes, folding included, lose essentially nothing against
  the analog IQ. Using one sample per 50 ns span is not a loss either.
- Clicks are FM threshold physics: the noise phasor circles the origin. The
  ideal analog discriminator (an RX5808) has them too. The IQ shows when
  they happen: the sample lands in the origin cells.
- The loss is the 32-sector phase lattice (625 kHz per output step). A
  tracker with clipping recovers it; the flashed LUT reaches its own float
  bound below 14 dB C/N.

## U85Q: the demodulator

An untrained search chose among closed-form formulas only (no fitted LUT
entry), with held-out confirmation:

- state: 32 tracked phases, `p' = p + 0.85 * wrap(observation - p)`;
- observation: the PAIR4411 shared word (sample A full byte + sample B
  signs);
- origin cells: four coarse quadrant tokens with their own gain 0.5. Erasing
  them (U85E) was sharper but failed under multipath, because fades put
  real signal into the origin cells;
- output: the tracked advance, linear over -6..+9 MHz onto the DAC.

Held-out results (picture = correctly displayed lines, mean):

| model | nominal picture / SINAD | mixed robustness picture / SINAD |
| --- | --- | --- |
| **U85Q** | 0.998 / 3.55 dB | **0.893** / 1.93 dB |
| U85E (origin erasure) | 0.995 / 4.26 dB | 0.706 / 2.48 dB |
| HC50 (stateless) | 0.981 / 2.17 dB | 0.872 / 0.76 dB |

The mixed set covers echo + fading, carrier +-2 MHz, DC, I/Q 1.4/20 deg and
a CW spur. Trained PAIR/RANGE32 scored 0.83 / 0.82 on the nominal set, with
0.00 in their worst cases. Under multipath U85Q matches its float upper
bound; what remains there is in the signal, not the LUT.

The output window is the one trade-off left: narrower is sharper but clips
the sync tip when the carrier is off centre.

## Board validation (2026-10-11, real video, A1)

U85Q with the narrow filter and the DC latch, 12 snapshots:

- the carrier's sync tip sat at -5.0 MHz and white at +6.25 MHz. U85Q's
  -6..+9 MHz window holds both; U85E's -3.5..+7.5 MHz clipped the sync tip,
  so a narrower window is not safe;
- clicks per 1000 samples: U85Q 83.0, U85E 85.7, HC50 101.5.

## AFC target matched to U85Q (C5VRX-5)

C5VRX-4 centred the VTX blanking level at -436 kHz, the design centre of
the trained trackers. U85Q's window is -6..+9 MHz, and the board's real VTX
swung wider than the deviations searched (up to 1.4). Shifting the AFC
porch target, held-out simulation (board conditions with +-1 MHz carrier
error, stress, 20 cases per cell at C/N 4-20):

| porch target | picture (all) | SINAD | deviation 1.4 | deviation 1.8 |
| --- | ---: | ---: | ---: | ---: |
| -436 kHz (C5VRX-4) | 0.908 | 2.42 dB | 1.00 | 0.64 |
| **-1036 kHz (C5VRX-5: -1.0 MHz)** | **0.969** | **3.27 dB** | 0.98 | 0.90 |
| -1436 kHz | 0.961 | 3.89 dB | 0.95 | 0.91 |

The larger shift buys more SINAD but starts to drop pictures; -1.0 MHz is
the balanced choice. Board confirmation is pending.

The Direct Gain V5 band also fits U85Q: same simulation, mean over C/N
4-20 and deviation 0.69-1.4, by IQ radius (cells):

| radius | 3.0 | 3.6 | 4.2 | 5.0 | 5.7 | 6.5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| picture | 0.992 | 0.997 | 0.987 | 0.995 | 0.993 | 0.980 |
| SINAD dB | 3.16 | 3.09 | 3.32 | 3.03 | 2.72 | 1.75 |

The healthy band (P50 13..32, radius ~3.6..5.7, targets 4.1 / 5.2) sits on
the flat part; folding only costs above it. The gain controller is kept.

## The narrow analog filter

FM threshold follows the noise bandwidth before the demodulator. The board
ran ~19.4 MHz (noise bandwidth 20.7 MHz) where Carson's rule for typical
VTX video is ~13-14 MHz. Host results, U85E:

| filter | lock at 2 dB | lock at 4 dB | mean SINAD |
| --- | ---: | ---: | ---: |
| ~20 MHz | 0.66 | 0.92 | 4.26 dB |
| 14 MHz | 0.95 | 0.99 | 4.55 dB |
| 12 MHz | 0.98 | 0.99 | 4.50 dB |

14 MHz costs <= 0.3 dB on a strong signal for every tested deviation, and
helps every weak case. C5VRX-5 measures the setting per board (VTX off,
once) and keeps the one with the lowest noise bandwidth that still covers
14 MHz. (Later lowered to a 12 MHz default class, plus 10 MHz narrow and
16 MHz wide classes; `calibration.c` FILTER_MIN_KHZ.)

## Receive-chain fixes carried over

- DC: the per-gain DC DAC pair is searched only in confirmed quiet, never
  on a channel where a carrier was seen this boot, and held after every gain
  write (board: DC median ~134 LSB without the hook, ~11 with it).
- Sampling phase: about 3 boots in 10 sampled MODEM_DIAG mid-transition;
  a fourth-difference probe finds it and slips the RX clock.
- Direct Gain V5: healthy-band zero-write zone, 20 ms persistence for
  moderate excursions, burst gate and staged overload recovery.

## What did not help (kept out)

Trained LUT demods, FusionDemod/AutoFit program swaps (3.8-5.1 ms engine
halts), the sync flywheel, the level servo, adaptive lanes, the radius
boost, the vendor RX recalibration (grain on strong pictures), one lookup per
IQ40 sample, frequency-only trackers, MMSE outputs with a narrow window.

## What is left

Within this demodulator class the C5 is close to its ceiling. The remaining
dB are outside the LUT: the RF front end (LNA, filter, antenna, diversity) or
hardware with line memory at sample rate. A flight comparison against an
RX5808 is the open measurement.

## PR #196 ladder and LP handover (2026-10-12)

C5VRX by Twotoz and the C5VRX contributors. Initial operator evidence:
video appeared stable during five requested kp steps, but telemetry reached
1759 verification failures in 1760 writes. The old software shadow still
acknowledged them, so reported levels did not prove hardware contents.
Paused instruction rewriting with P produced black video while USB lived.

The first candidate (65946f1) inferred a fixed 32-bit CPU LUT port from
ESP-IDF's LL comment. Physical tests disproved it. The ESP32-C5 TRM states
that host addressing/data size must follow the configured LUT width too.
Stopped-core probes showed correct LUT16 low-half reads and neighbour bits
in some upper read halves; mode 32 accepted packed writes. Keep LUT16 and
compare only the low 16 bits. RUN writes returned unrelated RAM values and
failed almost entirely; PAUSE-only updates likewise failed and caused TX
empty events. HALT with the normal load/reset/start topology verified every
entry. The exact internal arbitration/read-latch cause is not established.
Source: [Espressif ESP32-C5 TRM, BitScrambler memory access](https://documentation.espressif.com/esp32-c5_technical_reference_manual_en.pdf).

The corrected path loads the complete target while halted, reselects each
CPU address before readback, and only publishes the shadow after all 1024
LUT16 entries verify. Failure restores baseline video. Both kp and LP
changes briefly restart output; no sample-seamless handover is claimed.
`test_kp_lut_io.c` models LUT16, address refresh, unrelated upper read bits,
a failed target and rollback, and every real kp table in both directions.
`kp_writes` counts attempted LUT16 writes.

Operator `lutfix2` telemetry reached 32,768 writes with zero verification
errors, one kp change and 31 program switches, zero transport/restart faults.
P switches measured about 5.5 ms; kp about 6.3 ms. The operator could barely
see the change. These timings belong to that build/setup, not a universal
latency or flight/range result. Continued P spam eventually hung in IDF
`parlio_tx_do_transaction` at its unbounded FIFO-ready spin (MEPC 408074e0,
control task, repeated IDLE watchdog warnings, USB heartbeat still alive).

Live TX now uses an owned nine-descriptor cycle covering the same 32-KiB
raw ring, instead of re-submitting driver loop transactions. Descriptor
ownership/zero EOF are restored with TX stopped; the FIFO-ready wait has a 20-ms timeout and
vTaskDelay so IDLE can run. A timeout returns failure and attempts baseline
recovery. The original boot/menu/RX topology and DAC pins are retained.

Every actual live change shares a five-second cooldown; failed restarts
also consume it. AUTO requires an unchanged combined kp/program target for
two seconds of usable observations in the same context. A combined target
uses one restart, not separate kp and program interruptions. P during
cooldown is refused and requires a new press later. Policy/mode reset
cannot erase the physical cooldown. `test_kp_switch_guard.c` checks boundary
times, target oscillation, coverage gaps and context changes. Diagnostics
expose switch_wait_ms, kp/prog restart times and failure counters.

Guarded owned-ring build hardware test (COM10, XIAO ESP32-C5 rev 1.0,
IDF 6.0.2, 2026-10-12): six bursts of 32 P requests, spaced 5.5 s apart,
produced six accepted program changes and 90 explicit cooldown refusals;
excess commands can be dropped by the bounded queues. Three manual kp
steps then reached verified levels 1, 2 and 3. Final totals: 9216 LUT16
writes, zero kp/prog verification failures, zero restart failures, zero
TX-empty/RX-overflow/DMA/BitScrambler faults, and no watchdog/abort. Program
restarts measured 5.94-6.19 ms, kp 5.78 ms. The observer remained alive.
This proves that tested manual sequence, not all flight conditions or the
AUTO target curve; continuous-video quality and the two-second AUTO dwell
still need field observation. Firmware was app-only flashed with a matching
partition layout and NVS/PHY left intact.

Sampling drift was observed separately and is not claimed fixed here.

## Review follow-up: recovery, calibration and output level (2026-10-12)

PR comments 6123181977 and 6123278553 identify remaining safety and
compatibility gaps. The first comment reviewed obsolete 65946f1; its
32-bit LUT-port endorsement was contradicted by the physical tests above.
The corrected LUT16/HALT access is retained.

`transport_recovery.h` drives the production restart: checked teardown,
BS enable, program load, instruction readback, complete LUT16 write/readback,
BS reset/start, RX/TX enable, RX start and bounded TX start. The last good
LUT/configuration is published only after the whole chain succeeds. Each
failed candidate is cleaned up before exactly one fully verified rollback;
failed rollback or teardown latches live OFF and reports an error without
aborting/retrying indefinitely. USB runs independently. Menu return and boot
use this same verification path. `test_transport_recovery.c` injects every
candidate and rollback stage failure, initial-load failure and failed cleanup,
and verifies publication only after successful final DMA start. These are
host coordinator tests, not physical SDK failure-injection evidence.

Manual P/k save AUTO OFF once, and AUTO is stopped/reconciled after restart
errors. Missing/invalid settings select AUTO kp+LP by the operator's explicit
decision; existing saved OFF is respected. Invalid probe/menu/scan windows
clear target confirmation. Five-second backend cooldown and two-second
continuous usable-evidence confirmation remain in force.

Issue #199 has no attached raw snapshot. A matching synthetic +1.23/-3.26
cell DC and 1.7-cell complex noise test now runs the exact shared quiet and
filter-measurement helpers, with no threshold change and no prior DC search.
100 synthetic bandwidth settings complete without a definite-carrier abort
and leave eligible quiet candidates. A too-narrow/noisy setting can be
skipped, just as in the real sweep. Weak FM (-1..3 dB model C/N) is refused
as quiet; failed/short probes, recent sync, origin and rails retain separate
refusals. These 100 synthetic settings are not the actual PHY register sweep.
The reporter's complete physical sweep and subsequent DCO remain pending.

Issue #200 manual OUTPUT is integrated via g/menu. Boot is always 1x;
1.5/2/2.5x change only DAC6 low bits. Transformation is applied to original
kp/program tables, never cumulatively to an already-scaled table. Verified
level/program/output survive menu return and rollback; T reports clipping
as the count of transformed LUT entries, not live sample clipping. HDZero
acceptance with the reporter's low-deviation VTX and a wide VTX is pending.

`research/output_level.py` uses the existing bit-exact U85Q and LP models
over PAL/NTSC, deviations .69/1.0/1.7 and C/N 6/30 dB, plus an unequal
sync/picture swing approximating issue #200 (-2.82/-1.5/+3.7 MHz): 160 model rows,
including four manual output levels and an ideal sync/white-referenced
candidate. All eight kp LUTs additionally pass identity and metadata
preservation checks. The DAC model assumes ideal 3.3 V GPIO, nominal
resistors, 200-ohm series resistor and 75-ohm termination (maximum .623 V
loaded). This differs from the issue reporter's nominal voltage model:
neither is a measured board voltage, and actual output impedance/load must
be established before interpreting compliance or selecting a voltage target.

Example, PAL strong carrier, U85Q, deviation 1.7: manual 2x clips 23.65%
of pre-LP samples and luma SINAD falls from 15.63 to 11.55 dB. The ideal
sync/white-reference candidate selects 1.19x, clips .64% and scores 15.68 dB.
At deviation .69, the nominal loaded sync depth rises from .060 V (1x) to
.127 V with the ideal 2.5x candidate, with .66% clipped samples. The high
gain cap prevents this example from reaching a nominal .286-V sync target;
gain does not increase the DAC's physical voltage span. Noise and transient
excursions can still clip even with ideal reference headroom.

The existing goggle metric fits analogue amplitude away, so its normalised
picture score cannot prove AV-input compatibility. The simulation separately
reports unnormalised nominal depth/swing/clipping. It demonstrates potential
benefit and the harm of blind 2x, not a tested live estimator, HDZero model,
actual sensitivity gain or flight-safe automatic output. Automatic OUTPUT
is consequently not enabled. A future implementation needs reliable sync,
porch **and white/burst headroom** measurements, context resets, failed-write
rollback, at least 2 s stable evidence and the shared 5 s application gap;
it must hold on invalid/weak observations rather than follow a fade.

Follow-up board evidence: firmware `5.0.0-pr.196-output2` passed all eight
kp levels in both directions including wraps, two P-spam bursts, 1x/2x menu
return, 2x preservation across kp/LP and an OFF-persistence reboot. Before
reboot: 16 kp and 3 program changes, 25,600 total verified LUT writes,
zero verification/restart/transport faults and no watchdog/abort. Kp changes
were about 4.2-4.4 ms and program changes about 3.8 ms in this VTX-off run.
After testing, AUTO kp+LP was explicitly restored and OUTPUT was 1x.

Manual F first caught a follow-up regression: full recovery teardown was
also stopping RX during the menu/status raster, so all calibrator copies
failed. The correction keeps RX and its descriptor metadata alive while
normal menu/status screens own TX; failed pipeline recovery still stops all
enabled parts. The production stop-parts helper now has a host regression
assertion for retained RX plus complete cleanup even when a stop fails.

Repeating F on the board (operator confirmed VTX OFF) then measured all
100 settings, plus calibrated baseline/recheck (102 setting log records),
and stored normal BW40 code48/skirt8, width 13.125 MHz, NBW13.280 MHz;
narrow code60/skirt0, width11.875 MHz, NBW11.899 MHz; wide code24/skirt0,
width18.125 MHz, NBW18.245 MHz, calibrated NBW20.612 MHz. It subsequently
searched 12 DC rows and measured a floor at gain72; no missing rows remained.
The bounded startup finished in 119.205 s without no_quiet_channel,
sample_failure or new transport/verification faults. Sampling phase still
reported unsettled and is an explicitly added investigation, not a passed
test. This is this board's VTX-off acceptance, not the #199 reporter's capture.

Multi-Rescue proposal test (`research/rescue_study.py`, data/rescue_study.json
and standalone plots): 64 identical-Q4 cases x 17 candidates = 1088 results,
with eight kp values for U85Q/LP and a floating adjacent-FM same-Q4 reference.
Clean cases have four seeds (two PAL/two NTSC), deviations .69/1.0/1.4/1.7,
C/N1/4/30 dB and the existing matched filter/baseline. CFO +/-750 kHz, DC1
cell and 3-ms/-30-dB fades use two seeds. Ranking uses worst-seed absolute
model line-lock >=.90, no more than .02 lock loss vs baseline, relative burst
SINAD >= baseline-1 dB and no additional clean DAC-rail exposure, plus <=1 dB
strong-signal loss. This relative burst gate does not prove decoded colour;
negative burst SINAD may still pass if the baseline is equally impaired.

Examples: .69/CN1 LP kp.50 gives +1.98 dB worst luma SINAD vs MATCHED,
worst modeled lock .960; 1.0/CN4 LP kp.70 gives +1.17 dB, lock .995;
1.4/CN4 LP kp.85 gives +1.79 dB, lock .989. No candidate meets the gates
at 1.4/1.7 CN1, the modeled 3-ms fades, or wide/DC1 at CN4. These are
conditional noise/detail improvements, not attenuation-margin measurements
or recognisable moving-video proof. Four new hardware programs are not
justified by this result. The matched-topology restriction is reconciled
since (research README section 17); no universal automatic rescue claim.

The first non-oracle OUTPUT probe (`research/output_probe.c`) runs the
production AFC/Direct Gain helpers passively. It rejects all modeled CN-10,
CN1 and two windows of a real board VTX-off snapshot. Strong narrow PAL
accepts191/196 windows; median sync bias .86 kHz, p95-p5 spread15 kHz,
candidate gain1.61 with spread.039. The conservative five-sync-span white
reserve refuses normal/wide candidates; the global C/N proxy also
underestimates wide strong-carrier confidence. This prototype therefore
does not qualify a universal automatic OUTPUT servo. White percentiles
from a few lines cannot guarantee headroom for a subsequent bright scene.

## The ladder froze at the range edge (2026-10-13)

C5VRX by Twotoz and the C5VRX contributors. The kp + LP ladder only helps if
the control loop lets it reach the edge, and two parts of `control.c`
stopped it there (research README section 18, `research/ladder_gate.py`):

- The ladder and its switch confirmation shared the AFC window gate, which
  includes the envelope-stationarity test. Receiver noise near the FM
  threshold fails it: on the 12 MHz class 46 % of control windows pass at
  0 dB C/N and 85 % at 3 dB (sim.py IQ). Each failure held the ladder and
  restarted the confirmation. `kp_ladder_window_usable()` now gates the
  ladder on settle, gain write and context only; the AFC keeps the test.
- The confirmation wanted the exact (kp level, program) for 2 s while the
  ladder steps a level every 0.2-1 s. `kp_switch_confirm_toward()` confirms
  the program and the direction instead and applies the latest level;
  oscillation around the running pair still never switches.

Control-loop model with the real headers and the best_combo picture model
(16 seeds): arriving at the edge and staying there, the C/N where half the
windows still show a picture improves by 1.0-2.1 dB in 10 of 15 cases
(deviation 0.69 / 1.0, 12 MHz and MATCHED, with and without fades) and lands
within 0.2 dB of an instant oracle; deviation 1.4 under fades and MATCHED 1.0
under fades gain 0..0.3 dB. A slow steady fly-out changes -0.2..+0.2 dB, which
is why a walk-away test can hide it. Simulation only; board check: `kp_steps`
and `lp_switches` in `T` should follow the C/N at the edge.

## Sampling phase on the carrier, WIDE rows (2026-10-13)

C5VRX by Twotoz and the C5VRX contributors. Host model (research README
section 19): a mid-transition MODEM_DIAG read costs 0.3-0.6 dB of range
when one LSB lane mixes and 1.4-5 dB when several bits do. The boot check
ran on receiver noise when the VTX was off and only reported in-flight
drift. The firmware now chooses the cleanest of the three positions on the
carrier once per boot, probes it every 10 s against half the old limit and
repairs a confirmed excursion with a short scan (no per-boot cap; false
alarms back off to 16 min). The WIDE profile gets its own 16 MHz kp/LP rows
(sharper picture; no range gain exists for wide VTXs in this demodulator),
MATCHED uses the 10 MHz rows only for a filter measured <= 11 MHz, and
FIELD_DIAG logs kp and LP per second. Host tests and a local ESP-IDF 6.0.2
build pass; board check pending.

## Board walk: the ladder never switched (2026-10-13)

C5VRX by Twotoz and the C5VRX contributors. Board check of this PR
(`5.0.0-pr.211`, kp_auto=2, A1 5865 MHz, DEFAULT 12 MHz profile), VTX
walked through the house to spots where the picture was certainly gone.
Data: `research/data/walk_2026-10-13/` (`walk_serial.log`: FIELD_DIAG 1/s
plus `T` every 5 s; VTX on ~57-171 s; `off_*.bin`: IQSNAP with the VTX off).
Findings only, no fix yet.

- **Nothing was applied.** `prog_switches=0`, `kp_switches=0`, `kp_level=0`,
  `program=U85Q` the whole run. The internal ladder moved (`kp_steps` 1 -> 8,
  `lp_switches` 0 -> 6) but `kp_switch_confirm_toward()` never confirmed.
- **The gain never left G59-60 for a weak carrier.** Table maximum is 83.
  With the VTX on: 76 of ~114 s at G59, none above 60, `sync_valid_pct=0`
  in all but 17 s; AFC never locked, deviation never measured.
- **With no carrier the IQ is a constant DC point, not noise.** VTX off,
  G59, four snapshots (`research/dc_coherence_check.py`): DC
  (-1.5, +2.9..3.3) cells, spread 0.06-0.08 (I) / 0.4-0.5 (Q) cells, 3 I
  and 2 Q codes in 8190 bytes. Receiver noise is below one LSB at G59.
- **The DC point reads as a strong coherent carrier.** dg_measure counts a
  phase step <= 45 degrees on power >= 8; a fixed point has no phase step,
  so coherence 99-100 %. `cn_estimate_x10` turns that into 14-20 dB
  (FIELD_DIAG `cn_est_x10=200` with the VTX off, before and after the walk).
  With the window DC removed the same captures read coherence 0 (noise
  floor). `carrier()` in gain.c (coherence >= 55, P50 >= 5) is also
  satisfied by the DC point, consistent with the gain parking at G59.
- **So the ladder steers on the DC, not on the VTX.** With the VTX weak the
  C/N estimate swung 0-12 dB second to second with `sync_valid_pct=0`; the
  wanted program crossed the 8.5 dB LP threshold (1.5 dB hysteresis) back
  and forth, and the 2 s directional confirmation restarted every time.
- **During the fade itself the ladder was blind.** 57-77 s: 50-120 gain
  writes per second; `kp_ladder_window_usable()` rejects any window with a
  write in the previous 100 ms, so the smoothed C/N was not even updated.

Where the DC comes from and why the gain stops at G59
(`range_diag_vtx_off.txt`, `T` DCO line; gain.c, calibration.c):

- **G59-60 are DCO FLOOR rows, so their DC is never corrected.** The DCO
  table covers the top RF stage, G54-83 (`top_rf_stage_from=54`): 14 rows
  OK, 16 FLOOR. `dco_quiet_search()` marks a row and every row below it
  FLOOR when the receiver noise there is below one quantizer step ("nothing
  to correct"), so FLOOR is G54-69. Below the noise step the DC is still
  there: (-1.5, +3) cells at G59, `dc_ok=1`, `dc_held=0`. The noise being
  below one step is exactly what makes the DC a clean constant.
- **That DC point sits inside the Direct Gain healthy band.** `healthy()`
  wants `carrier()` (coherence >= 55, P50 >= 5, origin < 650) and P50
  13..32; the DC point gives coherence 99-100, P50 ~15, origin 0. The gain
  climbs while the DC is below the band and holds as soon as the DC alone
  reaches it: G59-60, about 23 steps below the maximum. `no_carrier`
  (P50 <= 4, origin >= 650, coherence < 20) can never fire on it, so the
  "listen at the table maximum" path is never taken.
- **A weak VTX is therefore invisible.** At G59 even receiver noise is
  below one LSB, so a carrier near the sensitivity limit is too; noise
  only dithers the quantizer from ~G70 (first OK row). The range edge is
  set by where the DC point parks the gain, not by the demodulator.
- `RANGE_DIAG verdict=OK strong carrier with gain headroom` with the VTX
  off, for the same reason.

This is not PR 211 code: gain.c and the DCO FLOOR rule are unchanged by
it. PR 211's ladder only made it visible, because its C/N input is the same
coherence.

At the sync edge, after a reboot (`edge_serial.log`, `weak_*.bin`; VTX
placed where the picture is between desync and just video):

- The gain now sits at G70-76 (OK rows, `DCO held=1`) and receiver noise
  is visible (spread 1.8-2.5 cells, 11-13 codes per axis), DC still
  (+1.4..2.2, -0.8..-1.3) cells. Coherence 54-68 %, `cn_est_x10` mostly
  CN_UNKNOWN or -2..+3 dB, briefly 3-6 dB when sync returns.
  `dc_coherence_check.py` reads 3-17 points lower without the DC (50-68
  vs 33-65 %): the DC still lifts it, but no longer dominates.
- Still no switch in 40 s (`prog_switches=0`, `kp_switches=0`), internal
  `lp_switches` 4 -> 6. Here this is the curve, not a gate: the deviation
  is unknown (`deviation_x100=0`), so the ladder uses the wide class, whose
  LP window is 2.0-8.5 dB (`kp_lp_floor_x10` 1.5 dB + hysteresis/3) and
  whose kp row is 0.85 up to 2 dB. A noise-like window counts as 0 dB, so
  at the edge the wanted pair is U85Q kp 0.85, the pair already running.
- The deviation is never measured because `profile_observe()` needs
  `s_afc_locked`, and the AFC never locked in either boot, even close in
  (sync 52-62 %, `afc_corrections=0`). With no deviation the ladder can
  only ever use the wide class.

Open: whether the G59 park returns in this boot with the VTX off; why the
AFC never locks (burst gate `AFC2_BURST_MIN_X10`, CFO -476 kHz seen once);
an IQ capture of the weak carrier at G59 for comparison.

Hard check of the steering (`research/field_walk_analysis.py`,
`research/ladder_replay.py`: the C ladder and guard in Python, wide class,
each 1 s FIELD_DIAG C/N held for its 20 windows):

- **The C/N alone would have switched LP 15 times in the walk.** Best case
  (every window usable): first apply at 60.8 s, 15 internal LP changes, 15
  applies. Board: 6 internal changes (from 85 s), 0 applies. So a gate
  outside the C/N held the ladder.
- **One unusable window resets everything.** The guard wants 40 unbroken
  usable windows (2 s), the LP hold 20 (1 s). Random unusable windows in
  the replay: 1 % -> 13 applies, 5 % -> 3, 10 % -> 2, 20 % -> 0 (6 internal
  changes at 10 %, as on the board). `measurement_coverage_pct` while the
  C/N was in the LP band was 50 % (walk) and 49-62 % (edge); that counts
  the AFC envelope test and failed window copies too, so it bounds the
  ladder's share from below only. 57-77 s add the gain writes (50-120/s).
  Which gate fails the other windows is not visible in the log: `T` has no
  counter for unusable ladder windows or guard resets.
- **Even the best case flaps.** 15 program restarts in 114 s in the walk;
  at the edge LP is on for 2-4 s and off again, because a noise-like window
  counts as 0 dB, which is below the wide class's 1.5 dB LP floor.
- **The deviation decides most of the range and is never known.** The
  wide class (unknown deviation) keeps U85Q kp 0.85 below 2 dB and gains
  0.36 dB from the ladder in simulation; for deviation 1.0 the kp + LP
  ladder is worth 1.6 dB and for 0.69 3.35 dB (research README section
  16). The board never measures it because the AFC never locks.
- **Max gain is not the issue in boot 2.** At G70-78 receiver noise spans
  1.8-2.5 cells per axis, so the 4-bit quantizer is not what limits C/N
  there; more gain would mainly move the envelope out of the band. The
  G59 park of boot 1 is the gain problem (DC point, above).


### First fixes (same PR, 2026-10-13)

- **Fixed IQ point is no carrier** (`gain.h` `dg_stuck`, `gain.c`):
  `dg_measure` counts samples equal to the one before. VTX off at G59:
  476-873 per mille per 64 bytes; weak carrier in noise at G73-75: 15-301.
  At >= 400 per mille with P50 <= 32 the window is no carrier: Direct Gain
  listens at the table maximum (host test: G59 -> G83 on the board's DC
  point), and `receiver_obs_cn_x10()` returns CN_UNKNOWN for the ladder,
  FIELD_DIAG, the profile gate and the AFC strength test. This removes the
  G59 park and the 14-20 dB reading without a VTX; it does not correct the
  DC of the FLOOR rows (that needs a DCO search that works without noise
  dither, still open).
- **Unusable windows hold, they do not erase** (`kp_ladder.h`,
  `kp_switch_guard.h`, `control.c`): the pending kp/LP change and the switch
  candidate survive unusable windows and failed copies; a gap of more than
  500 ms without a usable window restarts them. Replay on the walk C/N
  (`ladder_replay.py ... p_bad new`): 10 % unusable 15 applies (old: 2),
  20-30 % 13 (old: 0).
- **Gain-write guard 5 ms** instead of 100 ms plus "no write during the
  window": the C/N is one observer probe taken right before it is read, and
  Direct Gain settles a write in 0.3-1 ms.
- **`T` LADDER line**: windows, unusable by reason (settle, gain write,
  context, copy failure), fixed-point windows, guard confirms and resets,
  current level, LP and the smoothed C/N.

Not changed yet: the wide-class curve with an unknown deviation (U85Q kp
0.85 below 2 dB, LP only 2-8.5 dB) and with it the LP flapping at the edge
(the replay best case still applies 15 program changes in 114 s); the AFC
never locking, so the deviation is never measured; the FLOOR-row DC itself.

### Walk 3 on 2d0088f, and a revert (2026-10-13)

`walk3_close_check.log`, `walk3_gain_loop.log`:

- **Close in (G25-36, 0-120 s):** no fixed-point windows (`stuck_iq=0`),
  but the C/N model still dipped below 8.5 dB in 19 of 115 s with a perfect
  picture, and LP was applied twice (46-51 s, 124-130 s). Next to the VTX
  the smoothed C/N sits at 9-13 dB, only just above the LP threshold: the
  coherence model reads too low on a strong real carrier (sim tables:
  deviation 1.0 PAL; a 45-degree step test also fails on fast FM swings).
- **Direct Gain looped** after the fixed-point change: with the VTX off
  ~1400 writes and hundreds of overloads a second, at the weak spots
  75-600/s. The fixed point sent the gain to G83; the first window after
  the write still carried the row's uncorrected DC and clipped, the
  overload dropped the gain into the FLOOR rows (18 this boot, G54-71, DC
  up to (-4.3, +3.8) cells, P50 51-87 with clipping) and round again.
  **Reverted for Direct Gain**: `dg_stuck` now only feeds the C/N model.
  The G59 park with the VTX off is back; the fix belongs in the DCO path
  (correct the FLOOR rows' DC), not in the gain controller.
- **Weak spot 147-195 s:** sync 0 at G60-67 (FLOOR rows) with coherence
  85-98 and C/N 4-12 dB: the DC point with a little carrier on it (span > 2)
  still inflates the coherence. Another reason to fix the DC itself.
- 12 program and 11 kp switches in 250 s; `copy_fail` 44 % of control
  windows (1812 of 4132).

### Second round of fixes (2026-10-13, not yet board-tested)

- **FLOOR-row DC** (`phy_dco_search_flat`, `calibration.c`, DCO_VERSION 7):
  rows whose noise is below one quantizer step get DCO codes by bisecting,
  per axis, the code where the fixed output code flips (DC within half a
  cell). Only rows without a flip in +-96 codes stay FLOOR. This targets the
  root of the G59-67 park ("black screen, gain stuck at 65"). Board check:
  boot log `DCO flat ... ok` lines, `T` DCO `floor_rows` near 0, gain
  reaching G75-83 with the VTX off.
- **LP follows the gain** (`kp_ladder.h` KP_LP_GAIN_ON/OFF): LP from G70,
  back below G65, with the fast entry and adaptive release above. kp within
  a program still follows the C/N curve.
- **AFC through missing windows** (`afc2_ctrl_miss`): failed copies,
  non-stationary windows and windows without a burst no longer clear the 16
  estimates; more than 20 in a row (1 s) do. Host test: two thirds missing
  keeps 14 of 16.
- **Control-window copy** holds the scheduler for the copy. Board check:
  `T` LADDER `copy_fail` near 0.
- `ladder_gate.py check` passes with the new timing (after 0.48-0.82 dB vs
  oracle 0.50-0.90).

Follow-up: AFC now also accepts syncs confirmed by the preceding line's
PAL/NTSC period, so a B/W camera can supply measurements without a colour
burst (`d199d02`). The short control window may contain only one sync;
such windows remain missing evidence. Host tests/build passed; board test
pending.

### Envelope C/N instead of phase coherence (2026-10-13, board test pending)

The phase-step coherence model read 9-13 dB next to the VTX and could select
weak-signal kp levels on a strong carrier. A fast FM swing also fails its
45-degree phase-step test, so the model depends on picture/deviation.

- `dg_measure` now computes the DC-removed envelope statistic
  `100 * mean(|z|^2)^2 / var(|z|^2)` on the existing observer probe.
  A fixed IQ point has no residual power and remains unknown. This extends
  the envelope statistic already used by calibration, outside the hardware
  sample path.
- `cn_estimate_env_x10` uses four filter-class tables. All runtime C/N
  consumers (kp, AFC correction, profile gates, diagnostic traces and
  per-second records) use the envelope estimate. Fixed-point windows are
  excluded from the per-second envelope average; a majority makes C/N
  unknown. `T` GAIN adds `env_x100`, `stuck` and `cn_x10`.
- Reproduction: `python research/cn_envelope.py`; full results in
  `research/data/cn_envelope.txt`. PAL, two seeds, deviations 0.69/1.0/1.4,
  256-byte probes matching `copy_probe_window`. Deviation 1.0 medians exactly
  reproduce the tables. Noise-only medians are 114-116; the unknown cutoff
  is 130. Deviations 0.69/1.4 differ by up to ~1.1 dB through 12 dB C/N
  (the earlier ~0.5 dB claim was too optimistic). Above ~15 dB, filter AM
  and quantisation limit accuracy; values saturate at 20 dB.
- Host checks cover analytic envelope moments, DC translations on both
  axes, fixed and rotating IQ, monotonicity over the full uint16 range,
  and the 6 dB AFC boundary for every filter class.
- Log audit: normal and narrow sometimes share the same setting at
  13.125 MHz, but the old class lookup matched narrow first and reported
  10 MHz. Normal now wins a shared setting; a distinct narrow setting
  measured wider than 11 MHz also uses the 12 MHz C/N model, matching the
  kp ladder's existing width boundary.
- `range_diag_vtx_off.txt` said `OK strong carrier with gain headroom`
  while `video_recent=0` and the last 60 s had no sync. The strong-estimate
  verdict now requires recent video evidence and is labelled
  `VIDEO_EVIDENCE`; it is not a claim about displayed picture quality.

Validation: all 13 C host executables pass (including 68 kp-ladder and 24
gain-fade checks), diagnostic-log tests pass, the full envelope sweep
reproduces the tables, and `ladder_gate.py check` passes. ESP-IDF v6.0.2
build passes without warnings.

### Gain-driven LP independent of IQ windows (2026-10-13)

Operator: from G70 the receiver should stay on LP. The gain thresholds were
already present, but failed copies exited the control loop before the policy
and gain-write guards held its confirmation. A decision based on gain still
depended on C/N window availability.

- Separate evidence for LP and kp: in AUTO kp+LP, G70 enters LP, G65-69
  retains it, and below G65 releases after the adaptive hold. The existing
  rescue confirmation/restart gap remains. Copy failures, unknown C/N and
  gain writes do not block the program policy; missing C/N holds kp.
- Retune settle, scan/calibration/menu ownership, unknown verified transport
  state and manual modes still block automatic switching. Pending program
  and kp evidence have independent stale timers. Failed copies remain
  missing AFC evidence and never read the stale sample buffer.
- Host regression follows G70 with no usable IQ through the actual switch
  confirmation: LP is confirmed within 1 s, without creating C/N or moving
  kp. It also covers G65-83 staying on LP, G64 release, ownership and manual
  mode. No hardware/range claim; board test pending.
- The envelope calculation uses integer raw moments in the existing sample
  loop and translates them to the DC centre once per probe. This removes
  the unfinished implementation's two extra passes and per-sample floating
  point. A direct centred oracle agrees within one ratio unit on 64
  asymmetric/correlated probes. Actual C5 observer timing remains to be checked.

These are simulation-derived decision estimates, not independently measured
RF C/N. After flashing, compare VTX-off and close-VTX logs, kp selection,
`afc_locked`, and `obs_us_max` to check the added observer cost on the C5.
