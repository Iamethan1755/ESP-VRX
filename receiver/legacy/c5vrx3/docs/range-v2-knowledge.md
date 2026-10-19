# C5VRX Range v2 — Engineering Knowledge Base

This file preserves the technical reasoning behind PR #43 and Range v2 so the project does not depend on chat history.

Legend:
- PROVEN — measured, established by current hardware/repo evidence, or an architectural fact of the code.
- IMPLEMENTED — present in Range v2, but still requires hardware A/B validation before claiming a range improvement.
- HYPOTHESIS — technically plausible research direction; not yet proven on C5VRX hardware.

The distinction matters: C5VRX should never turn an attractive RF/DSP theory into a range claim without a controlled test.

---

## 1. Current realtime architecture

PROVEN

~~~text
5.8 GHz analog FPV RF
 -> ESP32-C5 5 GHz RX front end
 -> MODEM_DIAG packed Q4/I4 @ 40 MS/s
 -> PARLIO RX
 -> 16 KiB cyclic raw-IQ GDMA ring
 -> BitScrambler Phase5 WBFM demod
 -> [D,D] 40 MHz transport / 20 MS/s unique CVBS
 -> 6-bit resistor DAC
 -> analog display/goggles
~~~

The live video path is hardware-driven. CPU control code is supervisory and must not become a sample-by-sample realtime dependency.

Design rule: any range feature that requires the CPU to keep up with all 40 million Q4/I4 samples per second is suspect until proven otherwise.

Preferred pattern:

~~~text
hardware realtime data plane
+
slow/read-only CPU measurement and control plane
~~~

---

## 2. Separate three different "range" limits

A bad picture does not automatically mean the RF front end has stopped receiving a carrier.

### A. RF/front-end sensitivity

Can the analog front end still deliver useful I/Q information?

Influenced by antenna efficiency and polarization, feed loss, RF gain stages, LNA noise figure, blocker/compression headroom, and analog bandwidth.

### B. FM demodulation threshold

Can the demodulator still recover the correct phase trajectory from weak/noisy/quantized Q4/I4?

Influenced by Q4 quantization, near-origin I/Q, phase ambiguity, endpoint winding loss, discriminator design, PLL/phase continuity logic, filtering and decimation.

### C. Usable CVBS threshold

Does the recovered waveform still contain valid enough sync/video that the display remains locked?

Influenced by isolated FM errors, error bursts, malformed H-sync/V-sync, output filtering and display decoder relock behavior.

Key conclusion: optimize usable-video threshold, not merely RSSI or raw power.

---

## 3. Q4/I4 is the critical information boundary

PROVEN architectural fact

MODEM_DIAG exposes signed 4-bit I and 4-bit Q.

Once useful analog information has collapsed into too few Q4 codes, downstream digital multiplication cannot recreate it.

Every proposed RX control should therefore be classified as:

1. pre-Q4 — can plausibly improve actual information entering Q4;
2. post-Q4 / digital scaling — can change representation or headroom, but cannot restore already-lost analog information.

This is why FFT/digital scaling must be measured rather than assumed to be "more RF gain".

---

## 4. Production Phase5 endpoint problem

PROVEN / issue #23

The compact production demod chooses samples at 20 MS/s and effectively computes a phase delta over 50 ns:

~~~text
n ---- middle sample ---- n+2
|                         |
+------ endpoint d02 -----+
~~~

The exact 40 MS/s trajectory contains:

~~~text
d01 = phase(n+1) - phase(n)
d12 = phase(n+2) - phase(n+1)

exact trajectory = d01 + d12
~~~

Important rule:

Do not wrap d01 + d12 again before video mapping.

A second wrap can throw away exactly the winding information the adjacent-sample method recovered.

Existing Q4 analysis found approximately:
- endpoint n -> n+2 winding loss: 8.351% over the analyzed dataset;
- strong-IQ subset winding loss: 0.285%.

Interpretation:
- many endpoint disagreements occur in weak/low-confidence IQ;
- endpoint failure is not purely a strong-signal algebra problem;
- weak-signal phase continuity is a real candidate for extending usable range.

Do not translate those percentages directly into a dB range claim.

---

## 5. Exact-adjacent FM target

HYPOTHESIS with strong existing evidence

Preferred mathematical chain:

