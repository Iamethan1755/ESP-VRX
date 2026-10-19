#!/usr/bin/env python3
"""Generate a full signed-range adjacent Phase8 live demodulator."""

from pathlib import Path
import re

from gen_phase8_hr import build as build_oracle, phase8


ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/fm_phase8_hr_live.bsasm"
BIAS = 128
MULT = 1


def build(dac_bits: int = 6) -> str:
    if dac_bits not in (6, 8):
        raise ValueError("DAC width must be 6 or 8")
    source = build_oracle()
    match = re.search(r"(?m)^lut (.*)$", source)
    assert match is not None
    words = []
    for _bank in range(4):
        for raw in range(256):
            p = phase8(raw)
            minus = (BIAS - MULT * p) & 255
            plus = (MULT * p) & 255
            words.append(minus | (plus << 8))
    source = source[:match.start()] + "lut " + " ".join(map(str, words)) + source[match.end():]
    source = source.replace("# Phase8-HR arithmetic oracle only: unsafe large-delta wrap.",
                            "# Full signed Phase8 delta mapped across all 64 DAC levels.")
    source = source.replace("cfg trailing_bytes 10", "cfg trailing_bytes 0")
    source = source.replace("80 + 3*(current-previous)",
                            "128 + (current-previous)")
    if dac_bits == 8:
        # Preserve the complete counter result instead of discarding A8..A9.
        # Same bundles, LUT, read/write widths and continuous boundary state.
        source = source.replace("all 64 DAC levels", "all 256 DAC levels")
        source = source.replace("set 0..5 A10..A15", "set 0..7 A8..A15")
        source = source.replace("set 8..13 A10..A15", "set 8..15 A8..A15")
    return source


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
    target8 = TARGET.with_name("fm_phase8_8bit.bsasm")
    target8.write_text(build(8), encoding="utf-8")
    print(f"wrote {target8}")
