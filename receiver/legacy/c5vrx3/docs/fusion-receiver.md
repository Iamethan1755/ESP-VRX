# C5VRX Fusion Receiver experiment

This experiment adds a supervisory **IQ Fusion Engine** around the proven
realtime MODEM_DIAG -> PARLIO -> BitScrambler -> DAC path.

The live waveform remains hardware paced. Fusion only analyzes an already
completed 4092-byte RX descriptor every 50 ms.

## Combined evidence

The observer combines adjacent 25 ns phase deltas, lag-2 and lag-4 endpoint
disagreement, robust local phase-slope consensus, low-IQ confidence, Q_phase,
clipping, I/Q balance, winding and bounded semantic-sync validation.

Raw-IQ metrics dominate the score; sync is only a small sanity bonus.

## Contextual learner

Every observation becomes NO_CARRIER, WEAK, CLEAN, BLOCKER or OVERLOAD. A
bounded learner compares G62/G58/G54/G50/G46/G42/G38/G34 with switching cost,
settle windows and rollback.

Hard rules:

- NO_CARRIER -> G62.
- WEAK may only explore G62..G50.
- CLEAN/high-confidence -> no PHY writes.
- OVERLOAD gets immediate headroom.
- A trial must beat the baseline by a margin or roll back.

## Realtime gate

This does not pretend that CPU code can fuse multiple 40 MS/s demodulators.
Exact adjacent-FM waveform fusion remains gated by issue #23 until a
BitScrambler/M2M path proves sustained throughput and state continuity.
