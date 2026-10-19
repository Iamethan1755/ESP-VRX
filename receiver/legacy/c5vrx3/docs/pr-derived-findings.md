# C5VRX PR-derived engineering findings

This document preserves the engineering knowledge accumulated in C5VRX pull
requests through PR #71 plus high-signal non-PR research branches reviewed on
2026-09-24. It exists so useful findings do not disappear when an experimental
PR or branch is closed, superseded, or never merged.

The repository state on main remains authoritative for production behavior.
An open PR can prove mathematics, expose a hardware constraint, or record a
useful failure without being production-ready.

## Evidence vocabulary

- **PRODUCTION / MAIN** — present in current main and part of the supported
  receiver architecture.
- **HARDWARE-PROVEN** — directly observed on ESP32-C5 hardware in the project.
- **HOST-PROVEN** — exhaustive oracle, model, build, or simulation evidence,
  but not yet a live receiver result.
- **EXPERIMENTAL** — useful implementation or hypothesis with hardware gates
  still open.
- **MEASURED NEGATIVE** — a hardware result that rules out or strongly
  constrains a route.
- **SUPERSEDED** — retained as history, but a later result corrected or
  replaced the conclusion.

## 1. Current main contract

At main commit 96446ed (merge of PR #62), the important production defaults are:

- raw MODEM_DIAG packed Q4/I4 at 40 MS/s;
- PARLIO RX into the continuous 32 KiB raw-IQ ring;
- Zero-EOF circular GDMA;
- full-Q4/I4 Golden Phase5 as the default demodulator;
- 20 MS/s unique CVBS values transported as duplicated [D,D] bytes at a
  40 MHz / 40 MB/s physical DAC transport;
- six-bit resistor DAC output;
- BW40 as the normal RF bandwidth contract;
- RX_PROFILE_ARC as the default receiver profile;
- ARC V3 and ARC V5 are available research/control paths in main, but PR #63,
  which would have promoted ARC V5 into the simplified production menu/default
  contract, was closed without merge.

This distinction is important: merged code and a PR's intended final product
state are not the same thing.

## 2. Hard realtime and peripheral constraints

### Keep the 40 MS/s dataplane hardware paced

The successful architecture is a hardware dataplane plus a slow supervisory
CPU plane. The CPU may inspect completed DMA memory, calculate control metrics,
or change a bounded PHY state. It must not become a per-sample dependency for
40 million Q4/I4 samples per second unless throughput is physically proven.

PRs #1 and #2 established the "RF writer -> DMA -> hardware demod -> DAC"
direction and deliberately removed USB, heap allocation, video classification,
and per-block CPU work from realtime pacing.

### Zero-EOF is a real hardware requirement

PR #24 identified a production-critical ESP-IDF PARLIO/GDMA behavior: circular
8-bit transfers still carried EOF markers at ring boundaries. Those boundary
events produced periodic pipeline bubbles that showed up as raster jumps,
black/layer shifts, and horizontal teeth.

C5VRX-3 clears suc_eof on the active cyclic descriptor chains and performs the
required cache synchronization. Hardware testing reported stable raster,
colour, and sync after this change. Later receiver work should preserve this
contract unless an alternative is independently proven.

### 40 MB/s is the proven transport budget

PR #16 explored 80 MS/s reconstruction. PR #18's follow-up hardware work found
persistent FIFO underrun when trying PARLIO TX above the working 40 MB/s class
(80, 60, and 48 MHz attempts were part of that investigation).

The practical design target is therefore not "whatever assembles"; a realtime
BitScrambler path must fit the transport cadence. The repeatedly demonstrated
safe Golden path is two BitScrambler bundles per 50 ns output interval.

PR #68 is a particularly useful negative result: a four-bundle TX-only Phase6
path built and passed host validation, but produced a black screen on COM10
with an empty PARLIO TX FIFO. Build success is not throughput proof.

### LUT capacity is a hard design constraint

The C5 BitScrambler has 2 KiB of LUT storage. Designs that require multiple
independent lookup tables must be factored into one resident representation or
they are not live-feasible. PR #64 eventually reduced exact LIFT-FM to a
shared 1024x16 representation with 24 interstage token classes; earlier larger
factorizations did not fit.

### RX and TX BitScrambler cannot be treated as independent live cores

PR #65 attempted simultaneous RX preprocessing and TX LIFT processing. Hardware
work progressed through reset/prefetch/startup fixes, but the transformed ring
collapsed to near-constant data while the RF source and menu remained healthy.

PR #70 records the decisive project result: on ESP32-C5, the RX and TX
BitScrambler use is effectively half-duplex for this architecture. A live
RX identity path produced 4092/4092 bytes of 0xFF while Golden TX was active;
disabling the RX BitScrambler restored mixed raw IQ. Prefetch and startup-order
changes did not cure it.

Consequences:

- do not design a production path that requires an RX BitScrambler and TX
  BitScrambler to run concurrently;
- PR #69 Polar11 is mathematically interesting, but its original simultaneous
  RX-Polar6 + TX-Polar11 topology is blocked by this later hardware result;
- PR #66 has the same class of topology and also changes the ring away from raw
  Q4/I4, so it cannot be treated as a production candidate without a different
  transport architecture;
- PR #67's single-BitScrambler M2M approach exists specifically to avoid this
  blocker, but its realtime hardware gates are still open.

### Ring format is part of the receiver ABI

ARC, semantic sync, channel search, and other observers in current main expect
raw packed Q4/I4. PR reviews for #66 and #69 caught an important failure mode:
if an RX preprocessor changes the DMA ring into Phase5/Polar bytes and the
existing observers continue decoding those bytes as raw I/Q, control decisions
become meaningless.

Any transformed-ring experiment must either provide format-aware observers or
hard-disable every raw-Q4 consumer.

### Finite M2M is not equivalent to the continuous stream

The exact-adjacent experiments in #53, #54, #55 and #67 use finite M2M blocks.
Finite BitScrambler runs reset internal state. Therefore a live M2M design must
explicitly prove:

- processing time stays inside the raw-data service window;
- no skipped groups or deadline misses occur;
- the previous phase/output state is carried across block boundaries;
- repaired block prefixes do not produce periodic video artifacts;
- long soak tests preserve continuity.

Holding an unwritten tail or repairing the first samples can make a prototype
safer, but does not by itself prove continuous equivalence.

## 3. Acquisition, clocks, and the Q4 boundary

### Q4/I4 is the key observable boundary

C5VRX exposes signed four-bit I and four-bit Q through MODEM_DIAG. Once useful
analog information is collapsed near the origin or clipped into the Q4 rails,
post-Q4 arithmetic cannot recreate the lost analog information.

This is why receiver work must distinguish:

1. pre-Q4 RF/front-end sensitivity and gain placement;
2. FM/demodulation threshold after Q4;
3. final usable CVBS/sync threshold.

PRs #35, #43, #45, #52, #56, #57 and #58 progressively turned this distinction
into instrumentation and control logic.

### Positive 40 MS/s acquisition is the Golden reference

PR #15 explored internal, PLL-derived, and MODEM-derived clock variants.
Public sources did not establish the proposed MODEM_DIAG clock lane, so those
routes stayed hypotheses pending physical scope/capture evidence.

PR #18 later showed why the exact working clock configuration matters: one
early True40 build accidentally changed both CVBS rate handling and RX edge.
The strongest Golden Phase5 visual reference used the positive RX edge.

PR #50 adds a bounded TRUE80 source-synchronous oracle. It should be read as a
measurement gate, not as proof that the production receiver is 80 MS/s or that
TRUE80 improves range.

## 4. Golden Phase5 and demodulation knowledge

### Uniform Phase5 was a major quality improvement

PR #3 replaced the older asymmetric Cartesian state with full-Q4/I4 to a
uniform five-bit circular phase. Its exhaustive host model reduced phase
quantization error from about 9.52 degrees RMS / 25.63 degrees maximum to
3.27 degrees RMS / 5.62 degrees maximum.

PR #5 explored near-origin suppression and centroid mapping. PR #7 corrected a
critical mistake from that line of work: the invalid-state alias sacrificed a
real circular phase state. All 32 Phase5 states must remain available; do not
reintroduce that alias.

### Endpoint winding loss is real

PR #7 corrected the frozen-capture winding measurement to about 8.351% for the
analyzed 40 MS/s dataset. Later range/demod work retained the central identity:

- Golden sees the 50 ns endpoint delta;
- exact adjacent processing sees the two 25 ns deltas;
- the adjacent pair sum can differ from the endpoint result by one full turn.

The exact pair must be formed as:

d0 = wrap(m - p)
d1 = wrap(c - m)
pair = d0 + d1

The pair must **not** be wrapped a second time before video mapping. A second
wrap discards the winding information that adjacent processing recovered.

The 8.351% figure is a dataset observation, not a direct range-loss percentage
or dB value.

### Golden remains the live visual-quality reference

PR #18 initially looked better in offline metrics with a 25 ns True40 adjacent
output, but hardware testing exposed the missing physics:

- adjacent 25 ns differentiation loses the (1 + z^-1) factor present in the
  50 ns endpoint response and therefore loses the useful 20 MHz noise notch;
- the required gain/noise behavior raises high-frequency FM noise;
- the reduced Cartesian representation used by that experiment produced large
  angular quantization errors;
- independent 25 ns DAC noise updates looked visibly noisier than Golden's
  duplicated [A,A] output.

After rail clipping was fixed, True40 still showed sporadic large tears.
The recorded comparison had roughly 15.10% jumps >=16 DAC codes for that
adjacent25 candidate versus about 3.06% for the Golden Phase5 reference.

Reflashing the known Golden Phase5 configuration restored the strongest live
image reported in that investigation: low static, stable sync, and only small
regular edge teeth. Future demodulators should beat that hardware baseline, not
only an offline ideal reference.

### 80 MS/s reconstruction is not a free quality win

PR #16's interpolation path and PR #22's 4-bit@80 packing are useful design
studies. The former hit live transport limitations; the latter was never
hardware-qualified and was later archived. A higher DAC update rate does not
automatically improve the recovered FM waveform if the discriminator noise,
quantization, or transport budget gets worse.

### Trajectory models are evidence, not the Golden replacement

PR #10 proved a two-bundle middle-sample trajectory construction against an
exhaustive software reference and improved its own modeled error distribution.
It explicitly lacked independent RF validation.

PR #46 continued the idea as Trajectory v2 while preserving Golden as the
default. Review work caught a training issue: synthetic IQ must be quantized
using the actual Q4 bucket geometry, not a nearest-integer approximation that
injects a biased training distribution.

The general lesson is to validate learned LUTs against the real quantizer and
then against held-out physical RF captures before treating a model improvement
as receiver improvement.

## 5. Exact-adjacent, LIFT, Phase6, and Polar status

The mathematics is further ahead than the live architecture.

### PR #53 / #54 — exact-adjacent M2M

These paths preserve every Phase5 sample, compute both wrapped adjacent deltas,
sum them without a second wrap, and then output CVBS20. They are valuable
isolation experiments for issue #23.

They have not established a measured sensitivity/range improvement on main.
Their critical gates are M2M throughput, state continuity, and absence of
periodic block artifacts.

### PR #55 — Alpha

Alpha adds a causal confidence-aware predictor after exact adjacent phase
recovery. Its synthetic tests require lower hard-error tails than memoryless
adjacent. That is useful algorithm evidence, not a measured RF-range claim.

### PR #64 — exact LIFT-FM proof

PR #64 is an important mathematical result. For Phase5 triplets it proves the
exact relation between endpoint motion and adjacent motion, and factors the
exact mapping into a two-bundle, one-LUT design using only 24 stage-1 token
classes.

The remaining live problem is not the Phase5-domain identity. It is converting
both new raw Q4/I4 samples into the required phase information inside the same
realtime hardware budget.

### PR #65 — dual-BitScrambler LIFT

Useful as a hardware investigation, but superseded as a live topology. It
exposed RX BitScrambler reset/prefetch behavior and then the simultaneous
RX/TX failure that motivated #67 and was made explicit in #70.

### PR #66 — Phase6 with RX preprocessing

The no-second-wrap Phase6 identity is sound in the host model. The live
architecture still depends on RX preprocessing and changes the ring format,
which creates both throughput and raw-Q4-observer problems. It remains
experimental.

### PR #67 — single-BitScrambler M2M exact adjacent

This is the architectural escape from the simultaneous RX/TX BitScrambler
blocker: keep PARLIO RX raw, run one BitScrambler sequentially for raw->Phase5
and LIFT, and feed a separate CVBS ring to plain PARLIO TX.

The math/oracle is strong. Hardware still has to prove sustained service of the
40 MB/s source, zero deadline/skipped-group faults, finite-block continuity,
valid CVBS, and a real weak-signal benefit.

### PR #68 — TX-only four-bundle Phase6

MEASURED NEGATIVE. It built, but live hardware produced black video and an
empty TX FIFO. Four bundles per 50 ns exceeds the demonstrated realtime budget.

### PR #69 — Polar11

HOST-PROVEN geometry: nested Polar6 contains Phase5 plus one residual bit and
fits an 11-bit / 2048-entry TX lookup with one output each 25 ns.

However, the proposed live topology requires concurrent RX and TX BitScrambler
use. PR #70's later hardware result blocks that architecture as written.

### PR #70 — Phase5+ / half-duplex finding

This PR preserves two major findings:

1. simultaneous RX and TX BitScrambler use is not a viable live route on the
   tested ESP32-C5 architecture;
2. after phase encoding, exact adjacent FM can be factored into 24 interstage
   token classes and one 1024x16 LUT in two TX bundles.

The unresolved bottleneck is raw 40 MS/s IQ -> phase without a concurrent RX
BitScrambler. A CPU preprocessor at 240 MHz has only about six CPU cycles per
input sample, so CPU feasibility must be measured, not assumed.

### Non-PR branch: feat/golden360-middle — Golden360/Adjacent50 capacity proof

HOST-PROVEN NEGATIVE BOUND for the tested two-stage C5 LUT architectures.

This branch asks for a deliberately stricter hybrid target than ordinary exact
adjacent FM:

```text
p,m,c = Phase5(previous,middle,current)
d0 = wrap32(m-p)
d1 = wrap32(c-m)

if winding == 0:
    output must remain byte-exact Golden
else:
    output uses P20/G2(d0+d1), with no second wrap
```

That distinction matters. PR #64 proves that exact Phase5-domain adjacent
mapping itself can be factorized using 24 interstage token classes. Golden360
adds another requirement: all legal no-winding triplets must retain the exact
existing Golden DAC byte. The two results therefore do not contradict each
other.

The exhaustive oracle `tools/prove_golden360_capacity.py` establishes:

- all 1,024 `(middle,current)` pairs have distinct DAC-continuation rows over
  the 32 possible previous phases for the Golden-preserving target;
- the naive `(middle,current)->token; (previous,token)->DAC` decomposition
  therefore needs a 10-bit token and a 15-bit final address, well beyond
  LUT16's 10-bit address;
- every direct split of the 15 Phase5 triplet bits exceeds the available token
  capacity for the tested LUT widths:
  - LUT8: 1,365 / 1,365 direct splits fail;
  - LUT16: 3,003 / 3,003 direct splits fail;
  - LUT32: 5,005 / 5,005 direct splits fail;
- these split tests are optimistic because they allow independent first/second
  tables, while the real C5 stages share one physical LUT;
- all 32 middle phases have distinct winding-continuation behavior over endpoint
  pairs, so an endpoint-independent arbitrary-input middle summary requires
  five phase bits;
- for each of the eight possible direct raw-middle hint bits, there are zero
  endpoint/hint cells where a non-zero winding correction is safe for every
  legal raw middle sample in that hint class.

A concrete collision is `p=0,c=20`: middle phase 10 requires the +360-degree
branch while middle phase 20 requires ordinary Golden, yet both can share the
same raw-I sign. Therefore a one-bit middle-sign correction cannot be exact and
safe on arbitrary legal input.

The proof also found six LUT16 direct partitions for the *clipped ideal*
adjacent50 target if the byte-exact Golden no-winding requirement is dropped.
Those are not Golden360 solutions; they show that preserving Golden's exact
existing transfer is the additional capacity burden.

Important scope limit: this is a lower bound for the tested direct LUT
factorizations and the one-middle-bit idea, not a theorem that no clever use of
C5 counters or external logic can ever solve Golden360. However, combined with
the established live constraints — two bundles per 50 ns, one raw->Phase5
lookup already consumed, endpoint->DAC lookup already consumed, and no
concurrent RX+TX BitScrambler route — it closes the tempting "just add one
middle hint bit" shortcut.

No Golden360 firmware is emitted by the branch. Do not replace Golden with a
model-based one-bit correction: on legal noisy/discontinuous IQ it must produce
wrong winding decisions and can recreate the static/false-pulse failure class
seen in earlier adjacent experiments.

### PR #71 — TX-only PolarState8

This keeps the production raw-Q4 ring and fits one read/write bundle per sample
using current raw IQ plus retained history in a 2048x8 LUT.

First live hardware produced recognizable video but rapid desync/rainbow
artifacts. The initial geometric model is also poor in its own synthetic
metrics (about 14.93 DAC-code RMS error, 13.97% rail outputs, and 70.36%
pedestal outputs). It is a useful proof that TX-only stateful demod can produce
video, not a quality-qualified replacement for Golden.

### PR #72 — Adjacent50 raw-pair bridge with live deadline diagnostics

Evaluated bridging raw IQ pairs over CPU/DMA to feed Adjacent50 math.
Result: without dedicated hardware acceleration, CPU/DMA arbitration violates
the strict 50 ns (20 MS/s) deadline, causing buffer underruns and frame tears.
Confirmed that any demodulator must execute entirely within the
BitScrambler/PARLIO silicon dataplane.

### PR #73 & #74 — Counter-A / Static-A Relative Golden

Investigated relative worker accumulation in the live two-bundle video path.
PR #73 showed that unanchored relative integration accumulates drift across
long active video scanlines. PR #74 demonstrated that anchoring Relative
Golden to calibrated Golden DAC levels guarantees 100% video sync and color
stability while preserving the 50 ns timing budget.

### PR #75 — Pages release race resolution

Fixed race condition where a PR prerelease deleted upon merge caused the
subsequent GitHub Pages firmware mirror build to abort. Hardened download
pipeline against transient release lifecycles.

### PR #76 — Alternating Middle Phase5 and Static-A Live Qualification

Proved that Static-A Relative Golden coexists without BitScrambler instruction
or memory collisions. Confirmed live NTSC video with 0 rainbow artifacts on
hardware.

### PR #77 — Phase5-360 Simulator and Architecture Specification

Formalized full 360° phase unwrapping for Phase5 triplets $[-32 .. +30]$ bins
(span $-360^\circ .. +337.5^\circ$). Demonstrated algebraic $r_M$ amplitude
cancellation proof (residual $< 10^{-14}$ rad). Proved clear separation
between NTSC 3.58 MHz chroma ($|\Delta| \le 5$ bins, $<56^\circ$) and 180°
edge wraps ($|\Delta| \ge 12$ bins, $>135^\circ$).

### Phase5-360 Live Qualification: Rainbow Root Cause, Option A & Option B

- **Rainbow Root Cause**: Proved that single-bit Cartesian IQ gating (`if 7` or
  any single bit) clamps 3.58 MHz subcarrier transitions into 4,137 square-wave
  spikes per field. An exhaustive 65,536-triplet scan proved that no single
  Cartesian bit can cleanly decide winding without false alarms.
- **Option A (Delta-Gated Phase5-360 Core)**: For all 736 $(P, C)$ pairs with
  $|\Delta| < 12$ (100% of chroma and fine detail), output is byte-identical to
  Golden DAC (strictly 0 false alarms, 0 rainbow artifacts). For the 288 pairs
  with $|\Delta| \ge 12$, output is clamped to calibrated rails (63 if $\Delta < 0$,
  0 if $\Delta > 0$). Verified: 0 false alarms, completely eliminates rainbow
  artifacts in goggles.
- **Option B (2-Bit Quadrant Oracle & 16-Bit Word Packing)**: Proved that a
  2-bit quadrant oracle ($M_Q = \text{bit } 3, M_I = \text{bit } 7$) yields 256
  $(P, C)$ cells with 100% unanimous winding and strictly 0 false alarms
  (16,384 safe corrections). Proved that complete 16-bit word packing fits into
  a single 1024x16 LUT: `bits[5:0]` DAC (6b), `bits[9:6]` Quadrant flags (4b),
  `bit[10]` Rail polarity (1b), `bits[15:11]` Phase5 for Controller (5b)
  ($6 + 4 + 1 + 5 = 16\text{ bits}$). Proved `if L6+a` is a valid silicon
  opcode on ESP32-C5 BitScrambler.

## 6. RF gain, range, and ARC knowledge

### Old fixed-gain assumptions were wrong

Earlier work often treated a value around G52/G62 as the high-sensitivity
region. The PRE-Q4 and ARC V3 hardware work showed that the useful generated
vendor table extends much farther and is not globally smooth.

PR #56 recorded a highest decoded RF-stage span around G62..G81 in the tested
runtime table. A medium-distance sweep showed a non-monotonic example:
G62 good -> G63 collapse -> G64/G65 recover. Therefore numerical gain index
+/-1 is not a guaranteed smooth RF response.

At close range, high gain can clip badly. At weak range, some states above G62
can be essential to place the remaining analog information usefully into Q4.

### TX/DAC self-noise was not the dominant measured range limiter

PR #56's first ACTIVE/QUIET tests did not show a repeatable raw-Q4 improvement
when PARLIO/DAC TX was silenced. Some runs stayed similar and some degraded
while quiet. These tests do not prove zero coupling, but they do not support a
large multi-dB TX-self-noise penalty as the primary range problem.

### Downstream gain cannot recreate a lost pre-Q4 carrier

One far sweep remained essentially dead even at G80/G81. Once the RF
information is gone before Q4, more downstream gain only amplifies noise.

This coexists with a second important result: in other walk-test conditions,
better gain placement kept Q4 healthy much farther than the old controller did.
These are not contradictory; they separate controller-induced Q4 starvation
from the true physical RF sensitivity limit.

### Gain placement was a major contributor to the old practical range cliff

PR #58's ARC V3 hardware walk moved through a wide part of the vendor table,
roughly:

- very strong/close: G14-G18 class;
- close: G35-G46 class;
- medium-close: G50-G56 class;
- medium/weak: G60-G74 class;
- very weak/far: G77-G81 class.

These are empirical search anchors, not calibrated distance or dB mappings.

In the recorded close->far and far->close walks, the controller reached high
G70/G80-class states at weak locations and returned downward near the
transmitter. At a location that had represented the previous practical far
limit, raw Q4 could still be healthy and video was reported good. This strongly
supports pre-Q4 gain placement as a major contributor to the earlier range
limit.

### BW20 "+3 dB" must not be revived as an assumption

PR #25 proposed a BW40/BW20 weak-signal gearbox. Later hardware work summarized
in PR #31 found that narrowing the pre-discriminator bandwidth damaged analog
FM detail/chroma and did not demonstrate the advertised raw-MODEM_DIAG survival
gain. Production therefore returned to fixed BW40.

Any future BW20 proposal needs a controlled Q4/CVBS hardware A/B rather than a
thermal-noise calculation alone.

### Fast observer, slow actuator

PRs #43 and #45 established a useful control principle:

fast read-only sensing -> state estimate / confidence -> rare physical write

not:

fast sensing -> continuous PHY writes.

Gain, bandwidth, AFC, or other PHY writes can themselves disturb the recovered
waveform and cause decoder relock that looks much longer than the original
glitch.

### Classify no-carrier before overload/noise

Review and controller iterations in #43, #45, #48, #52, #59, #60 and #61
repeatedly found the same trap: amplified noise can contain rail hits or
non-zero power. If clipping/overload logic runs before no-carrier evidence, the
controller can reduce sensitivity exactly when the wanted signal is absent.

No-carrier handling must have explicit precedence and must not train adaptive
models on noise.

### ARC foundation in main

PR #52 reconstructs the vendor RX gain table and decodes vendor gain state into
RF/BB/fine components. It removed the unsafe one-argument private
phy_agc_max_gain_set route and keeps undocumented RXDC/IQ/filter writes gated.

Follow-up fixes in the same PR established important rules:

- persistent no-sync returns to the first entry of the highest RF stage rather
  than climbing indefinitely on noisy Q-phase;
- severe clipping is checked before an ordinary settle return;
- a successful channel retune recaptures the vendor table/state and reseeds ARC;
- calibrated IQ/filter/ADC state is kept as an observed receive tuple;
- RXDC reapplication stays excluded until ownership/state reconstruction is
  complete.

### ARC V3

PR #57 made receiver discovery reference-guarded and separated ACQUIRED,
OVERLOAD, RF_LIMIT, UNSTABLE, and INCONCLUSIVE outcomes.

PR #58 then made raw-Q4 gain placement the live control variable while holding
BW40 and carrier offset fixed. Its key correction was allowing collapsed Q4 to
request more gain without first demanding semantic sync — breaking the old
circular dependency where the controller needed a good phase estimate before
it would move to the gain required to obtain one.

### ARC V4 SNAP/GLIDE

PRs #59-#61 are useful control experiments, not current production authority.
They explored pre-cliff handoff, overlapping small gain steps, and calibrated
emergency anchors. Reviews exposed no-carrier precedence, stale integer-EMA
history, and diagnostic-actuator ownership hazards. Preserve those lessons
even if the controllers themselves are not promoted.

### ARC V5

PR #62 is merged and adds a persistent predictive layer over ARC V3. It learns
local short-horizon response of vendor gain transitions rather than a
distance->gain table:

- dP/dG;
- dQ/dG;
- dOrigin/dG;
- dClip/dG;
- settling time.

Its model is versioned, fingerprinted against the generated vendor gain table,
CRC-protected, and rate-limited in NVS. Clean Fusion observations are an
authoritative zero-write hold. Predictions require confidence on the exact
edge being extrapolated, and accepted learning waits at least the hardware
settle interval.

ARC V5 being present in main does **not** mean it is the normal default profile:
current source initializes s_rx_profile to RX_PROFILE_ARC. PR #63 proposed the
V5 production/menu promotion but was not merged.

## 7. CVBS menu, recovery, flashing, and CI lessons

### Menu timing and memory are hardware constraints too

PR #26 moved descriptor boundaries away from the H-sync falling edge by
starting scanline segments in a porch/blanking interval. It also hardened
console and descriptor ownership.

During PR #52 testing, a black-screen/reboot menu failure was traced on COM10
to ESP_ERR_NO_MEM from the old large descriptor allocation after Wi-Fi/PHY
startup. The fix reduced the standalone raster to a two-field allocation of
about 19.2 KiB and allocates before stopping live video, so allocation failure
does not destroy the working receiver path.

Rule: allocate/validate the replacement raster before tearing down known-good
live video.

### Recovery must bypass persisted bad settings

PR #49 added the unconditional three-second BOOT recovery path. A persisted
menu-disable or experimental setting must not be able to make recovery
unreachable.

### Web flashing

PR #31 fixed ESP32-C5 WebSerial flashing details that should remain preserved:

- firmware payload as Uint8Array;
- compressed flashing disabled for the observed C5 native USB/JTAG failure
  path;
- corrected C5 SPI register base;
- immutable semantic-version firmware releases instead of a mutable main asset.

PR #40 showed that the GitHub release asset path still hit browser CORS after
redirect. PR #41 solved the production path by mirroring firmware server-side
into the trusted GitHub Pages artifact and downloading it same-origin through
a generated releases manifest. Do not reintroduce an external CORS proxy.

### PR build/release workflow

PR #38 fixed a subtle Actions race: pull_request:closed and push:main can happen
together, and a ref-only concurrency group allowed cleanup to cancel the real
main build. Concurrency must distinguish event/source.

PR #39 was intentionally a disposable PR-build publication smoke test and was
never meant for main.

PR #44 adds an optional PlatformIO path, but the pinned ESP-IDF/Docker build
remains the project reference when equivalence matters.

PR #42's ESP32-S3 link-mode receiver is an interesting external-display
experiment, but it intentionally sits outside the core standalone analog-CVBS
production goal.

## 8. Rules that should survive future refactors

1. Preserve Zero-EOF circular GDMA unless a replacement is physically proven.
2. Treat 40 MB/s and the two-bundle Golden cadence as the live budget to beat,
   not a suggestion.
3. Never second-wrap the sum of the two adjacent phase deltas.
4. Preserve all 32 Phase5 states; do not recreate the rejected invalid-state
   alias.
5. Do not infer live quality from an offline error metric alone.
6. Golden Phase5 is the visual non-regression reference until hardware A/B
   proves a replacement.
7. Do not run RX and TX BitScrambler concurrently in the live architecture.
8. A transformed DMA ring requires transformed/format-aware observers.
9. Finite M2M needs explicit state stitching, deadline accounting, and long
   continuity tests.
10. Do not assume vendor gain indices are linear or globally monotonic.
11. Do not treat raw power or isolated clipping as carrier proof.
12. Classify NO_CARRIER before recovery logic that can reduce sensitivity.
13. Do not train persistent adaptive control on NO_CARRIER or unstable
    cross-context transitions.
14. Keep clean/locked video a zero-PHY-write state.
15. BW20 is an experiment, not a free +3 dB range switch.
16. Separate pre-Q4 sensitivity, demodulation threshold, and usable-CVBS
    threshold in every range claim.
17. Empirical gain anchors are not calibrated dB or distance.
18. Allocate replacement menu/video resources before stopping a known-good
    live stream.
19. A successful build/oracle is not a hardware-throughput or RF-quality pass.
20. Negative experiments are permanent knowledge; do not delete the reason a
    route was rejected.

## 9. PR coverage ledger

This ledger gives the shortest disposition of every C5VRX PR found in the
repository through #71.

| PR | Preserved knowledge |
|---|---|
| [#1](https://github.com/Twotoz/C5VRX/pull/1) | Minimal RF-writer -> hardware demod -> PARLIO architecture; keep CPU/USB out of realtime pacing. |
| [#2](https://github.com/Twotoz/C5VRX/pull/2) | Long-lived pre-trigger IQ, cyclic transport, state continuity as a hardware gate. |
| [#3](https://github.com/Twotoz/C5VRX/pull/3) | Uniform full-Q4 Phase5; large modeled quantization improvement. |
| [#4](https://github.com/Twotoz/C5VRX/pull/4) | Unified legacy history, knowledge, attribution, and licensing. |
| [#5](https://github.com/Twotoz/C5VRX/pull/5) | Centroid/near-origin experiment; invalid-state part superseded by #7. |
| [#7](https://github.com/Twotoz/C5VRX/pull/7) | Correct winding measurement, preserve all 32 phase states, edge A/B discipline. |
| [#8](https://github.com/Twotoz/C5VRX/pull/8) | 4092-byte PARLIO descriptor reality and boundary-analysis discipline. |
| [#10](https://github.com/Twotoz/C5VRX/pull/10) | Two-bundle trajectory LUT; exhaustive host proof, no independent RF proof. |
| [#15](https://github.com/Twotoz/C5VRX/pull/15) | Source-clock/edge oracle; MODEM debug clock lane remained a hypothesis. |
| [#16](https://github.com/Twotoz/C5VRX/pull/16) | 80 MS/s reconstruction study; LUT size and throughput are hard gates. |
| [#18](https://github.com/Twotoz/C5VRX/pull/18) | True40 adjacent25: offline promise, live static/tear regression; Golden baseline confirmed. |
| [#19](https://github.com/Twotoz/C5VRX/pull/19) | RPT40 checkpoint; never hardware-qualified, later superseded. |
| [#22](https://github.com/Twotoz/C5VRX/pull/22) | 4-bit@80 packing/oracle; archived without hardware qualification. |
| [#24](https://github.com/Twotoz/C5VRX/pull/24) | C5VRX-3 Zero-EOF breakthrough; later RF-setting claims superseded by newer ARC/BW work. |
| [#25](https://github.com/Twotoz/C5VRX/pull/25) | C5VRX-3 production-root migration; early AGC/BW ideas later refined/superseded. |
| [#26](https://github.com/Twotoz/C5VRX/pull/26) | Porch-aligned menu raster, console/descriptor hardening, PAL/NTSC support. |
| [#29](https://github.com/Twotoz/C5VRX/pull/29) | Docker build/flash GUI and reproducible desktop flow. |
| [#30](https://github.com/Twotoz/C5VRX/pull/30) | Initial browser flasher/release integration. |
| [#31](https://github.com/Twotoz/C5VRX/pull/31) | C5 WebSerial fixes, immutable releases, fixed BW40, safer gain transitions, transport diagnostics. |
| [#32](https://github.com/Twotoz/C5VRX/pull/32) | Experimental BW selector and 4-bit@80 runtime A/B; not production evidence. |
| [#33](https://github.com/Twotoz/C5VRX/pull/33) | Persistent settings, channel autosearch, live menu signal meter. |
| [#34](https://github.com/Twotoz/C5VRX/pull/34) | Refined menu persistence/search/raster; hardware flash validation. |
| [#35](https://github.com/Twotoz/C5VRX/pull/35) | Controlled gain/lag/PHY characterization framework; avoid guessed undocumented controls. |
| [#36](https://github.com/Twotoz/C5VRX/pull/36) | Gain trials with rollback and semantic evidence; heuristic quality is not calibrated SNR. |
| [#37](https://github.com/Twotoz/C5VRX/pull/37) | Shadow exact-adjacent winding observer and semantic CVBS sync scoring. |
| [#38](https://github.com/Twotoz/C5VRX/pull/38) | Actions concurrency must separate PR cleanup from main push. |
| [#39](https://github.com/Twotoz/C5VRX/pull/39) | Disposable PR-build publication smoke test; do not merge. |
| [#40](https://github.com/Twotoz/C5VRX/pull/40) | Direct GitHub asset API still exposed browser CORS redirect limits. |
| [#41](https://github.com/Twotoz/C5VRX/pull/41) | Same-origin GitHub Pages firmware mirror; trusted-main web deployment. |
| [#42](https://github.com/Twotoz/C5VRX/pull/42) | External ESP32-S3 link/display experiment, intentionally outside core product path. |
| [#43](https://github.com/Twotoz/C5VRX/pull/43) | Fusion measurement/learner framework; no-carrier precedence is critical. |
| [#44](https://github.com/Twotoz/C5VRX/pull/44) | Optional pinned PlatformIO environment; toolchain equivalence matters. |
| [#45](https://github.com/Twotoz/C5VRX/pull/45) | Range v2: RF vs demod vs CVBS thresholds, fast observer/slow actuator, risk tails. |
| [#46](https://github.com/Twotoz/C5VRX/pull/46) | Trajectory v2; learned quantizer must match real Q4 bucket geometry. |
| [#48](https://github.com/Twotoz/C5VRX/pull/48) | Range V3/Q10 teacher experiments; keep BW/AFC fixed when isolating gain. |
| [#49](https://github.com/Twotoz/C5VRX/pull/49) | Unconditional three-second BOOT recovery from persisted experimental states. |
| [#50](https://github.com/Twotoz/C5VRX/pull/50) | TRUE80 bounded source-synchronous acquisition oracle, not a production/range claim. |
| [#52](https://github.com/Twotoz/C5VRX/pull/52) | Vendor-aware ARC foundation, safe gain-table ownership, retune recapture, menu-memory hardware fix. |
| [#53](https://github.com/Twotoz/C5VRX/pull/53) | Exact-adjacent Phase5 M2M experiment; no-second-wrap math, hardware gates remain. |
| [#54](https://github.com/Twotoz/C5VRX/pull/54) | Exact-adjacent M2M with confidence/boundary handling and strict service-window accounting. |
| [#55](https://github.com/Twotoz/C5VRX/pull/55) | Alpha predictive adjacent tracker; synthetic benefit is not measured range. |
| [#56](https://github.com/Twotoz/C5VRX/pull/56) | PRE-Q4 lab; TX self-noise not dominant in first tests; gain table is wide/non-monotonic. |
| [#57](https://github.com/Twotoz/C5VRX/pull/57) | Reference-guarded ARC V3 auto-lab and explicit RF-limit classification. |
| [#58](https://github.com/Twotoz/C5VRX/pull/58) | Gain-first ARC V3; hardware walks show gain placement was a major old range limiter. |
| [#59](https://github.com/Twotoz/C5VRX/pull/59) | SNAP pre-handoff experiment; reviews exposed recovery/confirmation hazards. |
| [#60](https://github.com/Twotoz/C5VRX/pull/60) | First GLIDE overlap experiment; no-carrier and EMA pitfalls. |
| [#61](https://github.com/Twotoz/C5VRX/pull/61) | GLIDE continuation; diagnostic actuator ownership and no-carrier precedence remain key. |
| [#62](https://github.com/Twotoz/C5VRX/pull/62) | Merged ARC V5 persistent predictive local-gain model over ARC V3. |
| [#63](https://github.com/Twotoz/C5VRX/pull/63) | Proposed simplified V5 production menu/default; closed unmerged, so not current main behavior. |
| [#64](https://github.com/Twotoz/C5VRX/pull/64) | Exact LIFT-FM Phase5-domain factorization: 24 token classes, one LUT, two bundles. |
| [#65](https://github.com/Twotoz/C5VRX/pull/65) | Dual-BitScrambler LIFT hardware investigation; live topology superseded/blocked. |
| [#66](https://github.com/Twotoz/C5VRX/pull/66) | Phase6 no-second-wrap concept; RX-predecode and transformed-ring gates unresolved. |
| [#67](https://github.com/Twotoz/C5VRX/pull/67) | Single-BitScrambler two-pass M2M exact-adjacent draft; hardware realtime gates open. |
| [#68](https://github.com/Twotoz/C5VRX/pull/68) | Four-bundle TX-only Phase6 hardware failure: black screen / TX FIFO empty. |
| [#69](https://github.com/Twotoz/C5VRX/pull/69) | Polar11 host geometry; original concurrent RX/TX topology blocked by #70. |
| [#70](https://github.com/Twotoz/C5VRX/pull/70) | Half-duplex BitScrambler hardware blocker; exact 24-token adjacent factorization retained. |
| [#71](https://github.com/Twotoz/C5VRX/pull/71) | TX-only PolarState8 produced video but rapid desync/rainbow; model quality still poor. |
| [#72](https://github.com/Twotoz/C5VRX/pull/72) | Experimental Adjacent50 raw-pair bridge; DMA/CPU transfer violates 50 ns deadline, proving all demod must run in BitScrambler. |
| [#73](https://github.com/Twotoz/C5VRX/pull/73) | Two-bundle Counter-A relative worker; unanchored relative integration accumulates drift over long scanlines. |
| [#74](https://github.com/Twotoz/C5VRX/pull/74) | Relative Golden in live two-bundle video path; anchoring to Golden DAC preserves 100% sync and color stability. |
| [#75](https://github.com/Twotoz/C5VRX/pull/75) | Hardened Pages release mirror against disappearing PR releases and rate limits. |
| [#76](https://github.com/Twotoz/C5VRX/pull/76) | Alternating middle Phase5 and Static-A Relative Golden; proved clean collision-free operation and zero rainbows. |
| [#77](https://github.com/Twotoz/C5VRX/pull/77) | Full Phase5-360 simulator and architecture specification; proved algebraic r_M cancellation and separation of chroma vs 180° edge wraps. |
| [#83](https://github.com/Twotoz/C5VRX/pull/83) | Pedestal-Gated Back-Porch AFC; measured CFO strictly during horizontal blanking back-porch with ±3.5 kHz precision. |
| [#86](https://github.com/Twotoz/C5VRX/pull/86) | Direct Phase5-360 FPGA demodulator draft; validated full (P,M,C) triplet interval logic in 3-stage RTL pipeline. |
| [#87](https://github.com/Twotoz/C5VRX/pull/87) | C5 BitScrambler M2M speedlab; confirmed PARLIO TX streaming remains the only verified zero-drop 40 MHz transport. |
| [#88](https://github.com/Twotoz/C5VRX/pull/88) | Exact endpoint-conditioned one-bit middle reduction; proved half-plane condition resolves >180° wrapping after endpoints are set. |
| [#90](https://github.com/Twotoz/C5VRX/pull/90) | Merged Counter-A hardware subtraction (ADDCTIAL); executes modulo-32 subtraction in 1 cycle, freeing 960 words in LUT RAM. |
| [#91](https://github.com/Twotoz/C5VRX/pull/91) | Compact Phase5-360 winding model; proved that non-linear 2D table compression introduces DAC deviations on normal pixels. |
| [#92](https://github.com/Twotoz/C5VRX/pull/92) | Live Q2 adjacent-50 demodulator; 5-bit learned token resolved synthetic edge wraps but added ~5% quantization grain to clean video ("niet echt clean"). |
| [#94](https://github.com/Twotoz/C5VRX/pull/94) | Q[6:3]/I[6:3] tap & max gain lock; proved Q[6:3] drops true sign bits, causing modulo-16 magnitude wrapping, permanent AGC clip-veto, and desync. Q[9:6] is mandatory. |

### Non-PR branch & Phase5-360 production coverage

| Branch / Phase | Preserved knowledge |
|---|---|
| [feat/golden360-middle](https://github.com/Twotoz/C5VRX/tree/feat/golden360-middle) | Exhaustive Golden360/Adjacent50 capacity proof: exact one-bit middle correction is unsafe; all tested direct LUT8/LUT16/LUT32 Golden-preserving two-stage splits exceed token capacity; proof and oracle are preserved on main. |
| [Phase5-360 Option A](tools/gen_phase5_360.py) | Delta-gated 360° core: $|\Delta| < 12$ emits exact Golden DAC (100% chroma immunity, 0 rainbows); $|\Delta| \ge 12$ clamped to calibrated rails (63/0) for clean edge recovery. |
| [Phase5-360 Option B](tools/phase5_360_architecture.md) | 2-bit quadrant oracle ($M_Q$=bit 3, $M_I$=bit 7): 256 unanimous cells, 16,384 safe corrections, 0 false alarms. Packs 16-bit word cleanly (6b DAC, 4b flags, 1b rail, 5b Phase5) with `if L6+a`. |

## 10. How to use this document

Before starting a new RF, demod, DMA, gain, or CVBS experiment:

1. read the relevant current source and focused document from
   docs/KNOWLEDGE_INDEX.md;
2. check this PR-derived summary for a previous hardware failure or corrected
   assumption;
3. state whether the new claim is mathematical, host-validated, bounded
   hardware, or sustained live-RF evidence;
4. compare any visual-quality replacement against the known Golden Phase5
   hardware baseline;
5. preserve a negative result in documentation even when the implementation
   itself should not merge.