~~~text
Q4/I4 @ 40 MS/s
 -> phase every sample
 -> exact adjacent FM @ 40 MS/s
 -> confidence-aware repair only where justified
 -> low-pass / combine
 -> 2:1 decimation
 -> CVBS @ 20 MS/s
 -> stable [D,D] 40 MHz DAC transport
~~~

Advantages:
- keeps the middle sample;
- preserves trajectory winding;
- separates demodulation from rate conversion;
- makes confidence handling possible before information is discarded.

Why it is not production yet:

Historical exact-adjacent BitScrambler work has hit:
- instruction/scheduling limits;
- finite-run state boundary issues;
- M2M/block boundary complexity;
- state continuity concerns;
- realtime throughput uncertainty.

Gate:
1. prove benefit on the same raw Q4 capture;
2. prove realtime throughput/state continuity;
3. only then replace the Golden Phase5 path.

---

## 6. PLL / stateful phase tracking

HYPOTHESIS

A memoryless discriminator turns a noise-driven phase jump into a large instantaneous FM impulse.

A stateful PLL can maintain a predicted phase/frequency trajectory and treat short inconsistent observations as noise.

Potential benefits:
- fewer isolated phase impulses;
- better weak-signal threshold;
- fewer false CVBS sync events;
- graceful handling of brief low-IQ samples.

Potential costs:
- too-narrow loop bandwidth can remove valid wideband FM detail;
- too-slow tracking can distort sync/chroma;
- realtime implementation may be expensive;
- synthetic FM success does not prove real FPV CVBS success.

Range v2 therefore includes an offline PLL benchmark, not a production PLL.

Tool: tools/range_demod_bench.py

---

## 7. Confidence-aware phase repair

HYPOTHESIS / offline experiment

A repair should not modify every large phase delta.

Candidate rule:
- only consider repair when I/Q power is low or close to origin;
- require the phase delta to disagree strongly with local trajectory;
- use nearby slope/phase consensus;
- preserve large valid FM excursions when confidence is high.

Strong-IQ large phase motion can be legitimate modulation. Blind smoothing can make the picture quieter while destroying actual CVBS information.

---

## 8. Temporal observation: what changed and what did not

### Old supervisory sampling

PROVEN

One completed 4092-byte descriptor every 50 ms:

~~~text
4092 / 40,000,000 = 102.3 us of RF
20 observations/s
~2.046 ms observed per second
~0.205% time coverage
~~~

This can completely miss short fades between control snapshots.

### Range v2 observer

IMPLEMENTED

512 bytes every 6 ms:

~~~text
512 / 40,000,000 = 12.8 us per snapshot
~166.7 observations/s
~2.13 ms observed per second
~0.213% time coverage
~~~

Important nuance:

Range v2 does not dramatically increase total RF-time coverage. It distributes roughly the same tiny observation budget much more evenly in time.

The benefit is temporal sampling density while CPU and memory pressure stay bounded.

The fast observer remains read-only, based on completed DMA memory, outside live-video pacing, and separate from the slower physical actuator.

---

## 9. Fast observer, slow actuator

IMPLEMENTED design principle

Do not equate faster measurement with faster PHY writes.

Gain writes, BW changes and offset writes may disturb RF/phase state or CVBS sync. Repeated control writes can create exactly the glitches the controller is trying to solve.

Preferred pattern:

~~~text
fast sensing
 -> state estimate
 -> hysteresis/confidence
 -> rare physical action
~~~

not:

~~~text
fast sensing
 -> constantly write PHY
~~~

---

## 10. Temporal Fusion signals

IMPLEMENTED

Range v2 maintains fast and slow integer EWMAs for:
- Fusion quality;
- confidence;
- Q_phase;
- near-origin rate;
- winding;
- lag-4 disagreement;
- clipping.

Derived state:
- fade_score;
- recovery_score;
- stability;
- fast-minus-slow deltas.

Example:

~~~text
quality_fast << quality_slow
origin_fast  > origin_slow
winding_fast > winding_slow
~~~

means RF state is deteriorating now even if the slow average still looks acceptable.

---

## 11. Fusion is estimator fusion, not RF diversity

PROVEN terminology rule

Current Fusion combines multiple measurements from one receiver:
- adjacent phase behavior;
- lag-2 evidence;
- lag-4 trajectory evidence;
- robust local slope;
- consensus outliers;
- Q_phase;
- low-IQ/near-origin evidence;
- clipping;
- I/Q balance;
- bounded semantic sync validation.

