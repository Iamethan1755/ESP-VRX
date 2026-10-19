# Unified CVT hardware feasibility

C5VRX by Twotoz and the C5VRX contributors. This extends the existing U85Q
and counter-LP research; it does not establish a new unrestricted DSP path.

The current schedule has two bundles per 50 ns input pair:

| Bundle | LUT operation | Counter operation in fixed LP | Output |
| --- | --- | --- | --- |
| Controller | Raw I4/Q4 plus two signs -> token | LDCTIA: load half previous y and address bits | previous y |
| Worker | Five-bit phase + five-bit token -> next phase and x | ADDCTIAH: add half x | repeat previous y |

The 1024-entry LUT is addressed with ten bits. Phase+token already consumes
all ten. Each LUT16 result retains the low six DAC bits, five phase-state
bits and five token bits. Controller and worker share the table, so their
two interpretations must remain valid simultaneously. Counter A holds the
six-bit post-filter output, with its lower bits also carrying address and
rounding information. Counter B is not an independently routable substitute
in this LUT16 geometry. These constraints are implemented in `u85q_lp.py`
and checked by the ESP-IDF parser and the instruction emulator.

An immutable nonlinear phase law fits: the worker table precomputes the
next phase and DAC for each phase/token pair. A confidence token costs angle
resolution. The 32-token alternatives keep angular resolution but infer
only from phase error; they have no independent amplitude confidence. This
changes phase-tracking bandwidth, without an extra bundle or program switch.

An exact `y += alpha * (x-y)` needs a different operation: both terms must
be weighted according to an observation-dependent alpha on **every** span.
The current fixed mux loads y/2 and the second fixed opcode adds x/2.
Replacing only the LUT's x with alpha*x leaves the previous-y coefficient
unchanged and is not a variable pole. Replacing the DAC code with an
adaptive phase correction is also not a post-detection IIR. Indexing the
worker table by phase, token and previous six-bit y would need 16 address
bits in a direct product (65536 entries), which this LUT does not provide.
That is a bound on that direct encoding, not a proof against all alternative
state compression, counter arithmetic or different programs.

Conditional bypass or held/reset updates have an existing negative result:
the four-span experiment produced periodic ~5 MHz artifacts and 3.72/3.10 dB
SINAD versus 5.09 dB for continuous LP at deviation 1.0, C/N 4 dB. It is not
an acceptable shortcut. Extra bundles reduce sustained input throughput;
the host or a frozen replay cannot qualify a slower program as live.

No exact variable-alpha two-bundle implementation is established here.
Further counter/state-compression designs must include a complete fixed
cadence schedule, bit-exact simulation and physical FIFO/colour checks.
Until then, CVT and CVT with a **fixed** LP are experimental alternatives.
Selection still restarts the pipeline; the running immutable CVT law itself
does not rewrite its LUT or switch programs in response to individual IQ.
