# Range / demod quality v2

This branch separates **RF sensitivity**, **demodulation threshold**, and
**visible CVBS quality**.  A larger Q4/I4 cloud is not automatically a better
picture.

## What changes in this PR

The proven hardware transport remains unchanged:

```text
MODEM_DIAG Q4/I4 @ 40 MS/s
 -> 16 KiB cyclic ring
 -> existing Phase5 endpoint BitScrambler
 -> 20 MS/s unique CVBS
```

No CPU task is inserted into the live sample path and the production
BitScrambler program is not replaced in this PR.

The 20 Hz supervisory observer now evaluates the already-completed 4092-byte
RX descriptor with two extra forms of evidence.

### 1. Exact-adjacent winding observer

For every endpoint interval used by the production 20 MS/s lane, with the skipped 40 MS/s middle sample restored as `a,b,c`:

```text
adjacent = wrap(b-a) + wrap(c-b)
endpoint = wrap(c-a)
```

The adjacent sum is deliberately **not wrapped again**.  If it differs from
the endpoint delta, the present 50 ns endpoint demodulator has discarded one
full winding.  This is the exact failure mechanism described in Issue #23.

The observer reports:

- `winding_pm`: winding disagreements per thousand **production-parity** endpoint triplets;
- `strong_winding_pm`: the same measurement only when all three raw IQ
  powers are >=64.

It is shadow-only: it never delays or modifies CVBS.

### 2. Semantic sync quality

A low Phase5 DAC region alone is no longer enough to call a window good video.
The observer now scores both:

- H-sync pulse width (target about 94 samples at 20 MS/s);
- repeated line period near NTSC (~1271 samples) or PAL (1280 samples).

A random low pulse can score at most 40/100. A perfect line-period match alone scores at most 60/100. A fresh sync requires >=70, so both a repeated physically plausible line period and credible H-sync width must contribute.

## Range-controller change

RANGE EXP still freezes PHY writes once reception is genuinely clean.  During
a settled gain trial its score now also penalizes endpoint winding loss.

This addresses an important failure mode:

```text
more gain
 -> bigger Q4/I4 amplitude
 -> more amplified noise / low-confidence phase
 -> valid-looking power metric
 -> visibly snowier CVBS
```

A gain state with clean semantic sync and low winding ambiguity is preferred
over a state that is merely larger.

When semantic video is stable, has normal headroom, and winding remains below
the conservative experimental threshold, the controller performs zero
optimization writes.  Very high winding with otherwise valid sync may trigger
one ordinary +/-2 trial; the existing settle, rollback and exponential
backoff logic remains responsible for accepting or rejecting it.

## Why exact adjacent FM is not yet the live output

The repository already contains the mathematically correct legacy
`c5vrx2_wbfm_q4_2to1.bsasm`, but it requires many more BitScrambler bundles
per output than the current two-bundle Phase5 path.  Simply swapping it into
the monolithic realtime TX path would trade a known demodulation problem for a
throughput/starvation problem.

The next implementation gate is therefore a sustained realtime architecture
for:

```text
raw40
 -> exact adjacent FM @40M
 -> pair combine / confidence repair
 -> CVBS20
 -> simple continuous TX
```

A split/M2M route is acceptable only after it demonstrates continuous
40 MB/s input processing, 20 MB/s output production, exact state continuity
across every cyclic boundary, and no new FIFO/GDMA events.

## RF gain / PHY direction

This PR intentionally does not blindly call additional undocumented C5 PHY
symbols.  The pinned C5 PHY contains separate RF RX gain, BB gain, generated
gain-table memory, RX sense/noise-floor, channel-filter and RX DC/IQ
calibration paths.  They should be promoted one at a time only after:

1. ABI/register effect is established;
2. raw MODEM_DIAG Q4/I4 A/B proves a pre-tap effect;
3. required RF input at equivalent CVBS quality improves;
4. the call does not introduce a visible re-lock transient.

The new winding/sync metrics are intended to be the scoring layer for those
experiments.

## Hardware validation

For every gain/filter/profile state record at least:

```text
p, q
clip_pm, origin_pm
winding_pm, strong_winding_pm
sync_q, sync_width
gain/filter/ADC register snapshot
transport fault deltas
visible picture
RF attenuation / source level
```

Success means additional input attenuation at the **same visible CVBS quality**,
not merely a larger sample amplitude.
