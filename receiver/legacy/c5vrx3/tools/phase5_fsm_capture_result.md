# Phase5 FSM capture: live result

This branch starts at `origin/main` commit `7de579f`. Its alternate TX
BitScrambler program keeps the exact Golden Phase5 DAC lookup and duplicates
the result as `[D,D]` every 50 ns. Four copies of the two steady-state bundles
fill the eight instruction slots. The final slot falls through to slot zero,
as already observed in the no-JMP hardware probe.

Bundle A stores the current endpoint Phase5 code in counter A. Bundle B stores
both full Q4/I4 bytes of the current pair in counter B. The LUT high five bits
are extended across all 1024 addresses so the low two address bits inherited
from the middle byte cannot corrupt endpoint phase lookup. Its low six DAC
bits remain byte-for-byte identical to Golden.

Validation: the generator's host model compared 256 random 512-byte streams
against Golden after the startup pipeline and checked the counter-B raw pair.
The ESP-IDF build and `tools/validate_build.py` passed. The firmware was
flashed on COM10. With the VTX on A1, the user reported clean Golden-like
video. The receiver reported `tx_empty=0`, `bs_empty=0` and `lag=1` in a
serial snapshot.

This is **capture only**. No middle phase lookup, adjacent sum, or winding
correction is performed. The two counter opcodes are both occupied by state
capture. Relative mux addressing can change which raw byte is selected, but
it does not add a third LUT lookup or another opcode in the same 50 ns.

An exhaustive direct-bit check of Phase5 triplets confirms why a cheap middle
hint is incomplete. The best one raw-bit selection yields no unconditionally
safe nonzero winding cell. The best two-bit selection yields 256 safe nonzero
cells out of 4096 endpoint/hint cells, leaving many ambiguous cells. That
check does not rule out a more elaborate counter/comparator construction.
