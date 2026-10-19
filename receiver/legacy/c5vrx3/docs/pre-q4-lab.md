# PRE-Q4 receiver lab

This lab isolates everything that can change the information quality **before**
C5VRX reduces the ESP32-C5 receive stream to the live MODEM_DIAG Q4/I4 byte.

The objective is not to make a register value larger. A pre-Q4 change is useful
only when the same RF source can tolerate more attenuation at the same raw-IQ
geometry and/or equivalent visible CVBS quality.

## Boundary

```text
antenna / board RF path
 -> C5 5 GHz frontend
 -> vendor RX gain table
 -> vendor calibration / DC / IQ state
 -> ADC + receive filter
 -> MODEM_DIAG Q4/I4       <-- PRE-Q4 boundary
 -> phase / adjacent FM
 -> Alpha or other demod
 -> CVBS
```

The production receiver must stay conservative. Undocumented RXDC, IQ-coefficient
and filter writers are not enabled by this lab merely because their ROM symbols
exist. Promote one only after its ABI/register effect and a raw-Q4 benefit are
physically proven.

## Console commands

| Key | PRE-Q4 action |
| --- | --- |
| `S` | Self-noise A/B. Measure normal live TX, remove the PARLIO TX unit, hold all six DAC GPIOs static low while RX remains the measurement source, then rebuild the exact live TX pipeline and measure again. |
| `G` | Sweep from ARC's first entry of the highest vendor RF stage (normally near G61) through the complete generated vendor-table maximum, one index at a time. This distinguishes additional RF sensitivity from downstream BB/fine amplification. |
| `U` | ARC V3 / RX AUTO LAB. Automatically separates Q4 gain placement from RF sensitivity by running reference-guarded gain scouting, top-candidate BW40/BW20 tests, carrier centering, and repeated baseline/winner proof. |
| `K` | Erase only Espressif's stored PHY calibration namespace and reboot. The running receiver is never recalibrated in place; the next Wi-Fi/PHY initialization rebuilds vendor calibration state. |
| `H` | Print the read-only ARC gain/filter/ADC/IQ oracle. |
| `W` | Existing safe BW40/BW20 A/B using the already-proven bandwidth API. |
| `A` | Existing acquisition-only carrier-offset sweep. |
| `p` | Print a machine-readable current lab row. |

All rows remain prefixed by `C5VRX_LAB_ROW`. PRE-Q4 rows add
`tx_quiet=0/1` so captures can be compared mechanically.

## 1. TX/DAC self-noise test

The XIAO simultaneously receives a weak 5.8 GHz carrier while C5VRX normally
toggles several GPIOs at the video-output cadence. This test determines whether
the receiver is being desensitized by its own digital/DAC activity.

`S` performs:

```text
MANUAL gain + BW40 + AFC off
 -> PREQ4_TX_ACTIVE
 -> disable PARLIO TX
 -> disable flight BitScrambler
 -> delete TX unit
 -> hold six physical DAC branches at static 0
 -> PREQ4_TX_QUIET
 -> recreate TX unit
 -> restart the same flight BitScrambler
 -> restart/synchronize RX/TX exactly like menu exit
 -> PREQ4_TX_RESTORED
```

The RX source itself is not intentionally gated during the quiet measurement.

A useful result is a repeatable improvement in raw-Q4 metrics or required RF
attenuation in `PREQ4_TX_QUIET`, not merely the expected loss of visible CVBS
while TX is intentionally disabled.

## First hardware evidence — 2026-09-22

PR #56 was exercised on A1 / 5865 MHz with `6BIT@40`, BW40 and the PRE-Q4
self-noise probe at multiple physical VTX distances. The probe itself froze the
receiver at G62 while each ACTIVE -> QUIET -> RESTORED sequence ran.

### Weak-signal observations

Two near-threshold runs did **not** improve when PARLIO/DAC TX was made quiet:

```text
run A
ACTIVE:   P=1  Q=45  origin=521  risk=545
QUIET:    P=1  Q=42  origin=554  risk=582
RESTORED: P=1  Q=43  origin=530  risk=568

run B
ACTIVE:   P=1  Q=40  origin=563  risk=604
QUIET:    P=1  Q=40  origin=572  risk=611
RESTORED: P=1  Q=32  origin=646  risk=703
```

