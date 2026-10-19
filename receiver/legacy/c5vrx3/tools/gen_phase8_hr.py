#!/usr/bin/env python3
"""Generate the two-bundle Phase8 endpoint arithmetic research program.

This version has no safe large-delta guard and must not be loaded as live video.
The 8-bit weighted result wraps outside -26..+58 Phase8 bins.
"""

from pathlib import Path
import math


ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/bs_phase8_hr_probe.bsasm"
BIAS = 80  # Four times the P20 pedestal.


def phase8(raw: int) -> int:
    q_code, i_code = raw & 15, raw >> 4
    q = (q_code if q_code < 8 else q_code - 16) * 64 + 31.5
    i = (i_code if i_code < 8 else i_code - 16) * 64 + 31.5
    return round(math.atan2(q, i) * 128 / math.pi) & 255


def packed_term(raw: int) -> int:
    p = phase8(raw)
    minus = (BIAS - 3 * p) & 255
    plus = (3 * p) & 255
    return minus | (plus << 8)


def build() -> str:
    # Worker addresses the raw byte with bits 16..23. Bits 24..25 carry
    # minus(previous), so replicate the 256 raw entries in every bank.
    lut = [packed_term(raw) for _bank in range(4) for raw in range(256)]
    blocks = []
    for slot in range(4):
        blocks.append(f"""controller_{slot}:
    # L is the current raw endpoint's packed term. Keep minus in O0..O7.
    set 0..7 L0..L7,
    set 16..23 L,
    set 24..31 L8..L15,
    addctia

worker_{slot}:
    # A[15:8] = (80 + 3*(current-previous)) mod 256.
    set 0..5 A10..A15,
    set 8..13 A10..A15,
    set 16..23 8..15,
    set 24..31 O0..O7,
    read 16,
    write 16,
    ldctia
""")
    return """# Phase8-HR arithmetic oracle only: unsafe large-delta wrap.
# Two bundles per pair, 20 MS/s unique DAC, duplicated at 40 MHz.
# Full-width Counter A allows next raw LUT address in its low byte while
# loading the retained previous term into its high byte.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 10
cfg lut_width_bits 16
lut """ + " ".join(map(str, lut)) + "\n\n" + "\n".join(blocks)


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