It does not currently combine:
- two antennas;
- two C5 receivers;
- multiple simultaneous gain states;
- multiple coherent RF streams.

Do not describe current Fusion as MRC or physical diversity.

---

## 12. One C5 cannot use all gain states simultaneously

PROVEN architectural constraint

The ESP32-C5 has one active receive state at a time.

Software can learn from previous gain transitions, predict neighboring gain response, time-multiplex experiments, and use vendor AGC as a temporary oracle.

It cannot obtain independent simultaneous Q4 streams from G62, G58, G54, etc. on one RF chain.

"Combine all gains" therefore means learn or infer across time, not physically receive every gain state at once.

---

## 13. Gain is ordered, not a set of unrelated arms

PR #43 originally treated each gain state mostly as an independent contextual arm.

IMPLEMENTED in Range v2

The modeled chain is:

~~~text
G62, G58, G54, G50, G46, G42, G38, G34,
G28, G22, G16, G8, G2
~~~

The learner records local transition evidence such as:

~~~text
G58 -> G62:
quality +x
risk -y
~~~

This lets future choices reuse measured local response instead of blindly reproving every state.

Profile distinction:
- FUSION EXP keeps the conservative G34 floor;
- RANGE V2 may go down to G2 for near-field overload/headroom.

---

## 14. Gain response is not assumed globally monotonic

PROVEN design rule

Do not assume:
- higher numerical gain is always better;
- lower gain is always cleaner.

The internal C5 RX chain may contain stage boundaries and nonlinear behavior.

Gain indices may cross internal LNA, baseband, filter or headroom regions. Range v2 therefore learns local transitions and uses hard safety rules for larger moves.

---

## 15. Stale learning is dangerous

PR #43 used an EWMA mean while monotonically increasing visits.

That creates a mismatch:
- quality history is gradually forgotten;
- statistical certainty keeps increasing forever;
- exploration eventually collapses even after the RF environment changes.

RF is non-stationary because of distance, orientation, shadowing, multipath, channel changes and blockers.

IMPLEMENTED: Range v2 decays effective visit counts so old certainty does not remain permanent.

---

## 16. Change-point detection

HYPOTHESIS / future

Visit decay is simple and safe.

A future integer change detector could use Page-Hinkley, CUSUM, or sustained fast/slow divergence.

On a confirmed change point it could reduce confidence in old cells and temporarily widen exploration while keeping safety bounds.

Do not reset learning on every small fade.

---

## 17. Hard contexts versus soft state

Current Fusion classifies:

~~~text
NO_CARRIER
WEAK
CLEAN
BLOCKER
OVERLOAD
~~~

KNOWN LIMITATION: hard thresholds can flap near boundaries.

HYPOTHESIS / future: add hysteresis, soft probability-like context weights, or a tiny fixed-point HMM/state filter.

Then the controller could reason approximately:

~~~text
70% WEAK
20% BLOCKER
10% CLEAN
~~~

rather than instantly snapping between labels.

---

## 18. NO_CARRIER versus blocker ambiguity

KNOWN LIMITATION

Poor phase coherence can mean:
1. genuinely no useful carrier;
2. strong non-FPV interference or blocker;
3. severe overload;
4. badly centered desired carrier.

A blind bad-phase -> maximum-gain rule can make cases 2 and 3 worse.

Useful evidence includes median power, origin rate, clipping, winding, trajectory consistency, frequency-centering response and vendor RX state.

---

## 19. Optimize tails and bursts, not only averages

Analog CVBS can fail because of rare large errors even when the average waveform looks acceptable.

Useful metrics include:
- p95/p99 discriminator error;
- phase-slip rate;
- longest hard-error burst;
- hard-error clusters;
- sync-adjacent error rate;
- decoder relock count.

IMPLEMENTED partially: Range v2 adds catastrophic_risk so average quality cannot fully compensate for dangerous phase behavior.

Future: explicit p95/p99 and burst-length tracking.

---

## 20. Catastrophic risk versus average quality

IMPLEMENTED

Range v2 keeps a separate risk objective derived from:
- near-origin IQ;
- endpoint winding;
- strong-IQ winding;
- lag-4 disagreement;
- consensus outliers;
- low-confidence intervals;
- clipping.

Why separate it?

~~~text
+ nice power
+ decent Q_phase
- terrible phase-slip tail
= "average looks okay"
~~~

That is a bad objective for analog video.

Gain trials now accept a state when it improves quality without materially increasing risk, or materially reduces risk without a large quality regression.

