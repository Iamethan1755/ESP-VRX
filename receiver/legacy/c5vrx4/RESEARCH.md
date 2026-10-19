# Long-range receiver design research (2026-09-30)

## Scope and evidence

Objective requested by the operator: maximum useful analog-video range with
at least Golden Phase5 picture quality, within two steady-state BitScrambler
bundles. Phase8 is a comparison baseline, not a required final architecture.
No new firmware, demodulator, PHY setting or flash is implemented by this note.

The main review is pinned to `59a8713b467916f00436642702a4b14e9598c662`
(PR #127, Direct Gain V5). The PR #122 worktree additionally contains native
AGC pacing and finer lane choices, including `5878115`. These are different
gain policies. Findings about V5 do not describe the currently flashed native
receiver. Historical documents are evidence scoped to their experiments;
their statements about immutable algorithms or universal video phase bounds
are not design requirements.

Operator evidence: the initial 1000 us / 20 us paced-native build was reported
as "een stuk beter". This establishes a subjective stationary-picture
improvement, not extra RF sensitivity, reliable gain reacquisition or an
optimal period. The requested 5 ms switch was not confirmed during this
research because COM10 was occupied. Subsequent worktree commits do not prove
that corresponding firmware is running on the board.

## Hard hardware limits

- ESP32-C5 PARLIO RX has **eight** data lines. Ten simultaneous lanes for
  I[9:5] and Q[9:5] cannot be obtained by selecting a 16-bit capture width.
  The 5+5-bit LUT fits memory, but its proposed direct input does not fit this
  peripheral. More pins alone do not solve this limitation.
- The BitScrambler has one shared 2048-byte LUT and eight instruction slots.
  Its RX and TX processing channels cannot run concurrently as two independent
  DSP engines. A separate free RX preprocessor cannot be assumed.
- The proven live cadence is two bundles per 50 ns output pair, raw IQ at
  40 MS/s, and 20 MS/s unique DAC codes duplicated at 40 MHz. Eight instruction
  slots allow unrolling; they do not create eight cycles per output pair.
- A LUT result arrives in the next bundle. A full raw IQ pair has 65536 states;
  a direct 16-bit pair-index discriminator cannot fit this LUT.

Primary references: [C5 capability header, IDF v6.0.2](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/include/soc/soc_caps.h#L308)
and [C5 datasheet, PARLIO and BitScrambler sections](https://documentation.espressif.com/esp32-c5_datasheet_en.html).
The repository's hardware-proven transport takes precedence where a generic
datasheet description does not capture the tested routing configuration.

## Where additional range can come from

Three distinct improvements must be measured separately:

1. **RF sensitivity:** improve carrier/noise ratio before phase detection by
   preserving useful front-end gain, reducing effective noise bandwidth, or
   improving the RF path's noise figure/loss.
2. **Quantization efficiency:** retain more of the phase information already
   present at the ADC instead of collapsing it into four centre cells.
3. **Demodulation threshold:** extract useful video under a poorer input C/N
   through a better estimator, while preserving video bandwidth and sync.

For a receiver, sensitivity depends on thermal noise bandwidth, noise figure
and required demodulation SNR. Late gain cannot recover a worse RF C/N.
See [Analog Devices sensitivity analysis](https://www.analog.com/en/resources/technical-articles/improving-receiver-sensitivity-with-external-lna.html).
The +12 compensation-field setting has not demonstrated a 12 dB improvement
in any of these three quantities.

### RF gain and current main

The reconstructed gain table in [arc-receive-chain.md](../../docs/arc-receive-chain.md)
has separate RF, BB and fine codes. G61 and higher normally share RF code 127;
raising G62 to G83 predominantly adds later gain. Runtime-generated tables
must be inspected before assuming exact stage boundaries on a particular boot.

Two concrete V5 findings at the pinned main revision:

- `direct_gain_v3.c:252`, `emergency_drop()`, searches for a lower RF stage
  **before** reducing BB gain within the current RF stage. A late-stage
  overload can therefore trigger a front-end gain reduction. Whether that
  reduction worsens C/N depends on the actual RF stage's noise/linearity;
  coarse clipping alone cannot identify where compression happened.
- `direct_gain_v3.c:51` flags any I or Q cell -8 or +7 as clipping. These are
  outer Q4 cells, not evidence that the physical ADC hit its limit. Positive
  cell +7 includes codes 448..511; most of this interval is not the ADC rail.

The former G62/no-carrier ceiling is already corrected on current main
(PR #126). Re-fixing it is not a new range improvement. V5's settle early
return also precedes its saturation-handling branch; urgent recovery is not
unconditionally immediate throughout settling.

For native AGC, these V5 functions do not own gain. Native pacing addresses
gain transitions; target radius, front-end sensitivity and ADC efficiency
remain separate questions.

### Bandwidth

Halving actual equivalent noise bandwidth gives a theoretical 3.01 dB thermal
noise reduction, **if the wanted FM spectrum is preserved**. This is not a
claim that selecting BW20 gives 3 dB on C5VRX. The tap's position relative to
the affected filters and the analog filter response remain uncharacterized.

For FM, occupied bandwidth depends on both deviation and maximum modulation
frequency. The useful video includes chroma and sync; VTX carrier offset must
also fit. A narrow setting that loses these components fails the quality goal.
[Espressif's 20/40 MHz modes](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/api-guides/wifi-driver/wifi-mac-protocols.html)
establish available Wi-Fi modes, not analog-FPV sensitivity or tap bandwidth.

`phy_set_rx_sense()` changes detection thresholds; the binary analysis does
not establish a low-noise amplifier gain improvement. FFT scaling has likewise
not established a pre-tap benefit. Neither is a justified "extra dB" knob.

## IQ lanes: preserve sign and choose the usable window

| Lane set per axis | Code spacing | Contiguous valid signed window | Main limitation |
| --- | ---: | --- | --- |
| {9,8,7,6} | 64 | -512..511 | coarse near-origin phase |
| {9,7,6,5} | 32 | -256..255 | magnitude folds outside this window |
| {9,6,5,4} | 16 | -128..127 | still more vulnerable to folding |

Sign-preserving fine lanes retain the quadrant outside their valid window,
but repeated magnitude codes no longer identify the original vector. For
example, +288 becomes +32 in the fine representation. This is different from
physical ADC clipping. Native radius around 135 raw codes also shows why
ultrafine is not automatically safe even during a stable acquisition.

A finer window reduces quantization error without adding RF gain. The best
window is the finest one that contains the relevant axis peaks at the chosen
gain, including noise and reacquisition excursions. A universal ultrafine
default has not established that condition. Observations collected through
folded lanes cannot by themselves reconstruct the missing high bits.

Potential lane selection should therefore use a coarse reference, actual
gain-transition behaviour and hysteresis. Automatic live switching remains
an unproven candidate: register transitions and observer interpretation need
their own continuity proof. Sample-ring boundaries are not reset points.

An asymmetric 5+3 split is not free extra resolution. For equal independent
I/Q quantization error, moving one bit from Q to I changes the approximate
sum of error variances from 2 to 0.25+4, a factor 2.125 worse. Such a split
requires measured anisotropy to justify it.

## Formula candidates within two bundles

### Baseline: endpoint phase difference

Full Phase8 performs `wrap(phi[c] - phi[p])` across 50 ns. Increasing phase
precision beyond what the Cartesian input contains cannot recover lost IQ
information. For independent equal I/Q noise, angle detection is a strong
baseline, rather than an obviously poor formula to replace.

Without a wrapping event, endpoint detection equals the sum of the two
adjacent 25 ns differences. It also retains the useful two-sample factor in
`1-z^-2 = (1-z^-1)(1+z^-1)`. Replacing it with a noisier single adjacent
sample is not intrinsically a range improvement.

The exact adjacent sum, without wrapping the sum a second time, uses the
middle sample to resolve winding. Previous two-bundle compressed trajectory
implementations introduced visible approximation noise. See
[trajectory-v2.md](../../docs/trajectory-v2.md),
[issue-17-true40-cadence-and-interleaved-phase5.md](../../docs/issue-17-true40-cadence-and-interleaved-phase5.md)
and [phase5-360-two-bundle-investigation.md](../../docs/phase5-360-two-bundle-investigation.md).
Their negative results do not rule out every new design, but do rule out
promoting that same compression merely because synthetic clicks improved.

### Concrete new candidate: history-conditioned phase estimator

This is a design inference, **not a measured winner or implemented program**.
It targets Phase5 quality and uses the same two-stage lookup shape as Golden:

1. Decoder address: current raw IQ byte (8 bits) plus the previously retained
   phase state's quadrant (2 bits). Exactly 1024 addresses.
2. Decoder result: a 5-bit estimated current phase state `F(raw, quadrant)`.
   With reliable IQ, use the original Golden phase state. With ambiguous IQ,
   evaluate an estimator conditioned on the previous quadrant and the input
   noise/amplitude model, rather than always taking the cell-centre angle.
3. Output address: previous 5-bit phase plus estimated current 5-bit phase.
   Exactly 1024 addresses. Its DAC mapping can remain the Golden pair mapping.

The one 1024x16 LUT can hold both functions: the decoder uses bits 8..12 of
each word, while the DAC function uses bits 0..5. Their addresses may overlap
without their fields conflicting. Golden already uses this field sharing.
The output-history register retains the estimated phase for the next pair.
No extra per-sample multiply, CPU loop or lookup is required by this layout.

Proposed steady-state schedule:

| Bundle | Result available | Lookup issued | Other work |
| --- | --- | --- | --- |
| phase | history-conditioned current phase | previous/current phase pair | retain current phase |
| emit | calibrated DAC code | next raw IQ + retained quadrant | read16, write16 duplicated DAC |

This establishes a resource layout, not assembled or silicon-verified timing.
The existing `fm.bsasm` needs its address bits 24..25 routed from the retained
quadrant for the decoder stage; the pair address still uses all ten bits.
No code change is made here.

Why it might help: Phase5's static decoder treats a near-origin sample without
using any previous direction. The additional two address bits let the decoder
use limited temporal information at the same throughput. The full raw byte
also supplies an amplitude/confidence class without separate radius logic.

Why it might fail: the previous quadrant is only 90-degree information, not
an exact phase or frequency predictor. Its uncertainty remains large. A
strong prior can flatten chroma, bias luminance or mishandle CFO and multipath.
Estimated phase errors also propagate into the next state. Reliable-IQ decode
identity does not by itself prove clean whole-stream output identity.

The estimator must be derived against full-band composite-video trajectories,
not trained to imitate a noisy endpoint output or one capture. Conditional
phase likelihoods must account for quantizer-cell regions, amplitude/noise,
carrier offset and phase movement. Do not impose the older document's
universal 124-degree cutoff: total rotation includes CFO and the actual VTX.
No universal cutoff has been measured for all permitted inputs.

This is the strongest identified **new two-bundle formula candidate** to
evaluate first. It must beat both Phase5 and Full Phase8 at matched RF input;
it has not yet established that it will extend range.

### Full PLL / tracking demodulator

A tracking demodulator can in principle improve FM threshold behaviour.
However, a true loop needs phase prediction, frequency/loop state and an
appropriate bandwidth. No feasible two-bundle implementation with the
required composite-video response has been established here. The repo's
`range_demod_bench.py` PLL is an offline model, not a live implementation.

[NASA's FM threshold-extension research](https://ntrs.nasa.gov/api/citations/19720014482/downloads/19720014482.pdf)
demonstrates threshold improvement from signal-dependent processing in its
own experimental systems. Its numerical gains cannot be assigned to C5VRX:
those signals, bandwidths and detectors differ, and some methods trade detail
for impulse rejection. A stopped/held DAC sample is not equivalent to a
validated tracking demodulator.

## Clipping: mild overdrive is different from severe loss

Common positive amplitude scaling leaves ideal phase unchanged. Independent
I/Q limiting does not: for an ideal circle hard-limited separately on each
axis, amplitude/axis-limit ratios 1.1, 1.25, 1.5 and 2 yield maximum phase
errors of approximately 0.90, 3.15, 7.39 and 15.25 degrees. These follow from
`arg(clip(R*cos(phi)) + j*clip(R*sin(phi))) - phi` over a complete rotation.
Dynamic RF compression, DC shifts and gain transients are not included.

An earlier exploratory model used main's exact Phase8 LUT, 40 MS/s IQ,
20 MS/s endpoint DAC, fixed RF C/N, independent Gaussian I/Q noise and eight
flat FM frequencies from 0 to 5.7 MHz. Illustrative PAL chroma-band noise
used 4.43361875 MHz +/-0.6 MHz and 57 kHz/IRE. At C/N=20 dB:

| Radius in coarse cells | Outer-cell occupancy | Actual ADC-limit exceedance | Worst dark chroma-band noise, model IRE rms |
| ---: | ---: | ---: | ---: |
| 2.5 | 0% | 0% | 3.52 |
| 4.0 | 0% | 0% | 2.66 |
| 5.5 | approximately 0% | approximately 0% | 2.38 |
| 6.5 | 4.22% | 0.01% | 2.15 |
| 7.5 | 44.50% | 5.63% | 2.15 |
| 9.0 | 86.77% | 60.36% | 2.74 |

At C/N=12 dB, the same noise metric was 5.47 at radius 2.5, 4.97 at 4.0
and 4.80 at 5.5: benefit diminishes as RF noise dominates. Seed 74, 32768
samples per tone; tones 0, .285, .570, .855, 1.140, 1.710, 2.850, 5.700 MHz.
This is exploratory numerical evidence, not a camera simulation, board test,
range measurement or a recommended universal radius. It omits multipath,
analog nonlinearity, gain transients, real noise filtering and video dynamics.

The robust inference is that the outer-cell flag greatly overstates actual
ADC-limit exceedance; useful headroom may be rejected prematurely. Neither
this model nor Phase8's amplitude insensitivity justifies unlimited clipping.

## Recommended decision order

1. Preserve the paced-native improvement as the RF-control comparison state.
   Establish weak/strong reacquisition before selecting a long hold period.
2. Determine which signed IQ window gives the best picture near the usable
   range edge. Do not assume that the finest lanes or largest radius wins.
3. Evaluate the history-conditioned estimator above against Phase5 and
   Phase8 using independent full-band video/RF cases. It is a new mathematical
   candidate with a specific two-bundle storage layout, not a filter added
   after a defective signal path.
4. Characterize effective BW20/BW40 noise and wanted-signal response. A
   genuine bandwidth reduction preserving video is the most direct identified
   firmware-controlled route to better RF C/N, if the selected filter affects
   this tap. That applicability is still unproven.
5. In a separate V5 comparison, distinguish rail risk from actual overload
   and RF compression from BB/ADC overdrive before dropping the RF stage.
   Do not apply this software gain policy silently to native mode.
6. If the calibrated RF C/N floor is then the limit, investigate antenna/feed
   loss, matching and front-end noise figure. LUT changes cannot restore RF
   information that never reached the ADC.

Retire Phase8 when a replacement sustains the same hardware cadence and
achieves additional controlled RF attenuation at the same Phase5-level
colour, detail, sync stability and fade recovery. Picture smoothing at the
same RF limit is a different result. No additional sensitivity in dB, range
in metres or final winning demodulator has yet been demonstrated.

## Expanded scope: replacing the pipeline

The operator subsequently rejected the limited estimator as an adequate
answer and allowed re-engineering the whole pipeline for range. The
two-bundle shape is no longer the boundary of the design search. The following
are proposals only; no firmware, RTL, wiring or board setting was changed.

### Clock and engine audit

The official [C5 technical reference manual](https://www.espressif.com/sites/default/files/documentation/esp32-c5_technical_reference_manual_en.pdf)
downloaded for this review identifies itself as Version 1.1. Chapter 9,
Table 9.2-2, says AHB frequency cannot exceed crystal frequency. Chapter 44
describes processing per DMA clock and the shared BitScrambler engine.
CPU 240 MHz is therefore not a supported 240 MHz DSP clock for this path.
There is no documented multiplier providing six bundles per 50 ns pair.
Changing the output geometry can nevertheless increase bundles per *unique*
video value without violating the bus rate.

### C5-only candidate: three bundles per three output bytes

At a 40 MHz bus/output clock, a three-bundle loop can consume three IQ bytes
and emit three DAC bytes each 75 ns. Reads of 16+8 and writes of 8+16 bits
use supported widths. The unique-video rate is 13.333 MS/s with `[D,D,D]`
holds, rather than 20 MS/s `[D,D]`. Nyquist is 6.667 MHz.

A specific layout to investigate is a calibrated Phase6 span-75 detector.
Six-bit phase arithmetic can occupy Counter A bits 10..15, leaving the
ten-bit LUT address independent of those six phase bits. A 256-word IQ
decoder and separate 64-word DAC map then fit without four decoder replicas.

| Bundle | Arithmetic / lookup | Stream work |
| --- | --- | --- |
| 1 | Load retained negative phase above counter bit 9; address current IQ decoder; retain previous DAC lookup result | read16, write8 |
| 2 | Add biased current phase above bit 9; retain its negative phase outside DAC history bits | read8, write16 duplicating retained DAC |
| 3 | Address DAC map from updated six-bit delta; loop control | no read/write |

This schedule has one period of pipeline latency. Counter low bits must be
bounded against carry into phase arithmetic. Lookup latency, source endpoint
position, output history and 8/16-bit emission order need a dataflow proof.
No assembled or hardware-verified program is claimed.

One six-bit delta code represents about 208.33 kHz, versus 312.5 kHz per
final DAC code in current full-span Phase8. The extra stage permits a separate
calibrated frequency-to-DAC transfer. These are representation properties,
not RF sensitivity or guaranteed noise improvement.

Trade-offs: span-75 wraps at +/-6.667 MHz, reducing CFO/deviation headroom.
It still discards middle samples and is not exact adjacent FM. Its small-signal
averaging factor is `|1+2*cos(2*pi*f/40MHz)|/3`, about -1.46 dB at PAL chroma
versus approximately -0.54 dB for the current two-sample factor. This is not
a steep video anti-alias filter. Rate reduction alone proves no range benefit.

This is a substantive firmware-only candidate, but it does not establish
sufficient arithmetic for a complete wideband tracking receiver.

### Decode actual bit regions rather than false Cartesian coordinates

A noncontiguous lane set represents a set of possible ADC intervals. For
approximately constant-envelope FM, a joint I/Q code plus radius/noise model
can sometimes resolve those intervals using ring geometry. A LUT can estimate
phase directly from the possible regions instead of assuming each nibble is
one ordinary signed coordinate. This is another numerical candidate, not a
selected lane map.

Folding is not automatically invertible: different phases can have identical
codes, particularly near axes and folding radii. Noise, multipath and native
radius spread weaken the ring prior. Choosing sign plus alternative magnitude
bits therefore requires a held-out comparison across radii and C/N. It must
not be justified by one perfect-circle simulation.

### Wider architecture: C5 RF front end plus external DSP logic

The GPIO diagnostic bus does not share PARLIO's eight-line input limit.
Aligned captures establish Q[4:9] and I[4:9] as actual ADC bits. Twelve lanes
would provide six signed bits per axis over the full ADC range: four times
the resolution of Q4/I4, without FINE's reduced window. Individual bit proofs
do not yet prove simultaneous twelve-lane timing.

Moving DAC driving to external logic frees the six current DAC pins, allowing
the XIAO pin budget to be reassigned for more IQ lanes and a source clock.
Pin accessibility, clock export, skew and electrical timing remain gates.
40 MS/s is the initial target. 80 MS/s requires a separate clock/timing proof;
asynchronous CPU snapshots cannot supply either continuous stream.

Proposed architecture:

```text
C5 RF / ADC / native gain selection
 -> source-synchronous I6 + Q6
 -> complex channel filter and measured DC/IQ correction
 -> adjacent-FM / tracking-demodulator comparison
 -> matched de-emphasis and video anti-alias filter
 -> rate reduction and DAC calibration
 -> continuous DAC + reconstruction filter
```

External FPGA logic removes the shared 2 KiB LUT and two-bundle restriction.
[Lattice UltraPlus](https://www.latticesemi.com/Products/FPGAandCPLD/iCE40UltraPlus)
is a capability example, not a selected part or a synthesis/timing result.
Filters and the tracking loop need resource/throughput budgets before device
selection. The C5 stays the RF/control front end; sample processing and video
output move to deterministic external logic.

This enables rejecting noise before angle detection and implementing a real
tracking detector with controlled bandwidth. A complex channel filter must
preserve the **FM sidebands**, not just the 6 MHz video baseband. It cannot
reverse aliasing that already occurred before this tap. Twelve-lane capture
improves quantization, not RF noise figure. No numerical range gain is proven.

Receiver diversity is a further option, but coherent combination needs
independent receivers, clock/CFO alignment and channel estimation. Raw IQ
streams with different phases cannot simply be averaged.

### Documentation assumptions that need correction

Comparing [static-reduction-and-filtering.md](../../docs/static-reduction-and-filtering.md)
with primary references exposes several unsupported premises:

- Upstream filtering exists, but its placement relative to MODEM_DIAG is
  unresolved. Definite absence of pre-tap anti-alias filtering is not proved.
- Without winding, endpoint differentiation retains a boxcar factor; it is
  not simply unfiltered decimation, although it is not a full anti-alias filter.
- [ITU-R F.405-1](https://www.itu.int/dms_pubrec/itu-r/rec/f/R-REC-F.405-1-197007-W!!PDF-E.pdf)
  specifies a particular television emphasis curve. It does not prove our VTX
  uses that curve. A shunt capacitor is not automatically a matched network.
- Reconstruction pole calculations must include the six driven DAC branches
  as source impedances. Ideal 8200/3900/2000/1000/470/240-ohm branches have
  parallel resistance about 122.36 ohms. Including the 200-ohm shunt and
  75-ohm display gives about 37.73 ohms. Thus 470 pF yields approximately
  8.98 MHz, not the cited 6.2 MHz. GPIO impedance, wiring and termination
  further change the physical result.

Matched transmitter/receiver emphasis can recover the intended waveform with
less output noise. Arbitrary low-pass smoothing can lose colour/detail.
These must not be treated as the same engineering operation.

### Updated recommendation

Do not promote the previous quadrant estimator as the final answer. For the
existing board, investigate three-bundle/rate geometry and verified pre-tap
filter behaviour; code-region-aware lane decoding is another numerical
candidate. For a complete architectural redesign, twelve-lane external DSP
is the clearest identified route around both input resolution and instruction
limits. It requires new hardware and remains a design hypothesis.

The decision criterion remains additional controlled RF attenuation at equal
full-band Phase5 quality and fade recovery. This review identifies architectures
and incorrect assumptions; it does not establish a final winner. No new
implementation or hardware experiment was performed in this expanded review.

