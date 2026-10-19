# Phase5-360 in the current two-bundle C5 data path

This is an investigation of the **live raw Q4/I4 to DAC path**, based on
`main/fm.bsasm` at `c078d409` and the ESP32-C5 BitScrambler assembler. It does
not establish that no other two-bundle program exists.

## Exact target

For every 25 ns Phase5 triplet `P,M,C`, produce the adjacent result
`wrap32(M-P)+wrap32(C-M)` using the raw middle and current IQ samples, while
retaining the 50 ns output cadence. The mathematical winding predicate is
already exhaustive in `tools/prove_golden360_capacity.py`; its one-bit result
must be **computed** from the middle sample and both endpoints.

## Why placing the predicate into the current Phase5 LUT is insufficient

| Bundle | Existing lookup address | Existing result |
| --- | --- | --- |
| `emit` | raw current Q4/I4, 8 bits | current Phase5, 5 bits |
| `address_delta` | previous Phase5 + current Phase5, 10 bits | calibrated Golden DAC, 6 bits |

The actual order in the loop is `address_delta -> emit`, with LUT results
available on the following bundle. Both lookups are used once per 50 ns pair.
The first lookup's address has room for two additional direct raw-middle bits,
but cannot independently decode both complete raw middle and raw endpoint
bytes (8+8 bits). The second lookup is indexed by the full 10-bit endpoint
pair and cannot add a winding bit in LUT16 mode (11 address bits are needed).

Capturing all eight middle raw bits in counter B is already implemented in
`main/fm_phase5_fsm_capture.bsasm`; capture alone supplies no extra LUT
evaluation or arithmetic opcode. `tools/search_middle_bit_winding.py` verifies
that one or several *fixed* raw bits do not replace the endpoint-conditioned
winding predicate for all input triples.

## Counter experiment: decode M and C separately

One lookup per 25 ns instant can decode Phase5(M) and Phase5(C). This removes
the Golden DAC lookup, so the DAC would have to be formed with the counters
and mux sources. ESP32-C5 `ADDCTI` adds **one** value formed on the output bus
to a counter per instruction bundle. A two-step adjacent discriminator needs
the signed differences `M-P` and `C-M`, their wrap decisions, their sum, and
the P20/G2 DAC transfer. The available C bundle cannot directly supply
`C-M` as one `ADDCTI` operand: `C` has only just come from the raw-C LUT and
`M` is held in separate state; the mux selects source bits but does not
subtract two source words. This counter arrangement therefore does not
produce an exact live DAC output in two bundles. It is a limitation of this
particular counter arrangement, not a global impossibility proof.

## Moving middle preprocessing to another C5 hardware block

The ESP32-C5 SoC capability list includes GDMA, ETM, PCNT, I2S, SPI, AES,
SHA, PARLIO and one BitScrambler. The BitScrambler is the only identified
programmable per-sample LUT in the existing 40 MS/s IQ-to-DAC chain. Its RX
and TX channels share one half-duplex engine and cannot perform an independent
concurrent RX preprocessing pass while TX drives the DAC.

| Candidate | Hardware operation | Missing for exact `h(P,M,C)` |
| --- | --- | --- |
| A second BS RX pass | programmable per-byte lookup | shares the active half-duplex BS engine |
| GDMA / PARLIO | autonomous transfer / bit-lane capture | address-dependent IQ lookup or comparison |
| ETM | event-to-task routing | parallel 8-bit sample computation |
| PCNT | edge/quad counting | all 32 Phase5 positions and arbitrary 25 ns shortest arcs |
| AES / SHA | fixed cryptographic block transform | configurable per-sample phase lookup with endpoint state |
| Wi-Fi PHY debug tap | internal modem observations | no validated post-angle or adjacent-FM lane yet |

The last row is a concrete **hardware experiment**, not a claimed feature.
`legacy/c5vrx2/main/diagnostics.c` already sweeps four bounded diagnostic
configurations across all 32 MODEM_DIAG lanes; the current production mapping
selects Q4/I4 lanes 6..9 and 16..19. Extend that bounded diagnostic with a
known RF stimulus and simultaneous IQ reference or reproducible stimulus;
correlate each candidate lane set with Phase5(raw), and verify all five-bit
phases including wrap boundaries. If an independent Phase5-like tap exists
and can be captured at the right clock, the current decode LUT may be freed
for middle preprocessing. The existing diagnostic has **not** found or
validated such a tap.

An opt-in boot probe in `main/phy_phase_tap_probe.c` ports those four bounded
selector states into the current application and records raw debug-lane traces
before restoring the IQ routing. See `tools/phy_phase_tap_probe.md` and
`tools/analyze_phy_phase_tap.py`. CPU snapshots are asynchronous; they only
shortlist candidates. A selected bus still needs source-clocked 40 MS/s
validation, and no phase tap or live winding correction is claimed yet.

## Gate for a live firmware change

Do not replace `main/fm.bsasm` or label an experiment "true Phase5-360" until
a candidate has all of the following: two assembled steady-state bundles,
raw middle and endpoint input at 40 MS/s, continuous state across DMA rings,
an actual 6-bit DAC write each 50 ns, and an independent comparison of all
32,768 Phase5 triplets against `wrap32(M-P)+wrap32(C-M)`. An FPGA ROM or a
predecoded-Phase5 oracle alone does not pass this gate.