A dominant TX/DAC self-noise mechanism would normally be expected to move
`Q` upward and `origin_pm` / risk downward during the QUIET interval. That
pattern was not observed.

### Medium-signal observation

One medium run changed in the opposite direction:

```text
ACTIVE:   P=17  Q=86  origin=37   syncQ=72  risk=93
QUIET:    P=16  Q=77  origin=150  syncQ=0   risk=194
RESTORED: P=20  Q=91  origin=9    syncQ=0   risk=56
```

The receiver therefore did not show a repeatable raw-Q4 improvement merely from
removing the live video-output activity. The ACTIVE -> QUIET -> RESTORED spread
is large enough that ordinary 5.8 GHz fading / multipath over the several-second
sequence is a plausible confounder.

### Strong-signal runs are not sensitivity evidence

Several close-range measurements reached approximately:

```text
P = 80..85
Q = 90..96
clip_pm = 784..959
origin_pm = 0
```

Those windows are heavily clipped at G62. They are useful for proving that the
probe can stop and restore TX without transport faults, but they must not be
used to estimate a self-noise sensitivity penalty.

### Current conclusion

The first hardware evidence provides **no reproducible evidence that the
PARLIO/resistor-DAC output is the dominant range limiter**.

In particular:

- a large multi-dB self-noise penalty is not supported by these runs;
- a small effect remains possible because the present A/B sequence is vulnerable
  to time-varying multipath and does not yet estimate an RF-equivalent dB delta;
- the result does **not** prove that board-level digital coupling is exactly zero;
- PRE-Q4 work should now prioritize the complete highest-RF-stage gain sweep,
  then vendor RXDC/IQ state, ADC/filter tuple and acquisition-only centering if
  those controls pass their individual proof gates.

### Better automatic self-noise experiment

A future automatic detector should avoid one-shot ACTIVE -> QUIET -> RESTORED
classification. It should:

1. let ARC find a non-clipping receive gain first;
2. freeze that exact valid vendor gain tuple;
3. run a short repeated `ACTIVE -> QUIET -> ACTIVE -> QUIET -> ACTIVE`
   sequence;
4. reject windows with heavy clipping or obvious physical fade;
5. compare medians / robust deltas for `Q`, `origin_pm`, IQ geometry and risk;
6. classify self-noise only when the QUIET improvement is repeatable in both
   directions;
7. never probe while clean video is in LOCK.

Until that repeated test exists, the manual `S` result is evidence against a
large self-noise problem, not a calibrated upper bound in dB.

## 2. Highest-RF-stage gain sweep

ARC reconstructed the generated vendor table rather than treating gain as one
opaque number. The last RF-code transition normally begins around G61. Values
above that point can still change BB/fine gain, so maximum numerical gain is not
automatically maximum sensitivity.

`G` therefore uses:

```text
first = rf_get_arc_survival_gain()
last  = rf_get_arc_gain_table()->max_index
step  = 1
```

Every state is applied through `phy_force_rx_gain()`; the probe does not write
a handcrafted PBUS tuple.

At a fixed near-threshold RF input compare:

- `p`, `q`, `origin_pm`, and `clip_pm`;
- DC I/Q, skew and cross-correlation;
- winding and semantic sync;
- decoded RF/BB/fine tuple;
- visible picture and maximum attenuation.

The desired FAR state is the one that lowers the equivalent-video RF threshold,
not the one with the largest Q4 magnitude.

### Hardware gain-sweep evidence — 2026-09-22

Three `G` sweeps were captured at far, medium and close physical VTX
distances. This runtime table reported:

```text
first=62
max=81
rf_stage=8
rf_code=127
```

so G62..G81 all remain inside the highest decoded RF stage and mainly change
the generated BB/fine tuple.

#### Far sweep: downstream gain cannot recreate lost RF information

At the far position the stream was already effectively collapsed at G62:

```text
G62: P=1  Q=0  origin=1000  clip=0
...
G80: P=2  Q=2  origin=850   clip=0
G81: P=2  Q=3  origin=822   clip=0
```

