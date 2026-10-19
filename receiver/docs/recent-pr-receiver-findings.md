# Additional receiver findings from recent PRs

Reviewed on 2026-10-11 alongside the [PR #190/#192 extraction](pr190-key-findings.md).
These are selected additions from open receiver PRs and their recent closed
predecessors, not a complete audit of all historical PRs. Main remains the
production authority. No code, binaries, changed defaults or proposed hosting
changes are imported by this document.

This preserves **C5VRX by Twotoz and the C5VRX contributors**, including Louis
Hitchcock's DC-hold and staged-recovery work. [Canonical project](https://github.com/Twotoz/C5VRX);
[official website and Discord invite](https://twotoz.github.io/C5VRX/).
Existing zerowidth/C5VRX, ESPARGOS and Espressif credits remain applicable.

## DC hold must survive actuator writes (#177/#180/#181)

Louis's #177 investigation found that gain writes and PHY restores released the
per-gain DC hold. A 250 ms service tick could claim a held correction while the
actual receiver remained offset between writes. His R8/no-VTX bench comparison
reported median DC around 134 ADC LSB with tick-only holding versus 11 LSB with
an immediate post-gain/restore hook (about 155 LSB uncorrected).

The hook alone still rolled the picture: V5 repeatedly wrote the gain already
in force, releasing/reapplying DC while changing no useful gain. Skipping that
redundant write **when the hold and ownership conditions permit** removed rolling
in the reported flight comparison. Preserve actuator/PHY ownership and settling
checks, rather than treating every equal-index write as globally redundant.
The tested flight build was an earlier #174 base plus the fixes, not the exact
submitted #177 combination. It is a useful physical finding, not isolated dB proof.

The #180/#181 audit adds context-qualified calibration and maps: channel,
lane policy, filter/IQ configuration and gain-table identity can change the
meaning of a cached result. It also records that automatic exact-frequency
vendor RX recalibration changed gain per index and produced grain/clipping on
a strong board carrier; that route was returned to an explicit lab. Do not
assume a recalibration merely updates DC or that a saved gain index implies
the same analog amplitude after recalibration.

Sources: [#177 report at `897c27c`](https://github.com/Twotoz/C5VRX/pull/177),
[#181 audit at `c1d9f1975ad5a8a7911d93290ab373f0dd59ed53`](https://github.com/Twotoz/C5VRX/blob/c1d9f1975ad5a8a7911d93290ab373f0dd59ed53/v4/docs/AUDIT_PR174.md).

## Gain recovery and the identity of the tested firmware (#182/#184)

The staged-recovery comparison reduces BB first, then RF through known physical
tuples on severe clipping. After overload it waits for settling and climbs one
physical step per remeasurement, instead of interpreting transient near-origin
IQ as real carrier loss and jumping directly to maximum. Healthy HOLD clears
recovery; persistent overload can still reach G20 and real loss can still reach
maximum sensitivity. Manual/native ownership remains separate.

Louis reported V3-level picture and flight range for HR50 plus this recovery
policy (app SHA256 prefix `7D29A002`). This is a valuable working comparison
baseline with recorded settings, but not a controlled attenuation result or
proof that the gain change alone caused all improvement. Golden was not tested
in that flight. Earlier settling-guard success on the host had failed live
acceptance; preserve that chronology rather than attributing the flight result
to the guard alone.

#184 contains an especially reusable build lesson: configuration-time verification
regenerated nine hand-swapped HC50 programs into Unwrap75 STD150 before the
bypassed result check. Published #183 and local binaries contained the span75
LUT six times and no HC50 LUT. Its accepted board result therefore belongs to
**Unwrap75 STD150, flywheel off, plus staged recovery**, not HC50/span50.
Validate the linked LUT/program and boot fingerprint of the flashed image;
source filenames and a successful build cannot identify the tested algorithm.

Sources: [reference/flight chronology at reviewed #190 head](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/V3_BENCHMARK.md),
[#184 correction at `fa47bed64faa130c88e93da4cf2781a65f2e0abf`](https://github.com/Twotoz/C5VRX/blob/fa47bed64faa130c88e93da4cf2781a65f2e0abf/v4/docs/STAGED_GAIN_SPAN50.md).

## Match AFC to the output window (#193)

Most #193 U85Q findings repeat #190. Its new useful lead is that the inherited
-436 kHz porch target was designed for other trackers. A synthetic U85Q
comparison shifts the target to -1036 kHz: modeled displayed-picture fraction
0.908 to 0.969 and SINAD 2.42 to 3.27 dB; deviation-1.8 cases improve from 0.64
to 0.90. Moving farther to -1436 kHz improves SINAD but reduces overall picture
fraction to 0.961. **Board confirmation of the target is pending.**

The general lesson is to select carrier placement jointly with the demodulator's
sync-side/white-side headroom and unseen VTX deviations, not copy an AFC target
from a different transfer. The gain-band sweep in the same study supports
keeping the existing band in those synthetic conditions, rather than maximizing
IQ radius until folding hurts detail.

#193 reports board boot/menu/video acceptance but still lists post-flash watchdog
reset startup as an unresolved issue and first-boot calibration/flight comparisons
as pending at the reviewed revision. #193 was subsequently merged into main
as `ffcea524ba69438457390686940328d3842944d8`; its packaging is now in main.
The open physical gates and lack of a global C5 information-limit proof remain.

Sources: [#193 findings at `59aee50b26651ec23db98bb2971fcd0b7e0c6745`](https://github.com/Twotoz/C5VRX/blob/59aee50b26651ec23db98bb2971fcd0b7e0c6745/v5/docs/FINDINGS.md),
[#193 board status](https://github.com/Twotoz/C5VRX/pull/193).

## Received IQ can retain timing after the current goggle path loses it (#195)

The sparse observer accumulates adjacent complex IQ products by trial PAL/NTSC
line phase, then tests sync/porch contrast. It estimates timing from received
IQ and declared standard priors without video truth, true C/N or supplied carrier
offset. Small near-origin products contribute little before angle extraction.
This is an H-clock observer, not a complete IQ-to-picture demodulator.

In 16 fresh off-centre echo/fade cases at synthetic 6 dB, it selected the correct
standard and held timing within 500 ns after removing the simulator's declared
500 ns FIR delay. U85Q goggle H-lock averaged 0.953% in that stress class.
Another 16 fresh cases support stride127/4 ms acquisition. These compare different
outputs: timing acquisition versus a goggle path. They do not show repaired
picture, color, physical C5 execution or flight range. Broader low-C/N and
false-timing failures remain; zero locks in small noise-only sets do not bound
rare false locks.

Two practical corrections are worth keeping:

- Stride64 revisited only 40 phases of a 2560-sample PAL line. Sparse sampling
  must cover line phase; integer chunk equivalence does not cure a commensurate
  sampling pattern or unmodeled camera-clock drift.
- The 3 KiB histogram C prototype compiled to a 122-instruction steady path,
  exceeding the earlier optimistic 60-cycle budget. At stride127 the conditional
  one-instruction-per-cycle estimate is 16.0% of a 240 MHz CPU, excluding stalls,
  multiplication latency, scoring, contention and preemption. It is not measured
  CPU utilization. Four-ms online accumulation saves sample rate, not acquisition
  latency, and requires explicit ring-lap/gap handling.

The same archive invalidates an earlier counter simulation (wrong LUT bank and
counter input semantics) and records official assembler rejection of overlapping
LUT16/B-counter routing. Host dataflow success cannot rescue an illegal source.
No candidate from those families was promoted.

The useful next hypothesis is received-only sync/porch-aided carrier acquisition.
The current actuator clamps offsets to +/-1.5 MHz, suspends/restores the live path
and requires at least 100 ms after a PHY write before AFC observation. An
instantaneous, unlimited, continuous-retune model is therefore misleading.
Safe DMA ownership, V/field timing, burst preservation and actual output benefit
remain unresolved; an H-clock result does not authorize synthetic picture repair.

Sources: [#195 timing design at `d9e4d26c155d89cded5f464d4a99e41bf49ea68e`](https://github.com/Twotoz/C5VRX/blob/d9e4d26c155d89cded5f464d4a99e41bf49ea68e/v4/docs/LOCAL_IQ_CLOCK_DESIGN.md),
[rejected demod/source ledger](https://github.com/Twotoz/C5VRX/blob/d9e4d26c155d89cded5f464d4a99e41bf49ea68e/v4/docs/LOCAL_DEMOD_RESEARCH.md),
[reproduction and actuator bounds](https://github.com/Twotoz/C5VRX/blob/d9e4d26c155d89cded5f464d4a99e41bf49ea68e/v4/docs/LOCAL_RESEARCH_INDEX.md).

### Carrier follow-up before closing #195

Reviewed again at `2e9929ee43791f0a9625bc49928b5588358a35b4`. Correlated noise
biases adjacent-product angles even when H timing is correct. On 16 fresh
stress cases, plateau covariance fitting reduced mean porch-frequency error
from 792.5 kHz to roughly 115 kHz; the simpler long-lag-only fit was as accurate
as the model-dependent colored-noise fit. An earlier gate admitted noise, and
a revised gate still admitted a wrong 4 dB PAL clock. Full-pulse consistency
rejected that known failure on hash-matched replay and retained 16 new stress
cases, but an admitted nominal NTSC clock still had 0.665 us p95 timing error.
Frequency-acquisition admission therefore cannot authorize H-pulse synthesis.
Zero admissions in 64 fresh noise controls is not a rare-false-alarm bound.

Eight separate full-field oracle cases tested known-direction 1 MHz RF centring
before filtering: mean goggle H lock rose from 3.94% to 94.75%, while luma SINAD
fell from 1.04 to 0.46 dB. This isolates a possible acquisition benefit without
establishing better active picture or a working received-only controller.
The experiment omits the actual stepped/settled PHY trajectory. The carrier
estimator remains Python research, without an integer kernel, measured deadline,
physical filter validation or connected closed-loop retuning.

Source: [latest carrier design and negative controls](https://github.com/Twotoz/C5VRX/blob/2e9929ee43791f0a9625bc49928b5588358a35b4/v4/docs/LOCAL_IQ_CARRIER_DESIGN.md).

## Other reviewed PRs and scope decisions

| PR / lineage | What to retain without importing the branch |
| --- | --- |
| #168, open at `f1362b4064c2ec243ed0878d4757c427dc0acf2a` | Phase8 FULL's counter scaling cannot simply be increased to standard CVBS swing: modulo wrap can corrupt sync/white, and a saturating lookup needs another bundle. Confirm loaded DAC voltage and actual goggle acceptance. Its digital DC/sampling port and live LUT writes remain hardware-pending, not accepted production fixes. [Source and gates](https://github.com/Twotoz/C5VRX/pull/168) |
| #178/#179/#180/#181 | Runtime organization is secondary to the actuator/DC/cache evidence above; merged host code is not combined-image physical acceptance |
| #183/#185/#186/#187 | Inherited optimizer/range/PLL research and quarantines are already represented by #190's studies; keep the #184 build correction when interpreting their board ancestry |
| #42, open at `d7bc24dd4c8af825524e78e7cae9e49b3be2656b` | Separate S3 display/link experiment with reported ~25 fps; not continuous analog CVBS or a measured sensitivity gain. [Experiment](https://github.com/Twotoz/C5VRX/pull/42) |

Recently merged tooling, wiring and hosting changes already on main are not
re-imported. Open/closed PR descriptions and operator reports are reviewed
evidence, not independent reruns. Exact source links above freeze the extracted
research; PR-page status can subsequently change.
