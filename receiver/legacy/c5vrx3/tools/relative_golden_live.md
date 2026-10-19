# Relative Golden live oracle

This branch keeps Golden's calibrated 50 ns DAC mapping while moving Counter A
to the source-selection role. It is an opt-in 6-bit TX BitScrambler program;
the normal firmware still loads the proven capture program.

The LUT16 word is repacked: bits 0..4 contain `Phase5(address & 255)`, while
bits 8..13 contain the original Golden DAC code at the full ten-bit address.
In each controller bundle, `L0..4` supplies the endpoint Phase5 and the
previous phase remains in `O26..30`; the resulting pair address is exactly
Golden's. In `controller_0`, Counter A is initialized to 8 (`ldctda 8`).
Because `controller_k` never uses `+a`, Counter A does not need to be restored
to 0. Counter A stays permanently at $+8$ ("Static-A"), which completely
eliminates all `adda -8` from the worker bundles and all subsequent `ldctda 8`
from controllers 1..3. This frees 7 out of 8 ALU opcode slots across the
execution loop for state tracking and winding arithmetic, while maintaining
two-bundle / 50 ns execution and slot-7 fallthrough.

`python tools/gen_relative_golden.py` checks 256 random 512-byte streams
against the original Golden BitScrambler model after startup, and
checks that both the phase table and every one of the 1024 DAC entries are
byte-identical to Golden. The program is generated and built with
`CONFIG_C5VRX_RELATIVE_GOLDEN_LIVE=y` for a live test.

This Static-A version keeps Golden's endpoint DAC mapping while demonstrating
that the worker relative mux does not cost any ALU cycle per bundle. Those
freed opcode slots provide the exact compute budget needed for middle-sample
winding correction.

## Initial silicon check

The opt-in firmware was flashed to a XIAO ESP32-C5 on COM10 on 2026-09-25;
esptool verified the bootloader, partition table, and app image hashes. With
the VTX off, the firmware booted and answered the serial `d` command. It
reported A1, `PARLIO tx_empty=0`, `rx_ovf=0`, and `BS eof_ovl=0`.

With the VTX on A1, the user reported Golden-like video from this flashed
build. The live serial snapshot showed ARC V3 in LOCK, Q_phase=99%,
`PARLIO tx_empty=0`, `rx_ovf=0`, `BS eof_ovl=0`, and NTSC detection. This
establishes that relative mux routing and the repacked LUT can run in the
live two-bundle video path. It is a visual equivalence result, not a
byte-for-byte physical DAC capture. The middle sample and winding correction
remain unimplemented.
