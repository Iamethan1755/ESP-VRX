# Trajectory v2 — weak-signal adjacent-FM research path

Trajectory v2 is an experimental realtime demodulator stacked on top of
Range v2. It exists to attack the measured endpoint-winding failure in issue
#23 without exceeding the ESP32-C5 BitScrambler realtime budget.

It is **not** the new default. Golden Phase5 remains the boot/default
demodulator until controlled RF hardware A/B testing proves otherwise.

## Problem

Golden Phase5 keeps one 40-MS/s Q4/I4 sample out of every two before the FM
branch decision:

```text
p -------- m -------- c
|                     |
+---- endpoint FM ----+
```

The correct 40-MS/s trajectory is:

```text
d0 = wrap(phi[m] - phi[p])
d1 = wrap(phi[c] - phi[m])

pair = d0 + d1
```

The pair sum must **not** be wrapped again. If it is wrapped again, the
middle-sample winding information has been discarded a second time.

Issue #23 measured endpoint winding disagreement at a meaningful rate,
especially when Q4/I4 amplitude is weak. That creates a plausible usable-video
cliff before the RF front end has necessarily lost all useful information.

## Why not run a classical PLL on the CPU?

At 40 MS/s a 240-MHz CPU has only about six CPU cycles per complex sample
before any other firmware work is considered. A normal PLL needs phase
detection, loop filtering, state update and NCO rotation.

That is not a credible live C implementation for this data plane.

The existing live design intentionally keeps:

```text
40 MB/s IQ pacing -> DMA / PARLIO / BitScrambler hardware
slow control       -> CPU
```

Trajectory v2 preserves that split.

## Why not use the old full-Q4 exact-adjacent BitScrambler directly?

The legacy repository already contains mathematically exact adjacent/pair-sum
programs. They perform:

1. full raw-Q4 phase lookup;
2. adjacent delta 0;
3. adjacent delta 1;
4. accumulation;
5. final video mapping.

The math is correct, but the program needs far more instruction bundles than
the proven live decorator can execute at 40 MB/s input.

The current physical budget is effectively two steady-state BitScrambler
bundles per 50-ns output period.

A correct algorithm that underruns the PARLIO TX FIFO is not a usable receiver.

## Why M2M is not silently promoted

ESP-IDF supports BitScrambler loopback for memory-to-memory transformation,
but on ESP32-C5 loopback disables the RX BitScrambler and loops the TX
BitScrambler back to the receive DMA path.

That makes loopback useful for bounded hardware oracles, but it is not a free
second independent DSP engine beside the already-attached live PARLIO
BitScrambler.

A future split architecture remains valid only after it proves:

- sustained 40 MB/s raw input;
- sustained 20 MB/s CVBS production;
- no FIFO errors;
- state continuity over cyclic boundaries;
- enough margin for a real flight receiver.

## Why not direct True40 adjacent video?

The project already tested an adjacent-25-ns output path.

It could meet the transport rate, but live picture quality regressed because:

- the endpoint path contains an implicit two-sample boxcar factor;
- direct adjacent differentiation removes that high-frequency noise notch;
- a 25-ns phase delta needs roughly twice the video gain of a 50-ns delta for
  equal CVBS swing;
- compressed IQ5 phase states added substantial angular error.

So the target remains:

```text
DISCRIMINATE USING 40M TRAJECTORY INFORMATION
                 ↓
COMBINE / REDUCE TO QUIET 20M VIDEO
                 ↓
PROVEN [D,D] 40M DAC TRANSPORT
```

not raw noisy adjacent-25-ns samples straight to the DAC.

## Chosen live architecture

Trajectory v2 now uses the same physical 1024x16 BitScrambler LUT twice in
each 50 ns output period. This avoids throwing away a Phase5 bit just to expose
the middle sample.

### Stage 1 — raw-Q4 trajectory token

While `emit` prefetches the next `[middle,current]` raw-Q4 pair, it addresses
the LUT with:

```text
current raw Q4/I4 byte         8 bits
middle raw-I sign              1 bit
previous actual Phase5 MSB     1 bit
                              -------
                              10 bits
```

The high/spare bits of that LUT word return two things:

```text
actual current Phase5          5 bits
learned trajectory token       5 bits
```

Actual current Phase5 is therefore still exact with respect to the same
production Phase5 decode used by Golden. The token is a compact learned summary
of the local two-adjacent trajectory, including information from the middle
sample and raw-Q4 amplitude/noise state.

### Stage 2 — previous Phase5 + token → CVBS

The next steady-state bundle addresses the same LUT again with:

```text
previous actual Phase5         5 bits
trajectory token               5 bits
                              -------
                              10 bits
```

The low six bits of that second LUT result are the final 6-bit CVBS code.

The steady-state loop remains exactly:

```text
trajectory
   ↓
emit + prefetch
   └────────────> trajectory
```

So Trajectory v2 still uses only two BitScrambler bundles per 50 ns output and
keeps the proven output contract:

```text
20 MS/s unique 6-bit CVBS
 -> [D,D]
 -> physical 40-MHz resistor DAC
```

No CPU pixel DSP is introduced.

## LUT target

The deterministic trainer generates physically plausible local FM motion,
carrier phase, amplitude fades and Q4 noise. For every training triplet it
knows the clean local adjacent trajectory:

```text
d0 = local previous→middle phase increment
d1 = local middle→current phase increment
target = map_to_CVBS(d0 + d1)
```

There is deliberately **no second wrap around `d0+d1`**. Preserving that branch
information is the reason Trajectory v2 exists.