Increasing downstream gain changed the quantized occupancy slightly but did not
recover coherent phase or sync. This is evidence that BB/fine gain cannot
replace frontend SNR once the signal has already fallen below the useful Q4
boundary.

#### Medium sweep: measured response is strongly non-monotonic

The medium-position sweep produced:

```text
G62: P=17 Q=100 origin=0   clip=0
G63: P=1  Q=0   origin=937 clip=0
G64: P=17 Q=95  origin=11  clip=0
G65: P=32 Q=100 origin=0   clip=0
G66: P=53 Q=99  origin=0   clip=257
G67: P=53 Q=100 origin=0   clip=219
G68: P=29 Q=99  origin=0   clip=0
G69: P=50 Q=99  origin=0   clip=131
```

The same decoded RF stage remained selected throughout. The corresponding
generated tuples stepped through BB/fine states such as:

```text
G62 -> bb=1  fine=5
G63 -> bb=1  fine=4
G64 -> bb=1  fine=3
G65 -> bb=1  fine=2
G66 -> bb=1  fine=1
G67 -> bb=1  fine=0
G68 -> bb=3  fine=5
```

The raw-Q4 response therefore must not be assumed to increase smoothly with the
numeric gain index. G63 in this medium sweep was a particularly severe valley,
while G64/G65 immediately recovered useful phase geometry. G68 also produced a
well-filled, non-clipping Q4 state.

This is a hardware observation, not yet proof that G63 is intrinsically bad:
the sweep takes several seconds and 5.8 GHz multipath can vary over time.
Repeatability at a fixed attenuated RF source is required before permanently
blacklisting any index.

#### Close sweep: high states are overload territory

At the close position G62 was already heavily clipped:

```text
G62: P=73 Q=99 clip=627
G63: P=58 Q=100 clip=286
...
G81: P=98 Q=87 clip=957
```

This confirms that simply forcing a higher table index is not a general range
solution. The best state must depend on raw-Q4 occupancy and clipping.

#### ARC implication

The current controller uses numeric `gain + 1` / `gain - 1` steps, while
persistent no-sync returns to `survival_gain`, which is G62 on this runtime
table. That policy was intentionally conservative, but the medium sweep shows
why it can miss a useful downstream operating point:

```text
G62 usable but weak
 -> numeric +1
G63 may look catastrophically worse
 -> sync / Q4 confidence disappears
 -> no-sync path returns to G62
 -> G64/G65 are never explored
```

Do **not** change production ARC to simply allow G62..G81 unconditionally.
The next controller experiment should instead treat highest-RF-stage entries as
a set of candidate BB/fine operating points during ACQUIRE:

1. keep the RF stage fixed at the highest valid stage;
2. probe a bounded subset of valid generated indices;
3. score each state using Q4 fill, phase coherence, origin occupancy, clipping,
   IQ geometry and semantic sync;
4. reject heavily clipped states and states whose apparent benefit is not
   repeatable;
5. choose/freeze the best non-clipping candidate;
6. preserve the clean-LOCK zero-write invariant;
7. never infer RF sensitivity from Q4 amplitude alone.

A repeated controlled-attenuator sweep is the promotion gate for any permanent
candidate map or skip list.

## 3. ARC V3 / RX AUTO LAB (`U`)

`U` is the integrated receiver-autotune experiment. It is intentionally a
console-only lab engine first; it does not replace production ARC until the
hardware results prove the search policy.

The experiment answers one question:

> Is useful RF information still present but badly placed in Q4/I4, or has the
> receiver reached a real PRE-Q4 sensitivity limit?

### Search hierarchy

```text
baseline: MANUAL + BW40 + offset 0 + first highest-RF-stage index
  -> GAIN SCOUT
  -> top 3 stable gain candidates
  -> BW40/BW20 SCOUT
  -> best gain + bandwidth
  -> CENTER SCOUT coarse (-1000..+1000 kHz)
  -> CENTER SCOUT fine (around the coarse winner)
  -> 5x BASELINE -> WINNER -> BASELINE proof
  -> ACQUIRED / OVERLOAD / RF_LIMIT / UNSTABLE / INCONCLUSIVE
```

The gain scout does not trust a single sequential sweep. Every candidate is
measured between repeated G62-like reference measurements:

```text
REF -> candidate -> REF
```

