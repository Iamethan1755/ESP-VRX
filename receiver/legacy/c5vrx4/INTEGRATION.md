# Integration and issue disposition

This contribution extends C5VRX by Twotoz and the C5VRX contributors; the official
website also carries the Discord invite: https://twotoz.github.io/C5VRX/.
Base main is `b8d32b90f479feff7fbb34e6ca01cc9ba2d0d697`. The PR is deliberately
directory-only and must not merge until the project maintainer approves it.
Older PRs remain open; this integration neither closes issues nor silently
replaces their evidence.

## Donor decisions

| Source | Integrated result |
|---|---|
| Main / #24, #26, #31, #34 | Zero-EOF raw ring, recovery, menu allocation and PAL/NTSC raster |
| Main / #35–37, #43, #45–46, #52, #56–58, #62 | RF/range observations, vendor gain-table model, existing optional controllers/labs |
| Main / #78–81, #85, #89–90, #96, #106, #110, #112, #120 | Existing diagnostics and gain lineage; later V5 and Unwrap75 replace historical live defaults |
| Main / #126–127 | Direct Gain V5, 200-us observer, dedupe, anti-hunt and opt-in native ownership |
| Main / #129–130 | Isolated build lineage, sign-preserving lanes, noise cap, BW gear and gain fixes; two-bundle HC is not the C5VRX-4 live detector |
| Main / #131–132 | Analog-video scan confidence and strongest RF among centred candidates |
| #133, #142, #145, #146 | Direct Gain ownership, three-bundle Phase8, bounded history, trajectory Unwrap75 and fixed CVBS transfer |
| #143 | Protected upgrade/recovery routing and observation freshness; replaces the unguarded baseline lane comparison |
| #122 | Burst-free AFC V2 measurement/controller, context hygiene and bounded completed-descriptor copy; ported onto V5/75-ns supervision |
| #154 | PHY restoration, generation-checked serialized gain/retune ownership, stale sentinel rejection, pinned reversible filter/BW/11p/native-hold labs |
| #141 / #136 / #140 | Target pipeline research, not permission to enable speculative PHY writes |
| #157 | Output-swing regression reference; no Phase5 rollback in this PR |

The complete reviewed PR/commit list is in INTEGRATION_SOURCES.json. Merged
features are inherited as implemented on current main, not reconstructed from
obsolete patches. Selective ports keep the newer V5, ownership and scan logic.

Leon chose fixed fine lanes {9,7,6,5} as the default on 2026-10-04: no runtime
lane switching for any gain owner. Lanes and BB gain are the same amplitude
knob for pre-ADC noise, and with ~35 codes of receiver noise at maximum gain
(docs/range-max.md) one fine step is close to one noise sigma; the analog loop
places the envelope inside the fine window. A fixed lane has no fold escape, so
its severe clipping takes the G20 floor. PR #146's fixed ultrafine and the
protected adaptive V5 lanes remain Z comparisons under the new `lane_mode` key.
This removes the six sequential GPIO writes from normal operation; it is not a
range measurement, and DC centring, sampling phase and filter width still
decide whether a finer fixed lane becomes better later.

The AFC supervisor uses PR122's adjacent raw Phase8 reference measurements,
while live output and slow sync scoring use the stride-3 winding detector. These
different cadences are explicit. Both-endpoint validity, burst confirmation and
the burst-free porch window replace whole-scene CFO. Gain HOLD and AFC video
TRACK are separate: only the latter freezes acquisition corrections. AUTO is
off by default. Same-window software epochs cannot reveal every native AGC
transition; stationarity remains a refusal heuristic, not an analog-state proof.

## Issues and remaining gates

| Issue | Software disposition / remaining proof |
|---|---|
| #144 | Unwrap75 and exact routing/oracle retained; adjacent-step bound remains |
| #111, #113, #103 | Saturating loaded DAC map after winding resolution; no multiplied modulo counter terms; physical swing/calibration pending |
| #114 | C5VRX-4 sync scoring uses Phase8 trajectory/transfer instead of the Phase5 shadow; local tests run during configuration; snapshot alignment/history remain estimates |
| #115 | Burst-confirmed clean-porch AFC, consecutive robust evidence, context/settle/copy rejection, bounded acquisition and sticky zero-write video TRACK; VTX sign/reference calibration pending |
| #119, #123, #125 | Sign-preserving adaptive finer lanes, noise cap, protected handover and fold escape; eight-wire precision/folding limits remain |
| #128 | Current-main scanner fixes retained; multi-VTX/Wi-Fi hardware acceptance pending |
| #139, #150–153, #155 | PR154 ownership fixes and reversible pinned lab controls retained; automatic native/video-aware AGC, matched passband and range proofs remain open |
| #158 | New severe coarse-clipping G20 escape with stale-settle refusal; fine-lane folding escapes first; manual/native ownership untouched |
| #117–118, #121, #124, #134–138 | Existing native/PHY observations and research preserved; no unverified automatic native policy, FFT live source or PHY replacement enabled |

