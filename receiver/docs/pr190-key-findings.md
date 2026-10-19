# PRs #190 and #192: reusable receiver findings

This is a selective knowledge extraction from [PR #190](https://github.com/Twotoz/C5VRX/pull/190),
reviewed on 2026-10-11 at
[`7491a660e26722afbb9dd9e24e69ba381b4f130d`](https://github.com/Twotoz/C5VRX/commit/7491a660e26722afbb9dd9e24e69ba381b4f130d).
It preserves findings from **C5VRX by Twotoz and the C5VRX contributors**;
the [official website](https://twotoz.github.io/C5VRX/) also provides the
Discord invite. The burst investigation additionally credits
[zerowidth/C5VRX PR #3](https://github.com/zerowidth/C5VRX/pull/3).

Extraction base: main `a165e19db47ebd49051217d7c968a1b8220c20fb`.
PR #190 targets `codex/c5vrx4-pll-lab`, not main. Its inherited V4 integration,
firmware, generated LUTs, search outputs, tools and changing defaults are not
imported here. The source artifacts remain accessible at the pinned revision.
This document does not change production behavior or establish measured RF range.

The sequence-reference follow-up [PR #192](https://github.com/Twotoz/C5VRX/pull/192)
is reviewed separately at
[`c758f05afeb7a953fbc4d915906f046f13566841`](https://github.com/Twotoz/C5VRX/commit/c758f05afeb7a953fbc4d915906f046f13566841).
It is stacked on an earlier #190 revision (`49b9269`), not the latest U85Q
work. Its reference decoder and capture/data artifacts are also not imported.
See [other recent PR findings](recent-pr-receiver-findings.md) for complementary
DC/gain, build-identity, AFC and direct-IQ timing evidence.

## Read the evidence at the right level

- **Synthetic:** model results, including modeled goggle lock, are hypotheses
  about receiver behavior. C/N in those models is not calibrated RF sensitivity.
- **Captured-IQ replay:** compares algorithms on the same real IQ; it does not
  measure the live DAC, transport continuity or actual goggle acceptance.
- **Board observation:** a log, fault, operator report or bench measurement
  applies to the recorded board/setup. It is not an across-VTX flight result.
- **Source/build proof:** compiler equivalence and assembler/IDF success prove
  a narrower claim than sustained hardware timing or usable video.

The studies are chronological ledgers. Later sections retract earlier conclusions;
use their corrections rather than combining numbers from incompatible screens.

## 1. Fix the comparison before optimizing the demodulator

The PAIR/APEX research found two time-alignment errors. The source executor
emitted a span's output one span later than its software teacher. Subsequently,
one OVP56-derived calibration lag was used for algorithms with different group
delays. Merely delaying the scoring truth raised PAIR's apparent SINAD by about
3 dB and EDGE's by 0.8 dB. The claimed roughly 6 dB teacher headroom and CVT
"smoothing gain" were withdrawn or superseded.

The later amplitude/particle/belief studies use the stronger protocol: determine
each model's lag on its clean counterpart, then freeze it for noisy comparisons.
Use identical hashed raw IQ, a fixed voltage scale and independent seeds. Scoring
truth must never be a runtime input to the estimator. A delay-only change should
not create an algorithmic gain. The intermediate per-noisy-decode alignment
search in the PAIR study is not the preferred confirmation protocol.

Measure sync misses **and false sync**, full-field H/V timing, moving-picture
detail, large errors/clicks, carrier-outage recovery and goggle lock. Earlier
short, centred, fitted-carrier snippets missed whole-picture failures that the
later full-field model exposed. Preserve failed gates rather than changing them
after seeing a candidate's result.

Sources: [PAIR corrections](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PAIR_RANGE_STUDY.md),
[belief-ROM protocol](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/BELIEF_ROM_STUDY.md).

## 2. The useful hardware budget is information plus cadence

The shared-word V4 schedule acquires raw Q4/I4 at 40 MS/s into a 32 KiB cyclic
ring. It makes 20 million distinct DAC6 decisions/s, duplicated as `[D,D]` at
physical TX40. Its eight slots execute two bundles/two lookups per 50 ns using
one 1024 x 16-bit LUT (2 KiB), with TX-only BitScrambler and persistent state.
These are the studied schedule's constraints, not a new production contract.

The first lookup compresses a ten-bit observation address into a token; the
second combines token and state to emit DAC6 and the next state. State and
observation compete for the same 1024 transitions: 256x4, 128x8, 64x16,
32x32 and 16x64 allocations were investigated. More reliability or history
bits have a resolution cost; there is no free extra LUT or third lookup.

PAIR4411 retains all eight bits of sample A plus I/Q sign bits of sample B.
It uses both acquired samples, but **does not retain both full IQ bytes** or
establish exact adjacent-FM/winding recovery. Its initial fine-lane confirmation
improved weak SINAD from 15.875 to 18.018 dB against matched RANGE32, while both
first passed nominal reception at 10 dB on the tested 2 dB grid. Those synthetic
results did not establish board range or universality; later off-centre/deviation
screens exposed tracker failures.

Counter/history proposals must retain their actual clock assumptions:
counter-difference prototypes require RX40/TX20 with one output byte per span,
so are incompatible with the duplicated TX40 transport. A later causal
`prefetch=false` input-history schedule fits TX40, but its selected decoder
fails quality gates. Assembler legality alone settles neither question.

Sources: [PAIR schedule/results](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PAIR_RANGE_STUDY.md),
[counter clock audit](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/AMPLITUDE_COUNTER_STUDY.md),
[TX40 history schedule](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/BELIEF_ROM_STUDY.md).

## 3. Check frontend and controller faults before blaming the LUT

| Finding in the PR | Reusable consequence | Evidence boundary |
| --- | --- | --- |
| Local IDF 6.0.1 libphy `694c5df5` failed the lab's binary verification, disabling DCO/filter/11p controls; the tested 6.0.2 libphy was `dbf33c41` | Record the actual linked PHY identity and feature-refusal logs, not just an SDK version | Specific lab binary gate; not a blanket verdict on every 6.0.1 build |
| A carrier's uneven angular occupancy biased the plain I/Q mean; circle-centre fitting recovered DC in the host example | Distinguish carrier statistics from hardware DC and validate any circle fit over weak/noisy signals | Host fit plus board DCO observations; not universal estimator accuracy |
| Boot/profile overrides silently forced AFC off after a range-mode AUTO selection | Verify the final active controller state after all restore/profile steps | Branch-specific source/log fault; do not assume main has it |
| The Rician envelope C/N meter reported -7.9 dB on a coherent strong board carrier because ultrafine lanes folded | Test quality meters over lane folding, DC, interference and gain transitions before using them to select a demod | Board fault; the replacement phase-second-difference meter still needs calibrated RF validation |
| Logs showed DCO_AUTO searching during usable weak video and repeated V5 gain writes | Absence of semantic sync or a noise-like envelope alone cannot authorize disruptive recalibration under live video | Board-log finding; gain steps and DCO hold interruptions are separate effects |

The branch replaced the envelope selector with a phase-domain estimate and
rejects observations crossing gain changes. Its later DCO guard remembers a
carrier at the current frequency and postpones automatic quiet searches until
a retune/reboot, while retaining cached per-gain codes. This is an investigated
guard with a recalibration trade-off, not a proven general carrier detector.

Brief interference saturation also needs separate treatment from sustained
overload. The zerowidth follow-up's host regression held a weak carrier at G83
with zero gain writes when 0.1-0.5 ms bursts were ignored until 1 ms of
saturation; sustained overload still triggered reduction. The old-board
no-VTX log (4,530 writes in 72 minutes) is consistent with hunting but does
not prove burst causation. Do not copy a different receiver's G77 ceiling:
this board reported table maximum 83 and no clipping at G83 without a VTX.

Sources: [frontend and burst investigation](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PAIR_RANGE_STUDY.md),
[picture-cliff logs](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PICTURE_CLIFF_STUDY.md),
[DCO guard source](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/main/video_dco.c).

## 4. Better nominal scores can still lose the whole picture

Full-field synthetic tests randomized VTX deviation, carrier offset, DC/IQ
error, content, fades and gain steps. PAIR/RANGE32 could lose lock even at
20-25 dB C/N; EDGE favored weak sync but sacrificed strong detail. Modeled
goggle behavior added another failure surface beyond raw luma SINAD.

The later closed-form U85 family avoids fitted LUT entries, but formula and
parameter selection still use search/confirmation data. U85E erased near-origin
cells and looked strong on the nominal screen. Independent echo/fade tests
showed those cells can retain quadrant information: erasure could hold the
phase too long and lose sync. U85Q instead uses four origin-quadrant tokens
with a lower update gain and a wider -6..+9 MHz output window. It trades nominal
sharpness for robustness; it is not a proven universal optimum.

The latest real-video report contains 12 IQ snapshots at A1/5865 MHz with
U85Q, narrow bandwidth and the DCO guard active. Its histogram estimates sync
at -5.0 MHz and white at +6.25 MHz. U85E's -3.5..+7.5 MHz window excludes that
sync tip; U85Q's includes it. Host replay on those captures reports 83.0, 85.7
and 101.5 surrogate clicks per 1000 samples for U85Q, U85E and HC50 respectively.
This is useful single-VTX evidence for keeping sync-side headroom, not a live
three-mode goggle A/B or measured range acceptance.

Predetection filtering is another useful lead: the synthetic U85E bandwidth
sweep changed modeled lock at 2 dB from 0.66 near 20 MHz to 0.95 at 14 MHz.
Narrowing further hurt high-deviation detail; 10 MHz broke deviation 1.4.
The board's existing edge profile had measured noise-equivalent bandwidth
15.359 MHz (reported roughly 1.3 dB lower noise). The branch chose a permanent
narrow profile rather than a switching gear. Any adoption needs actual
passband, chroma/group-delay, multi-VTX and goggle checks; bandwidth numbers
and synthetic C/N are not interchangeable sensitivity measurements.

Sources: [picture-cliff and U85 progression](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PICTURE_CLIFF_STUDY.md),
[12-capture report](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/data/board_validate/report-u85q-2026-10-11.json).

## 5. Avoid controller-induced output gaps and SRAM regressions

FusionDemod reloads stop/reset the engine. The studies report 3.8-5.1 ms
halts, roughly 60-80 video lines; one uncached software-synthesis reload took
335 ms. Caching reduced the pause but did not establish seamless switching.
A 50 ms supervisor cannot resolve a sub-ms fade. Measure DAC/FIFO continuity,
sync loss, rollback and real-goggle reacquisition before calling program swaps
or a sequence of output-table reloads smooth fusion. Readback success alone
does not prove safe LUT arbitration while the engine runs.

The latest commit also records a concrete memory failure: AutoFit/Fusion caches
left about 21.5 KiB internal SRAM while menu DMA allocations needed about
23.5 KiB. Removing unused/flash-duplicated table copies and sharing stopped-servo
self-test scratch recovered 6 KiB. The commit reports menu open/close and live
video resumption on the board. Budget DMA-capable internal memory across menu
and live ownership; firmware linking is not proof that later allocations fit.

Sources: [swap measurements and corrections](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/PAIR_RANGE_STUDY.md),
[menu SRAM fix](https://github.com/Twotoz/C5VRX/commit/7491a660e26722afbb9dd9e24e69ba381b4f130d).

## 6. Preserve the negative results, without promoting their firmware

| Experiment | Disposition worth retaining |
| --- | --- |
| PAIR's early nominal search, 75 ns cascade, 25 ns single-lookup, direct LUT polish | Offset/strong-detail or holdout failures; short-screen leadership was insufficient |
| APEX/CVT output innovation and smoothing | Detail/click trade-off; some apparent gains disappeared after latency correction; older pre-DCO capture vetoes also need context |
| Omega belief-state LUT | Combined weak-sync/strong-detail gates failed; operator reported poor high-amplitude video; value13 was quarantined in the branch |
| Counter-difference/history and quality variants | Legal source/arithmetic proofs, but wrong clock geometry for TX40 and failed weak-sync gates; no selectable replacement |
| Causal amplitude/noise-aware particle reference | Useful offline IQ-only inference; some synthetic weak-sync gains, but no dominance on every metric and no live C5 implementation |
| Particle/FSM distillation and belief ROMs | Tested state/token allocations and all 13 selected belief-ROM tables failed combined gates; some sequence optimization selected epoch0 unchanged |
| TX40 causal input-history schedule | Legal alternative representation and corrected startup model; selected decoder still failed picture/sync gates |

These results reject the tested methods and candidates, not every possible
finite-state estimator or legal C5 schedule. Stronger floating references see
both complete IQ bytes and carry far more state than the hardware student.
They are diagnostic controls, not deployable LUTs or a proof of global
optimality/impossibility. Existing production and physical baselines remain
the comparison points for any future port.

Sources: [Omega verdict](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/OMEGA_BELIEF_STUDY.md),
[amplitude/counter failures](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/AMPLITUDE_COUNTER_STUDY.md),
[particle reference/compression](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/ADAPTIVE_PARTICLE_STUDY.md),
[belief-ROM failures and validation manifests](https://github.com/Twotoz/C5VRX/blob/7491a660e26722afbb9dd9e24e69ba381b4f130d/v4/docs/BELIEF_ROM_STUDY.md).

## 7. PR #192: receiver-aware inference is useful, but still not a winner

The offline sequence reference retains alternative phase/frequency trajectories,
receiver-filter memory and inferred amplitude/noise regimes. Its likelihood
integrates all analog intervals mapping to a folded ADC code, including rail
tails, rather than assigning one signed-cell centre. This provides a useful
experiment to separate observation loss from estimator/model limitations.
The synthetic filter ordering is not a verified physical tap model; conditional
AR(1) noise matches only the first correlation, and Gaussian moment compression
is approximate. A richer model alone does not guarantee better recovery.

The optional eight-raw-sample (200 ns) ancestry estimate uses observations through
the declared availability time. It is genuine fixed-lag inference, not merely
shifting a causal output. Missing startup/tail samples stay unavailable, without
injected sync or old-line copies. Extra latency does not create extra hardware
lookups. At 512 hypotheses the persistent arrays alone occupy about 116 KiB;
this is not a current-LUT implementation or a demonstrated realtime CPU path.

On independent synthetic confirmation at 6 dB, the delayed reference scores
7.51 dB waveform SINAD with zero missed/false H-sync, against PAIR's 7.15 dB
and three misses. However, active-picture RMSE is worse (23.44 vs 20.89 IRE)
and detail correlation lower (.714 vs .733). At 30 dB its SINAD drops to 10.26
against PAIR's 15.62; the existing particle reference is better at 2 dB. Tested
PAL transient pulse recovery improves, but aggregate stressed picture quality
does not beat PAIR. No overall replacement qualifies.

The full-IQ versus PAIR4411-bit ablation isolates discarded observation bits;
more bits help some cases but do not remove finite-inference/model losses.
Increasing hypotheses from 512 to 2,048 improves a development result without
reaching the comparator. This is budget sensitivity, not an optimality bound.
One independent nominal clean record freezes calibration and each decoder's lag
across confirmation records; its absolute scores cannot be mixed with earlier
per-record calibration results.

Four archived physical fixtures contain only 8,190 bytes / 204.75 us each,
predate later DCO changes and have no known-video truth. Checksummed replay
proves reproducibility, not a picture gain. Keep snapshots separate across gaps:
concatenating them or relaxing copy guards cannot establish continuous dropout
recovery. Short records without a vertical interval cannot prove V-sync, and
three-pulse reacquisition is not actual goggle lock. The reported exact-head
IDF CI builds existing live demodulators, not the offline estimator.

Source: [sequence-reference study, results and reproduction](https://github.com/Twotoz/C5VRX/blob/c758f05afeb7a953fbc4d915906f046f13566841/v4/docs/IQ_SEQUENCE_REFERENCE.md).

## Next evidence that would justify a separate implementation PR

First measure frontend bandwidth and controller interruptions on the actual
board. Then compare qualifying algorithms on identical DCO-corrected IQ with
frozen clean-derived timing, including unseen VTXs/content and interference.
Only a candidate passing weak-sync, false-sync, strong-detail and amplitude
gates should proceed to assembler/source equivalence, sustained transport and
real moving PAL/NTSC/goggle A/B with controlled RF attenuation. Preserve raw
IQ hashes, gain/DCO actions, DAC voltages, outages and recovery times.

No new measured range, universally better hardware demodulator or seamless
live-switching result is established by this extraction.