A candidate is not promoted when the two reference observations drift beyond
the bounded Q/P/origin/clipping tolerances. This makes ordinary 5.8 GHz fading
visible instead of silently turning it into a fake gain-table conclusion.

At a normal/weak input the scout covers every valid index from
`survival_gain` through `table->max_index`. If the initial reference is
already overloaded, `U` switches to a bounded downward coarse search and then
refines around the best lower-gain result instead of making clipping worse.

### Candidate classification

The selection rules deliberately avoid a weighted "bigger P is better" score.

Hard rejection comes first:

- any transport fault;
- more than 30 permille Q4 rail clipping.

A `SWEET` candidate then requires:

- `Q >= 65`;
- `origin <= 250 pm`;
- `P = 14..34`;
- bounded I/Q skew and cross terms.

`USABLE` permits a wider Q4 window, while weak/near-origin states remain
`POOR`. Within the same class, selection is lexicographic: lower clipping,
higher phase coherence, lower origin occupancy, P closer to the target center,
better IQ geometry, lower winding, then semantic sync.

This means a saturated `P=80` state cannot beat a clean `P=24` state merely
because its amplitude is larger.

### RF-limit classification

A state is explicit RF-limit evidence when it has approximately:

```text
clip <= 8 pm
P <= 4
Q < 15
origin >= 800 pm
```

If at least 75% of the stable highest-RF-stage gain observations meet that
condition and the later BW/centering stages cannot produce a repeatably usable
winner, `U` reports `RF_LIMIT`.

That is the signal to **stop gain hunting**. The next PRE-Q4 work is then fresh
vendor calibration followed by individually gated RXDC/IQ and ADC/filter
experiments, not another downstream-gain increase.

### Repeated proof and freeze

A frontend winner is accepted only when at least four of five rounds have
stable baseline references and the winner beats both surrounding baseline
observations:

```text
BASELINE -> WINNER -> BASELINE
```

On `ACQUIRED`, the winning gain/BW/offset tuple is left live in:

```text
AGC = MANUAL
AFC = HOLD
BW  = fixed winner
```

No setting is persisted. A reboot or normal profile/configuration action can
return to the ordinary receiver.

The final line is machine-readable:

```text
C5VRX_RX_AUTO_RESULT status=ACQUIRED gain=... bw=... offset_khz=...
                         proof_wins=... proof_stable=... frozen=1
```

If the winner is not proven, the pre-`U` receiver state is restored.

### First ARC V3 hardware evidence — far / medium / close (2026-09-22)

Three complete `U` runs were captured without changing the firmware: one far,
one medium and one close. Together they show that the optimum generated vendor
gain state moves by **tens of indices** with RF input and that the old G62
"survival" assumption is not valid as a universal operating point.

#### Far: G62 is quantizer-starved, high generated gain restores coherence

The far run started with a stable dead G62 reference:

```text
G62: P=1  Q=0   origin=1000  clip=0  -> POOR
```

The reference remained stable while the candidate states improved progressively:

```text
G74: P=5   Q=13  origin=435  clip=0  -> POOR
G75: P=5   Q=41  origin=213  clip=0  -> POOR
G76: P=9   Q=74  origin=62   clip=0  -> USABLE
G77: P=10  Q=87  origin=20   clip=0  -> USABLE
G78: P=17  Q=99  origin=0    clip=0  -> SWEET
G79: P=17  Q=99  origin=0    clip=0  -> SWEET
G80: P=18  Q=99  origin=0    clip=0  -> SWEET
G81: P=29  Q=99  origin=0    clip=0  -> SWEET
```

