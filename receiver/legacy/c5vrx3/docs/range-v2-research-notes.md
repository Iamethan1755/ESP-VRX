# C5VRX Range v2 — RF / Demod Research Notes

This file contains the remaining research knowledge and test discipline around Range v2. It intentionally separates hypotheses from proven project behavior.

Legend:
- PROVEN — architectural fact or measured project evidence.
- IMPLEMENTED — available as an experiment in Range v2.
- HYPOTHESIS — useful direction that still needs controlled hardware evidence.

---

## 1. Vendor AGC as an oracle, not an owner

IMPLEMENTED lab method

Range v2 can temporarily:
1. hand gain ownership to vendor AGC;
2. let it settle;
3. capture RX/filter/ADC/source register state;
4. restore deterministic C5VRX control.

Questions this can answer:
- which gain indices correspond to hardware stage changes?
- does vendor AGC choose states our controller avoids?
- which registers move together?
- are useful pre-Q4 controls hidden behind the vendor chain?

Do not leave vendor AGC continuously fighting Fusion.

---

## 2. Undocumented ESP32-C5 RX controls

PROVEN existence; semantics unproven

ESP32-C5 ROM exposes symbols including:
- phy_pbus_set_rxgain;
- phy_bb_gain_index;
- phy_gen_rx_gain_table;
- phy_rx_sense_set;
- phy_agc_max_gain_set;
- phy_wifi_agc_sat_gain;
- phy_read_hw_noisefloor;
- phy_wifi_fbw_sel.

A symbol name is not an ABI contract.

Required process:
1. identify ABI and call semantics;
2. run a bounded lab test;
3. verify raw Q4 and phase effect;
4. check transients and stability;
5. only promote controls with reproducible benefit.

The key question is whether a control improves information before Q4 quantization.

---

## 3. FFT scaling

HYPOTHESIS until measured per hardware path

Forced FFT or digital scaling may change Q4 amplitude or downstream representation.

It does not automatically mean better RF sensitivity.

Correct experiment:

~~~text
same RF input
same analog gain
same bandwidth
change FFT scale
measure raw Q4:
  origin
  clipping
  Q_phase
  winding
  sync
~~~

If only numeric amplitude changes without phase-information improvement, it is not a sensitivity gain.

---

## 4. BW20 versus BW40

Pure noise theory says halving equivalent noise bandwidth reduces integrated thermal noise by about 3 dB.

Analog FPV reality is more complicated because the desired signal itself is wideband FM.

A narrower front end can also remove useful modulation sidebands and cause:
- detail loss;
- chroma problems;
- sync distortion;
- worse recovered video.

Therefore:

Do not claim "BW20 = +3 dB range".

IMPLEMENTED experiment: Range v2 can use acquisition-only bandwidth adaptation, but a valid BW20 win requires better phase-slip/winding, sync survival and usable picture under controlled attenuation.

A nicer RSSI or noise number alone does not count.

---

## 5. AFC and carrier centering

A slightly off-center desired carrier can look like poor phase quality, but continuous retuning can create disturbances.

IMPLEMENTED:
- bounded carrier-centering sweep;
- approximately -1000 to +1000 kHz;
- scored on RF and video quality;
- previous state restored afterwards.

Production philosophy:
- center during acquisition or loss;
- freeze while clean and locked;
- avoid constant frequency hunting during good video.

---

## 6. WBFM phase slope is not naive CFO

PROVEN project caution

Instantaneous WBFM phase slope contains the video modulation itself.

A short-window average can be useful diagnostic information, but it is not automatically a calibrated carrier-frequency error estimator.

An AFC estimator must distinguish:

~~~text
carrier offset
~~~

from:

~~~text
legitimate FM video modulation
~~~

before it is trusted for automatic retuning.

---

## 7. Antenna and feedline remain first-order variables

PROVEN RF principle

Software cannot recover signal that never reaches the receiver.

Check:
- correct 5.8 GHz antenna;
- polarization match;
- connector and pigtail loss;
- damaged antenna;
- board placement and coupling;
- local digital noise sources.

A software improvement and a bad antenna can easily mask each other.

---

## 8. External LNA

HYPOTHESIS / hardware experiment

