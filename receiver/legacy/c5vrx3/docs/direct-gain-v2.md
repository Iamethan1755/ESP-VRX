# Direct Gain V2, issue #95

Direct Gain V2 is the default RX profile. It reuses the existing 512-byte
completed-DMA observer at roughly 6 ms cadence. That task is the only
steady-state gain writer for this profile. The 4092-byte, 50 ms observer
continues to provide video diagnostics and publishes settled IQ metrics for
model learning; it makes no V2 gain decisions.

Each usable generated vendor index is decoded to its RF stage, BB code and
Fine code. The target estimator evaluates all states from G20 through the
runtime vendor maximum. It predicts Q4 power from an uncalibrated 0.82 dB per
index prior until three valid measurements of an **exact from/to edge** are
available. A normal correction accepts useful amplitude and penalizes BB and
RF boundaries, so individual Fine steps can win. Hard fades and overloads
prioritize a direct useful target in one write. No-carrier acquisition jumps
once to the known survival state. G20 is the user-selected minimum.

The model records per-state settled P/Q/origin/clip and a bounded set of
measured from/to edges with power ratio, quality change, clipping change and
an artifact proxy. It does not treat an adjacent edge as evidence for a
different direct hop. It discards clipped, carrierless and pre-write IQ from
edge learning. A gain write starts a new observation epoch; the fast observer
waits at least 12 ms for verification, while the full-window learner waits
at least 30 ms. A useful Q4 vector enters zero-write LOCK.

The prior is deliberately conservative. The vendor table is not globally
monotonic: the issue #93 close-range sweep found a clean G40 tuple between
clipped G38 and G42. The V2 candidate must be tested on the receiver before
claiming fewer visible artifacts. The present edge artifact score is an IQ
proxy and cannot measure a sub-millisecond video flash. The 12 ms and 30 ms
verification delays are initial values, not measured per-edge settle times.

Hardware A/B should use the same VTX/channel/power and demodulator for Direct
Gain V2 versus ARC V3, walking close, medium, far and back. Record gain writes,
RF/BB boundary crossings, LOCK time, P/Q/origin/clip, transport faults and
time-aligned visible static/desync. A clean locked picture should cause no
gain writes. Fine-only corrections and boundary hops should be compared
separately before tuning the route cost or settle interval.

## First live check and profile cleanup

The first PR #96 image was flashed to the XIAO ESP32-C5 on COM10 with
bootloader, partition table and application SHA verification. It booted in
profile 10, Direct Gain V2. The operator reported that the live picture
"werkt echt top" and requested V2 as the automatic default. This is positive
live video evidence, but no time-aligned gain-write/static trace was captured
with the carrier on: a later ten-row USB snapshot captured P=1/Q=0 at G62,
indicating the VTX was off or no carrier was present during that capture.

The follow-up firmware retains only Direct Gain V2, Direct Gain V1 and ARC V3
as selectable automatic gain profiles. Previously stored legacy gain choices
migrate to V2. GOLDEN is the sole selectable live demodulator; previously
stored TRAJ V2 choices migrate to GOLDEN. Historical experimental code stays
available for research, but is not reachable through the profile selector.