---

## 21. Semantic sync is useful, but bounded

PROVEN design rule

H-sync/V-sync quality measures actual video usability, but a controller must not become circular:

~~~text
bad sync -> gain change -> transient -> bad sync -> more gain changes
~~~

Semantic sync is therefore a validation signal, not the sole RF estimator.

Raw IQ and phase evidence come first.

---

## 22. Gain/BW/AFC writes can themselves damage video

PROVEN issue #28 research direction

A visible 50–300 ms "lag" can originate from a much shorter upstream glitch.

Possible causes:
- gain transient;
- frequency-offset write;
- BW switch;
- malformed CVBS sync;
- display decoder relock;
- actual transport starvation.

A user-visible pause does not prove the CPU or DMA stalled for that entire duration.

Control implication: correlate transport faults, PHY writes, lag marks and sync degradation. Minimize unnecessary writes.

---

## 23. Per-transition glitch cost

HYPOTHESIS / future

Not every gain transition may have the same transient cost.

If internal RX stages switch at certain boundaries, future learner state can include:

~~~text
transition_quality_delta
transition_risk_delta
transition_glitch_cost
~~~

Then it can avoid a disruptive stage crossing even when steady-state score is similar.

This connects Range v2 directly with issue #27 gain-chain characterization.

---

## 24. Trial methodology: moving RF makes A/B hard

KNOWN LIMITATION

PR #43 used roughly:
- 500 ms settle;
- 400 ms evaluation.

That is long enough for antenna orientation or multipath to change.

Range v2 improves trend awareness but does not completely solve paired RF experimentation.

Better future methods:
- stable averaged pre-trial baseline;
- trend correction;
- shorter post-write windows after measured settling time;
- repeated A/B/A in bench tests;
- never infer hardware-stage superiority from one moving-flight transition.

---

## 25. Confirmed fast-fade path

IMPLEMENTED

Normal decisions remain deliberately slow.

For a state that is simultaneously:
- WEAK;
- high catastrophic risk;
- showing a strong fast/slow negative trend;

Range v2 can make one local sensitivity move after about 150 ms instead of waiting for the normal multi-second decision age.

Constraints:
- one local step;
- not continuous hunting;
- clean state remains no-write;
- hard overload can move in the opposite direction.

---

## 26. Near-field overload

PROVEN problem class / IMPLEMENTED control headroom

Near the VTX, excessive receiver gain can cause clipping, distorted phase and bad picture despite huge RF power.

A range profile that never allows gain below G34 can therefore be worse nearby.

Range v2 allows headroom down to G2 under overload. This removes an artificial software floor; it does not claim G2 is always optimal.

---

## 27. Clean-state zero-write rule

IMPLEMENTED and important

When video and IQ are confidently clean, the best control action is often:

~~~text
do nothing
~~~

A small score improvement is not worth a gain, BW or AFC transient and possible decoder relock.

---

## 28. Range v2 reboot semantics

IMPLEMENTED

RANGE V2 restores a deterministic controller shape after reboot.

It starts from the proven wide-video configuration and permits acquisition-only adaptation according to the profile.

Persisted old menu fields must not silently turn RANGE V2 into a different controller.

---

## 29. FUSION EXP versus RANGE V2

### FUSION EXP

Conservative smart baseline:
- temporal/risk-aware learning;
- conservative G34 floor;
- fixed RF shape.

### RANGE V2

Broader experiment:
- same Fusion intelligence;
- G2 near-field headroom;
- acquisition-only BW adaptation;
- acquisition-only AFC behavior.

Keeping both makes A/B interpretation cleaner.

---

## 30. Current numerical reference

~~~text
IQ sample rate:                 40 MS/s
raw ring:                       16 KiB

production control window:      4092 bytes
production control cadence:     50 ms
production observed RF/window:  ~102.3 us
production temporal coverage:   ~0.205%

Range v2 fast window:           512 bytes
Range v2 fast cadence:          6 ms
fast observed RF/window:        12.8 us
fast temporal coverage:         ~0.213%

normal Fusion decision age:     ~2 s
confirmed fast-fade age:        ~150 ms
trial settle:                   ~500 ms
trial evaluation:               ~400 ms

FUSION EXP gain floor:          G34
RANGE V2 gain floor:            G2
maximum modeled gain:           G62
~~~

The key improvement of the 6 ms observer is temporal distribution, not total sampled-time percentage.
