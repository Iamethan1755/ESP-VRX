#!/usr/bin/env python3
"""Generate a full signed-range adjacent Phase8 live demodulator."""

from pathlib import Path
import re

from gen_phase8_hr import build as build_oracle, phase8


ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/fm_phase8_hr_live.bsasm"
BIAS = 128
MULT = 1


def build() -> str:
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
    return source


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