A low-noise preamp can improve system noise figure if:
- its own noise figure is good;
- loss before it is small;
- the C5 front end is actually noise-limited.

It can make performance worse if:
- blockers cause compression;
- internal gain is already sufficient;
- added gain drives Q4 into clipping;
- matching is poor;
- the LNA oscillates.

Required A/B:

~~~text
same antenna + controlled attenuation
LNA bypass
LNA enabled
measure weak threshold
measure strong/blocker headroom
~~~

Do not judge only by RSSI.

---

## 9. Preselection and filtering

HYPOTHESIS

A better RF filter can help in blocker-heavy environments.

Tradeoff:

~~~text
better rejection
vs
insertion loss before LNA
~~~

A filter that helps beside a strong blocker may hurt quiet-environment sensitivity.

Treat it as scenario-dependent, not as a universal range upgrade.

---

## 10. Diversity

### Selection diversity

HYPOTHESIS, realistic first step

Two receivers or antennas can independently score:
- phase risk;
- sync;
- origin;
- winding;
- quality.

Then choose the better receiver with hysteresis.

This directly attacks spatial multipath and antenna orientation.

### Coherent combining / MRC

HYPOTHESIS, substantially harder

True coherent combining requires alignment of:
- carrier phase;
- frequency offset;
- sample timing;
- possibly independent clocks.

Do not describe simple receiver selection as MRC.

The first useful dual-C5 design should probably be selection diversity.

---

## 11. Spatial diversity versus software Fusion

Current multi-estimator Fusion improves decision quality on one stream.

Physical antenna diversity attacks a different failure mode: one antenna can sit in a multipath null while another does not.

They are complementary:

~~~text
estimator fusion != spatial diversity
~~~

---

## 12. Range metric hierarchy

### Level 1 — transport sanity
- no GDMA faults;
- no PARLIO overflow or empty;
- no BitScrambler transport failure.

### Level 2 — raw IQ information
- origin rate;
- clipping rate;
- I/Q balance;
- Q_phase;
- phase consistency.

### Level 3 — demod integrity
- endpoint versus adjacent winding disagreement;
- hard phase-error rate;
- p95/p99 error;
- burst length.

### Level 4 — semantic video
- H-sync quality;
- V-sync stability;
- line-period stability;
- decoder relocks.

### Level 5 — actual usable range
- controlled attenuation threshold;
- same VTX, antenna and channel;
- repeatable picture usability.

Never jump from "RSSI increased" directly to "range improved".

---

## 13. Burst errors can matter more than mean error

A decoder can tolerate low-level random snow but react badly to a short error cluster that resembles or destroys sync.

Two demodulators with the same mean error can therefore behave very differently.

Future benchmarks should track:
- maximum hard-error run;
- p95/p99 run length;
- errors near predicted H-sync;
- display relock events.

This is why catastrophic tail behavior deserves its own objective.

---

## 14. A/B discipline

For a meaningful comparison:
1. same VTX;
2. same channel;
3. same antenna orientation;
4. same receiver placement;
5. same output and display;
6. controlled attenuation if possible;
7. repeated runs.

Prefer:

~~~text
baseline
feature ON
baseline again
~~~

over one sequential pass while RF conditions drift.

---

## 15. Strong-signal testing is mandatory

Every weak-signal feature must also survive:
- VTX very close;
- high RF power;
- adjacent-channel VTX;
- blocker or interference.

A feature that gains at the edge but destroys video near the pilot is not a good receiver mode.

This is why Range v2 explicitly includes near-field gain headroom.

---

## 16. Channel-specific learning

HYPOTHESIS / future

RX behavior can vary by frequency because of:
- antenna response;
- board matching;
- internal filter behavior;
- interference environment.

Future persistent learning should either:
- condition on channel or band;
- or discount learned state when the channel changes.

Do not automatically reuse one channel's gain model as universal truth.

---

## 17. Persisted learning versus per-flight learning

HYPOTHESIS

Persistent learning could speed future acquisition, but can also fossilize stale models.

Persist only stable hardware-like facts, for example:
- characterized stage boundaries;
- channel-specific safe priors.

Do not persist short-term multipath observations as if they were hardware truth.

---

## 18. Why a large ML model is not automatically smarter