Issue #158 reports 73.8% clipping and no HDZero image at very close VTX distance,
with picture returning farther away. Its subsequent comment says this still
needs verification. Treat it as an overload report, not a controlled conclusion
that DAC swing was never a factor. The >=50% coarse-clipping trigger is an
explicit engineering threshold; it is covered by policy tests but not validated
on hardware. Ordinary tested noise does not trigger the severe path. Manual
gain intentionally remains manual; select active Direct Gain for this test.

## Rejected combinations

- PR #83's original whole-back-porch estimator includes color burst and is
  superseded by PR122's reference windows.
- #100's Golden guard, #102/#104's two-bundle higher-slope mapping and #108's
  older predictive gain core do not replace the selected three-bundle/V5 path.
- VIDEO32, guessed native target/hysteresis, fixed-max RF patches and automatic
  filter overrides are not promoted to defaults merely because a branch exists.
- Simultaneous RX/TX BitScrambler, >40 MB/s live transport and CPU flywheel
  sample repair remain excluded by the recorded hardware/throughput findings.
- PR164 enables bounded sync-referenced output regulation by default at Leon's
  request; concurrent LUT/FIFO/goggle acceptance remains pending. No endpoint-only IQ DC correction, H/V regeneration or
  frame buffering is inserted. Missing RF/phase information cannot be restored
  by output scaling alone.

## Verification and acceptance

`python3 verify.py` checks 21 C regressions (gain/range, epochs, protected lanes,
severe overload, menu, AFC and pinned/unpinned PHY lab lifecycle), 48 synthetic
PAL/NTSC AFC cases, all 524,386,048 bounded unwrap trajectories, routing/ring
state, source-driven Phase8/Golden/HC models and fixed/legacy CVBS transfer/noise
refusal. It also rejects stale generated tables and shared-main build coupling.
The same script is mandatory during IDF configure through the existing alpha CI.
It is a host proof of these properties, not a physical source/DAC oracle.

Before approval to merge: require successful exact-head ESP-IDF 6.0.2 alpha
build/publication and verify that the PR changes only this directory. Before
calling range or HDZero acceptance complete, bench-test:

1. Full-firmware flash, active Direct Gain, fixed setup; record antenna/channel,
   VTX/camera, power, distance/attenuation, firmware SHA and settings.
2. One 75-ohm load: sync, porch, white/chroma and dynamic output; HDZero live
   view and recording in PAL/NTSC. Compare M transfer modes.
3. Strong input: coarse/fine escape, severe clipping recovery and gain-write
   counts. Manual/native remain excluded from automatic gain interventions.
4. Weak-input sweep: lane/fold events, origin/clip/phase coverage, usable video,
   recovery and FIFO faults. Compare fixed fine, fixed ultrafine and protected V5 via Z.
5. AUTO AFC only with a known VTX reference: injected offset/sign, scene/burst
   immunity, bounded acquisition and zero writes in AFC video TRACK.
6. Menu exit, channel/BW changes, N/h/Z/M reboots, native labs/rollback, settings,
   USB diagnostics, supervisor/J stack and menu heap margin; prolonged soak.

No issue is declared closed from software integration alone. Main merge remains
an explicit operator decision after reviewing this concrete PR and its evidence.

## Output-amplitude follow-up

The distance-dependent voltage report motivates the default-on `u` sync/black
level servo, with stopped-engine LUT16 addressing probe and bounded DAC-only
live writes. CVBS_LEVEL.md records its implementation, refusal conditions,
physical-source ambiguity and required bench acceptance. This does not establish
the cause of the observed amplitude fall or recover RF information.

## PR164 default-on follow-up (2026-10-04)

Extends Leon's weak-signal amplitude report and the existing level lab: a separate
adaptive 5/20-ms supervisor copies 8190 completed IQ bytes, requires three consistent
period/plateau snapshots and targets 286/300-mV sync with bounded 32-mV electrical slew.
Default-on is explicitly operator-authorized; explicit off settings remain off.
RF gain ownership and AFC defaults are unchanged. Host tests cover half-depth
fades, loss/stale hold, NTSC/PAL targets, all line alignments and ring-wrap/deadline
refusal. It needs 8190 heap bytes plus a 16-KiB stack. LUT readback and transport
faults after updates latch off until reboot. Physical arbitration, response time,
colour/white clipping and goggle/FIFO/heap acceptance remain unproven.

## RF-gain transition follow-up

Level recovery uses three new 5-ms snapshots after Direct Gain settling, within
a 100-ms fast interval before returning to 20 ms. Gain/PHY/lane history guards
are independent; invalid/replayed evidence holds the output. The DAC slew now
uses loaded voltage rather than numeric code distance, covering resistor carry
steps and measured nonmonotonic tables. The host gain-step regression recovers
both offset and half-depth changes within 75 ms of valid settled evidence.
Physical transition static, concurrent LUT timing and goggle acceptance still
need bench evidence; no unknown PHY/DC actuator or CPU waveform path is enabled.