This is direct evidence that useful RF information still existed upstream while
G62 was collapsing almost entirely into the Q4 origin. Raising the generated
gain within the receive table recovered a well-filled, coherent raw-Q4 vector.
The previous conclusion from the earlier one-pass `G` sweep ("higher BB/fine
gain cannot recover the far state") is therefore **not generally valid**; the
reference-guarded `U` run is stronger evidence.

The first `U` scorer selected G81 because its P landed closest to the nominal
P target, but the 5x proof exposed reduced headroom:

```text
winner G81 proof:
  rounds 1-2: clipping / REJECT
  rounds 3-5: SWEET
result: INCONCLUSIVE, proof_wins=3/5, frozen=0
```

The hardware implication is to prefer the **lowest generated gain that is
already robustly SWEET**, rather than maximizing P or choosing the numerically
largest clean-looking state. In this run G78 was the first clearly SWEET state
and retained more headroom than G81.

#### Medium: optimum moves down to roughly G56-G57

At the medium position the initial G62 reference was already heavily clipped,
so `U` entered overload descent. The useful region moved far lower:

```text
G62: heavy clipping / REJECT
G58: clean but high, USABLE/SWEET depending window
G57: P=17  Q=99  origin=0  clip=0  -> SWEET
G56: P=13  Q=99  origin=0  clip=0  -> USABLE
G54: P=17  Q=99  origin=0  clip=0  -> SWEET in one coarse window
G50: Q4 starts becoming under-filled
```

The refinement around the transition was especially informative:

```text
G55: P=13 Q=99 clip=0  -> USABLE
G56: P=13 Q=99 clip=0  -> USABLE
G57: P=17 Q=99 clip=0  -> SWEET
G58: P=36 Q=100 clip=0 -> USABLE
G59: P=52 Q=100 clip=228 pm -> REJECT
G60: P=58 Q=99  clip=281 pm -> REJECT
G61: P=53 Q=100 clip=298 pm -> REJECT
```

This run exposed a flaw in the first `U` proof method: overload mode continued
to use G62 as the fading reference. Because G62 itself was clipping by hundreds
of permille, the reference wandered enough to mark otherwise clean candidates
unstable. A future overload search must first find a **clean safe anchor** and
then use that anchor for REF -> candidate -> REF checks.

#### Close: optimum moves down again to roughly G46-G47

At close range the initial high-stage reference was even more overloaded:

```text
G62: REJECT, heavy clipping
G58: REJECT, heavy clipping
G54: REJECT, heavy clipping
G50: usable transition region
G46-G47: clean usable region
G42 and below: under-filled / POOR
```

The refinement shows a sharp upper edge:

```text
G47: P=45 Q=100 clip=15 pm  -> USABLE
G48: P=61 Q=99  clip=347 pm -> REJECT
G49: P=65 Q=99  clip=435 pm -> REJECT
G50: P=65 Q=99  clip=500 pm -> REJECT
```

The run later ended at G46 with approximately `P=17 Q=99`, confirming that the
clean operating region had moved well below both the medium and far settings.

#### Cross-distance result

The observed useful regions were approximately:

```text
close   -> G46-G47
medium  -> G56-G57
far     -> G78-G80
```

These are not production constants and must not be hardcoded. They demonstrate
the topology: the generated vendor state must move strongly with received
signal level to keep the raw 4-bit IQ representation inside its useful window.

The same G62 state can therefore be catastrophically wrong in **both**
directions:

```text
far:   G62 -> P~1, Q~0, origin~100%    (quantizer-starved)
close: G62 -> very high P, heavy clip  (overloaded)
```

This is the strongest hardware evidence so far that the main range problem is
substantially influenced by **pre-Q4 gain placement**, not only by the
post-Q4 demodulator.

#### BW / carrier-search caution exposed by the same runs

The first `U` implementation always continued into BW and carrier-centering
search after finding a good gain state. Hardware data shows that this can
over-fit time-varying RF conditions.

Examples:

- medium: both BW40 and BW20 produced SWEET states around G56-G58;
- far: one G78 BW20 window improved from a poor BW40 window to SWEET, but other
  candidates did not show the same deterministic relationship;
- close: multiple offsets from roughly -750 to +750 kHz produced SWEET windows,
  while the same nominal offset could later degrade badly during proof.

Therefore the production-oriented search order should be conservative:

```text
GAIN FIRST
  -> if a robust SWEET state exists: HOLD / LOCK
  -> only if gain alone cannot recover a usable state:
       try BW
       then carrier centering
  -> if all fail: RF_LIMIT
```

A SWEET-to-SWEET actuator change is not, by itself, evidence that the new
setting is better. BW or carrier offset should only move when the improvement
is material and repeatable.

#### Controller implication

The hardware evidence now supports a Q4-target controller with three primary
states:

```text
STARVED  -> raise effective generated gain
SWEET    -> hold / zero PHY writes
OVERLOAD -> lower effective generated gain
```

Selection should stop at the **lowest sufficient SWEET state with margin**,
rather than targeting maximum P. A clipped reference must never be used as the
stability oracle; overload recovery must establish a clean anchor first.

The next ARC V3 iteration should implement these policy changes in the lab
engine before any production promotion.

### ARC V3 gain-first live experiment

The far/medium/close `U` runs identify a concrete production-ARC failure:

```text
far G62:
P~1, Q~0, origin~100%
        |
        +-- current ARC requires useful phase/sync evidence before gain-up
        +-- persistent no-sync explicitly returns to survival_gain (G62)
        |
        +--> controller can remain permanently quantizer-starved
```

This is a control-loop trap, not evidence that the RF carrier is absent. In the
far hardware run, raising generated vendor gain converted the same class of
weak input into coherent Q4. Medium and close runs show the inverse problem:
G62 can also be far too hot.

PR ARC V3 therefore adds a separate `ARC V3 EXP` live profile. It does not
replace production ARC yet.

The experiment deliberately freezes every other frontend actuator:

```text
BW = BW40
offset = 0 kHz
FFT = normal
demod = independently selectable
```

Only a valid vendor-generated gain-table index may move.

The controller is raw-Q4-first and does not require semantic video sync to
escape starvation:

```text
STARVED
  hard: P<=4, Q<15, origin>=800 -> +4 indices
  ordinary below-target         -> +1 index

TARGET
  clip<=16 pm
  P=8..34
  Q>=55
  origin<=350 pm
  winding<300 pm
  -> after persistence: LOCK

HIGH / OVERLOAD
  above target / clipping -> -1
  severe clip/P           -> immediate -4

table max + persistent STARVED
  -> RF_LIMIT
```

The target is intentionally headroom-biased. The controller stops increasing
gain as soon as raw Q4 contains enough coherent phase information; it does not
optimize toward P=24 and it does not continue into BW/AFC search.

LOCK uses a slightly wider hold window and produces zero PHY writes while the
raw Q4 vector remains useful. This prevents normal video modulation or sparse
semantic-sync windows from causing gain hunting.

The old `ARC` profile remains available unchanged for direct A/B. Select the
new test profile through the normal profile cycle or serial key `Y`.

Expected first live test:

```text
far:
  old ARC -> remains near G62 / Q4-starved
  ARC V3 -> climbs through vendor states until Q4 enters TARGET

medium:
  ARC V3 -> descends from overloaded G62 and freezes near the first clean state

close:
  ARC V3 -> rapidly cuts gain until clipping disappears
```

Promotion criterion is not a specific G number. The test succeeds if the live
controller follows the raw-Q4 state across distance, preserves clean LOCK, and
extends matched-quality range without persistent oscillation.

### Live ARC V3 walk evidence and temporal fix

The first live `ARC V3 EXP` walk tests confirmed the gain direction over almost
the full vendor table, but also exposed excessive reaction to individual
50 ms control windows.

Observed useful regions were approximately:

```text
~1 cm / ultra-close -> G12-G18
close               -> G40-G46
medium              -> G60-G70
far                 -> G77-G81
```

These values are **observations, not a hardcoded distance table**. They show
that the correct generated vendor state can move by more than 60 indices across
the usable RF dynamic range.

The strongest moving test started far around G79-G81 and then walked back
toward the VTX. The overall trajectory correctly fell toward lower gain:

```text
~G80 -> G69 -> G66 -> G55 -> G40 -> G36
```

but a short fade produced an incorrect reversal:

```text
G69 -> G66 -> G81 -> G81 -> G67 -> G55
```

Static traces showed the same state could vary strongly between completed
control windows while the carrier remained usable. That proves the first live
ARC V3 implementation was directionally correct but temporally under-filtered.

The controller now uses a five-window component-wise median before ordinary
gain decisions. Gain-up is deliberately slower than gain-down:

```text
ordinary STARVED -> 5 filtered confirmations
hard STARVED     -> 3 filtered confirmations
HIGH             -> 3 filtered confirmations
OVERLOAD         -> 2 filtered confirmations
severe raw clip  -> 2 consecutive raw windows, then emergency -4
```

A downward gain move also installs a one-second no-up reversal guard. This is
specifically intended to prevent a short multipath fade from turning a valid
walk-back trajectory such as `G69 -> G66` into `G81`. Persistent real
starvation remains able to reverse direction once the guard expires.

LOCK now tolerates a broader filtered target region and requires six persistent
bad filtered observations before leaving the zero-write state.

### Post-fix hardware validation and calibration anchors

The temporal ARC V3 fix was re-tested with repeated moving hardware walks.
The strongest validation was a **close -> far -> close** trajectory. The
observed gain sequence followed the physical RF trend in both directions:

```text
close -> far:
G16 -> G54 -> G74/G73 -> G78 -> G79 -> G81

far -> close:
G81 -> G77 -> G69 -> G56 -> G45 -> G42 -> G39
```

The previously observed large short-fade reversal (`G66 -> G81` while
walking toward the VTX) was not reproduced in this run. Long plateaus at a
useful gain state are now common, which is consistent with the five-window
median, asymmetric persistence and reversal guard doing their intended job.

A separate medium -> extra-close walk similarly descended through:

```text
G62/G68 -> G54 -> G50 -> G46 -> G38 -> G35 -> G31 -> G14/G17
```

At the far end of the close/far/close test the raw-Q4 vector was still often
healthy even at the vendor ceiling, for example:

```text
G78 P10 Q88
G79 P17 Q99
G81 P25 Q99
G81 P17 Q99
```

The operator also reported that the point previously treated as "far" now
produced good video. This is strong practical evidence that earlier usable
range was being limited substantially by gain placement before Q4. It is **not
yet a calibrated sensitivity result**: no RF input power or step attenuation
was measured.

Across all current hardware walks, the empirical operating regions are now:

```text
very strong / ~1 cm   -> G14-G18
strong / close        -> roughly G35-G46
medium-close          -> roughly G50-G56
medium / weak         -> roughly G60-G74
very weak / far       -> roughly G77-G81
```

These ranges are **calibration anchors, not a distance table**. Indoor
multipath, antenna orientation and VTX power can move the optimum state. The
useful conclusion is that the correct vendor state spans almost the entire
generated table and moves monotonically enough to support a calibrated search
policy.

A future C5VRX gain calibration layer should therefore map **raw-Q4 condition
to search anchors**, not meters to gain. A first coarse ladder supported by
hardware evidence is:

```text
G16 -> G40 -> G54 -> G70 -> G78 -> G81
```

Example policy:

```text
hard-starved at G54 -> jump toward G70
still starved       -> try G78
TARGET               -> refine locally / LOCK

overloaded at G78   -> jump toward G70
still high          -> try G54
TARGET               -> refine locally / LOCK
```

The existing ARC V3 controller intentionally remains more conservative than
this proposed calibration search. The next calibration step should collect
per-state P/Q/origin/clip/winding statistics and, ideally, repeat them against
known RF attenuation. That would turn the empirical ladder into a reproducible
gain-transition table without assuming that vendor gain indices are linear dB.

The new working model for the original range problem is therefore:

```text
weak RF
  -> insufficient pre-Q4 generated gain
  -> Q4/I4 vector collapses around the origin
  -> phase information is quantized away
  -> FM/CVBS quality collapses early

ARC V3:
weak RF
  -> raise vendor gain until Q4 is usefully occupied
  -> hold with temporal hysteresis
  -> preserve phase information for the demodulator
```

Once ARC V3 reaches G81 and sustained Q4 coherence still collapses, that point
is much closer to the **real receiver sensitivity boundary**. Beyond that
point, further improvement must come from the RF/ADC/filter chain or from
making better use of the remaining weak-signal phase information downstream;
digital amplitude scaling after Q4 cannot reconstruct phase that was already
lost in quantization.

### 25 mW relative-power walk

A second close -> far -> close hardware walk was captured with the VTX reduced
from **200 mW to 25 mW**. That is an 8x power reduction:

```text
10 * log10(25 / 200) = -9.03 dB
```

The exact raw `[CARRIER]` capture is preserved in
`docs/live_walkaround_log.txt`. This capture contains P/Q/G only; it does not
contain origin/clip/winding, so those metrics must not be reconstructed or
assumed.

Observed gain trajectory:

```text
close -> far:
G29 -> G37 -> G58/G64/G60/G57 -> G81

far plateau:
G81 for 24 consecutive logged carrier samples

far -> close:
G81 -> G75 -> G67 -> G51 -> G40 -> G28
```

Even on the G81 far plateau, raw phase coherence was often still strong:

```text
G81 P13 Q93
G81 P26 Q99
G81 P17 Q98
G81 P16 Q94
G81 P26 Q100
G81 P25 Q99
```

Lower-Q windows also occurred at the same ceiling state (roughly Q60-Q77), so
the far point is now clearly **gain-ceiling limited**, but it is not yet a
clean, sustained raw-Q4 information collapse.

This 25 mW run is the first hardware walk with a known relative transmit-power
change. Relative to the earlier 200 mW strong/extra-close observations around
G14-G18, the 25 mW close end around G28-G29 is consistent with significantly
more generated pre-Q4 gain being required after a -9.03 dB RF reduction.

That comparison is useful calibration evidence, but it is **not yet a global
gain-index-to-dB conversion**. The two walks were not performed with a
calibrated attenuator at a fixed geometry, and vendor gain indices cross
different RF/BB/fine states. Multipath and antenna orientation can also shift
the optimum state substantially.

The correct next calibration experiment is therefore a fixed-geometry power
ladder, for example:

```text
200 mW   0.00 dB relative
100 mW  -3.01 dB
 50 mW  -6.02 dB
 25 mW  -9.03 dB
```

At each step, record the settled gain together with P/Q/origin/clip/winding.
That produces a repeatable **relative RF power -> required generated gain**
curve without pretending that distance itself is a receiver observable.

### Demodulator boundary

`U` does not mix frontend discovery with demodulator selection. Q4 placement is
solved first while the currently selected demod remains constant. After
`ACQUIRED`, the console prints a follow-up marker instructing the hardware A/B
to keep the frozen RF tuple unchanged while comparing Golden / Exact Adjacent /
Alpha on the demod branch.

Fresh full PHY calibration also stays a separate reboot A/B: run `U`, run
`K`, then run the same `U` setup again.

## 4. Fresh vendor PHY calibration

`K` calls the public ESP-IDF
`esp_phy_erase_cal_data_in_nvs()` API and then reboots. It does **not** run an
invasive calibration while live analog video owns the receiver.

After the reboot, capture `H` and `p` again at the same physical RF setup.
Compare the vendor IQ coefficients, ADC/filter state, Q4 geometry, and
attenuation threshold against the previous boot.

Do not conclude that calibration helped merely because coefficient values
changed.

## 5. Still gated

The ROM contains receive-side functions related to RXDC, IQ correction, ADC,
filters and gain. This PR deliberately does not promote these writers:

```text
phy_pbus_rx_dco_cal(...)
phy_dc_iq_est_new(...)
phy_set_cal_rxdc(...)
phy_rxiq_set_reg(...)
phy_chan_filt_set(...)
phy_rx_filter_mode(...)
phy_pbus_set_rxgain(...)
```

The next promotion gate for any one of them is:

1. recover the exact ESP32-C5 ABI or exact MMIO effect;
2. run it only in an explicit bounded lab state;
3. prove a change upstream of MODEM_DIAG using raw Q4/I4;
4. prove lower required RF input at matched output quality;
5. prove no periodic calibration or packet-state hunting is required;
6. keep it out of clean LOCK.

## Recommended test order

Use a fixed VTX/channel/antenna/scene and preferably a step attenuator.

```text
1. H + p baseline
2. S self-noise A/B
3. G highest-stage sweep near threshold
4. K fresh calibration, then repeat H/p/S/G
5. W bandwidth A/B
6. A carrier-centering A/B
```

Walk tests are useful later, but they are not suitable for assigning dB gains
because multipath and antenna orientation move between trials.

## Success criterion

A PRE-Q4 change becomes a range fix only when it moves the matched-quality
threshold. Example:

```text
baseline usable picture:  -86 dBm
candidate usable picture: -90 dBm

measured improvement:       4 dB
```

Raw amplitude alone is not a sensitivity measurement.