Training is two-stage. Stage 1 learns one of 32 trajectory tokens for every
raw-Q4/context address. Stage 2 learns the best CVBS code for
`previous Phase5 + token`. The trainer iterates those assignments with a
deterministic L1 objective, then emits both the 16-bit BitScrambler LUT words
and matching supervisory tables.

This means the live path is not a bit-exact full-Q4 atan2 discriminator.
It is a hardware-budgeted estimator trained toward the clean adjacent-FM
trajectory while retaining full current Phase5 state.

## Confidence

The generated confidence table belongs to **stage 1**. It scores how tightly
the training targets associated with one raw-Q4/context address agree after
the learned token mapping.

Small residual:
- stage-1 state is informative;
- high confidence.

Large residual:
- different plausible trajectories collapse onto the same compressed state;
- low confidence.

The confidence table is supervisory only. It does not put CPU DSP in the
40-MS/s video path. When TRAJ V2 is selected, Range/Fusion can use this
uncertainty as additional catastrophic-risk evidence. When GOLDEN is selected,
that Trajectory-only penalty is explicitly zeroed so A/B comparison remains
clean.

## PLL-lite

A full PLL is retained as an offline upper-bound benchmark.

The realtime supervisor instead has a tiny observation-only predictor:

1. learn local adjacent phase slope only from sufficiently strong IQ;
2. when the envelope is weak, coast instead of learning a large noisy jump;
3. flag a likely phase-slip when:
   - the two-adjacent trajectory disagrees with the endpoint branch;
   - IQ is weak;
   - the innovation versus the last clean slope is large.

Those metrics feed:

- `pll_lite_slip_permille`;
- `pll_lite_hold_permille`;
- Fusion catastrophic risk;
- Range v2 gain/BW/AFC decisions.

The predictor currently does **not** rewrite live CVBS samples. This is
deliberate: a false phase-slip correction can be more destructive than one
Golden fallback error. Pixel correction should be promoted only after real
Q4 captures prove a safe rule.

## Why this is safer than unconditional click suppression

Real analog-FPV video can contain large legitimate instantaneous frequency
excursions.

A rule such as:

```text
if abs(delta) is large:
    smooth it
```

will destroy real sync, luma edges or chroma.

The intended future recovery gate is closer to:

```text
low envelope
AND trajectory/endpoint branch disagreement
AND large innovation versus trusted local slope
AND compressed-state confidence is low
    -> short predictor/holdover candidate
```

That is the C5-friendly equivalent of using loop inertia through a brief
uncertain phase event.

## Menu

On the VIDEO page:

```text
DAC      6BIT@40
DEMOD    GOLDEN
```

Controls:

- normal long press: toggle DAC output;
- >=2 s hold: switch `GOLDEN <-> TRAJ V2`.

The two experimental contracts are mutually exclusive but neither menu option
is hidden:

- selecting TRAJ V2 automatically selects `6BIT@40`;
- selecting `4BIT@80` while TRAJ V2 is active automatically returns the
  demodulator to GOLDEN.

This keeps every live BitScrambler/TX combination valid while still allowing
4BIT@80 to be selected directly from the VIDEO page.

Range v2 and demod mode are independent:

```text
RF PROFILE = RANGE V2
VIDEO DEMOD = TRAJ V2
```

is the combined experimental receiver.

## Verification tools

### Structural/generator oracle

```sh
python3 tools/train_trajectory_v2.py --self-test
```

Checks:

- 1024 stage-1 states and 1024 stage-2 address states;
- embedded raw-Q4 -> actual Phase5 high bits;
- learned 5-bit token packing/unpacking;
- generated DAC, token and confidence table equality;
- pinned generated-table SHA256;
- exactly two steady-state bundles;
- persistent downstream/no-EOF transport contract.

Regeneration is explicit:

```sh
python3 tools/train_trajectory_v2.py --write --self-test
```

Regeneration is deterministic and dependency-free; normal firmware builds do
not silently retrain the LUT. The embedded tables are pinned by SHA-256.

### Weak-signal benchmark

```sh
python3 tools/range_demod_bench.py raw-q4.bin --json
```

The same capture now reports:

- Golden endpoint winding;
- exact full-Q4 adjacent oracle;
- Trajectory v2 error versus the exact oracle;
- hard >=16 / >=32 DAC-code tail;
- trajectory confidence;
- PLL-lite offline output;
- full second-order PLL telemetry.

The CI self-test also runs a deterministic synthetic high-deviation weak-FM
case. This is a regression test, **not a range measurement**.

## Required hardware A/B

Use the same VTX, camera, RF channel, antenna, RX edge and output DAC.

Compare at minimum:

1. RANGE V2 + GOLDEN;
2. RANGE V2 + TRAJ V2.

At several controlled attenuation levels log:

- origin permille;
- clipping;
- Q_phase;
- endpoint winding;
- trajectory uncertainty;
- PLL-lite slip/hold rate;
- semantic sync quality;
- transport faults;
- visible hard specks/tears;
- decoder relocks;
- usable-picture threshold.

Then replay frozen raw-Q4 snapshots through:

1. Golden;
2. exact full-Q4 adjacent;
3. Trajectory v2;
4. PLL-lite oracle;
5. full PLL oracle.

## Acceptance criteria

Trajectory v2 should not become the default until all of these are true:

- ESP32-C5 firmware compiles and the BitScrambler program loads;
- no TX FIFO empty / GDMA / PARLIO faults;
- long-run state survives ring wraps;
- strong-RF video is not worse than Golden;
- hard weak-RF error tail drops on real captures;
- semantic sync survives farther into controlled attenuation;
- visible usable-video threshold improves reproducibly.

The performance claim belongs to the hardware test, not to the LUT generator.