The problem has:
- a small action space;
- hard safety constraints;
- expensive physical actions;
- rapidly changing environment;
- limited labels.

A large neural model is not the obvious solution.

Better first tools:
- local response learning;
- temporal EWMAs;
- change detection;
- risk constraints;
- structured priors;
- bounded contextual state machines or bandits.

The useful intelligence is in RF structure, not model size.

---

## 19. PID analogy: useful and misleading

Useful analogy:
- observe error and trend;
- avoid oscillation;
- use damping and hysteresis;
- different attack and recovery behavior.

Misleading part:
- gain states are discrete;
- response is nonlinear;
- stage boundaries may exist.

A conventional continuous PID directly commanding gain can hunt badly.

Range v2 instead uses state, trends, local learned transitions, cooldown and bounded moves.

---

## 20. High-value research directions

These are priorities to test, not guaranteed performance rankings:

1. remove demod phase loss that occurs before the true RF sensitivity limit;
2. identify the real pre-Q4 RX gain chain and its best weak-signal operating region;
3. prevent control writes from causing visible relocks;
4. optimize antenna and front-end noise figure;
5. add spatial diversity where multipath dominates.

Tweaking a downstream numeric score is low value if the Q4 information is already gone.

---

## 21. What not to claim yet

Do not state as project facts without hardware evidence:
- Range v2 gives X dB more range;
- BW20 adds 3 dB;
- PLL beats Phase5 on C5VRX;
- an external LNA is always better;
- C5VRX is more sensitive than RX5808;
- vendor gain 62 is the maximum-SNR state;
- FFT scale is extra RF gain;
- Fusion combines multiple RF receivers;
- all ESP32-C5 gain knobs are understood.

These are testable questions, not conclusions.

---

## 22. Issue map

### Issue #23 — exact adjacent FM / endpoint winding

Questions:
- can exact adjacent FM materially reduce weak-signal hard errors?
- can confidence repair help without smoothing real CVBS?
- can the C5 sustain the realtime path?

### Issue #27 — RX gain-chain characterization

Questions:
- what does each gain index really change?
- which controls are pre-Q4?
- where are internal stage boundaries?
- can vendor AGC reveal better states?

### Issue #28 — intermittent lag and sync corruption

Questions:
- are visible pauses real transport stalls?
- do gain, BW or AFC writes corrupt sync?
- does display relock amplify very short glitches?

Coupling:

~~~text
#27 determines the best raw IQ
#23 determines how much video can be recovered from it
#28 determines whether adaptation itself damages the result
~~~

---

## 23. Suggested next hardware sequence

### Test A — baseline attenuation curve

Record with fixed FUSION settings:
- attenuation or distance;
- Q4 origin;
- clipping;
- Q_phase;
- winding;
- sync;
- visible usability.

### Test B — RANGE V2 controller

Repeat and log:
- chosen gain;
- fade, recovery and stability;
- number of PHY writes;
- near-field behavior;
- edge-of-range behavior.

### Test C — carrier centering

At a weak but repeatable RF level:
- run bounded AFC sweep;
- compare phase, winding and sync by offset.

### Test D — vendor AGC oracle

At multiple controlled RF levels:
- capture vendor-selected registers;
- compare against fixed-gain states.

### Test E — raw Q4 demod benchmark

Capture strong, medium and near-threshold raw Q4 and compare:
- endpoint Phase5;
- exact adjacent;
- confidence repair;
- PLL.

Only after Test E shows a real advantage should a new live demod architecture be promoted.

### Test F — front-end hardware

After software limits are understood:
- antenna A/B;
- optional LNA bypass/enabled;
- blocker test;
- optional diversity.

---

## 24. Design target

The desired behavior is not "maximum gain".

It is:

~~~text
use the minimum number of physical changes necessary
to keep the desired carrier above the demod/video failure threshold
without clipping, blocker collapse or decoder relock.
~~~

A good controller should feel boring:
- clean video: no writes;
- gradual fade: one predictive move;
- sudden overload: fast bounded cut;
- recovery: slow and stable return;
- lost carrier: recover sensitivity without random gain walking;
- close VTX: enough headroom to avoid distortion.

That is the Range v2 design philosophy.
