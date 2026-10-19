#!/usr/bin/env python3
"""Generate the fixed-point Q8 dB lookup used by Direct Gain V3."""

from math import log10
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/direct_gain_v3_lut.h"


def build() -> str:
    values = [round(10.0 * log10(max(power, 1)) * 256.0)
              for power in range(114)]
    rows = [", ".join(str(value) for value in values[i:i + 10])
            for i in range(0, len(values), 10)]
    return (
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "/* 10*log10(P) in Q8 dB for centered Q4 power P=0..113. */\n"
        "static const int16_t s_dg3_power_db_q8[114] = {\n    "
        + ",\n    ".join(rows) + "\n};\n"
    )


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
