# Golden360 from main: exact adjacent50 target and present C5 boundary

This branch starts from `main` and evaluates a Golden-based demodulator that
retains the 50 ns `[D,D]` output cadence. Its desired Phase5-bin operation is

```
p,m,c = phase5(raw_previous), phase5(raw_middle), phase5(raw_current)
d0 = wrap32(m-p)
d1 = wrap32(c-m)
pair = d0+d1                 # do not wrap the sum again
DAC = P20/G2(pair)
```

Each 25 ns difference is unambiguous only when the physical step is strictly
inside +/-180 degrees. The two-step sum can then preserve a +/-360-degree
50 ns trajectory that Golden's endpoint delta loses. The 6-bit DAC clips
large sums, so this is a *winding/sign* improvement, not linear +/-360-degree
video output with the existing gain.

`python tools/prove_golden360_capacity.py` exhausts the 32^3 Phase5 triples
using the production P20/G2 transfer. It finds:

- All 1,024 `(middle,current)` pairs have distinct desired DAC-continuation
  rows over the 32 possible previous phases. A first-stage token for an exact
  `(middle,current)->token; (previous,token)->DAC` LUT factorization needs
  10 bits; the final LUT would need 15 address bits, above LUT16's 10.
- Exhausting **every direct bit split** of the 15 Phase5 triplet bits also
  rules out exact two-stage Golden-preserving factorization at each hardware
  LUT width: 1,365/1,365 LUT8 (`11->token7->11`), 3,003/3,003 LUT16
  (`10->token5->10`), and 5,005/5,005 LUT32 (`9->token3->9`) splits need
  more distinct token values than their second address can carry. This gives
  the stages independent tables in the test, so it is an optimistic bound;
  real C5 stages share one physical LUT. If the exact Golden DAC curve is
  dropped, six LUT16 partitions become possible for the *clipped ideal*
  adjacent50 target. They do not satisfy this Golden-preserving request.
- All 32 middle phases have distinct winding-continuation rows over possible
  endpoint pairs. A phase summary that is independent of the endpoints must
  retain five middle-phase bits for an arbitrary-input guarantee.
- The assembled TX-only two-bundle LUT32 topology from the Phase5+ experiment
  retains exact Golden endpoint phases and only **one direct raw-middle bit**.
  For each of the eight possible raw-bit choices, zero endpoint/hint cells can
  be corrected with a nonzero winding while remaining correct for *every*
  possible middle IQ sample in that hint class. Example: `p=0,c=20` with
  `m=10` needs a +360-degree branch, while `m=20` needs Golden. Both middle
  samples can have the same raw-I sign.

These are lower bounds for the tested LUT factorization and a direct proof
against an exact one-middle-bit correction. They do not prove that *every*
possible use of C5 counters or external logic is impossible. However, the
known live TX BitScrambler budget is two bundles per 50 ns, and RX+TX
BitScrambler is half-duplex on this C5. The existing Golden program spends
one lookup on raw-to-Phase5 and one on endpoint-to-DAC. A full independent
middle-phase lookup and two adjacent deltas have not been scheduled in the
remaining hardware slots.

**No Golden360 firmware is emitted by this proof.** Enabling a model-based
correction from one middle sign would generate wrong video pulses on legal
noisy/discontinuous IQ and repeat the earlier static failure. Keep Golden as
the live baseline until a program assembles, passes exhaustive source-model
tests, and demonstrates clean live video. A parallel phase/discriminator
datapath, or proven extra per-pair lookup capacity, would make the exact
adjacent50 pipeline straightforward.
