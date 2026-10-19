#!/usr/bin/env python3
"""Generate the live Phase5c endpoint Golden program.

The historical filename says 360, but both middle-sample branches currently
emit the same endpoint LUT bits. Ideal Adjacent50/360 simulations are separate
from this live two-bundle BitScrambler program; do not claim equivalence.
"""

from pathlib import Path
import re
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "main/fm.bsasm"
TARGET = ROOT / "main/fm_phase5_360.bsasm"
MODEL_PATH = ROOT / "legacy/c5vrx2/tools/bs_model.py"

spec = importlib.util.spec_from_file_location("bs_model", MODEL_PATH)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def wrap32(delta: int) -> int:
    return ((delta + 16) & 31) - 16


def winding(previous: int, middle: int, current: int) -> int:
    endpoint = wrap32(current - previous)
    return (wrap32(middle - previous) + wrap32(current - middle) - endpoint) // 32


def quadrant(raw: int) -> int:
    return ((raw >> 3) & 1) | (((raw >> 7) & 1) << 1)


def build():
    baseline = BASE.read_text(encoding="utf-8")
    _, old_lut, _, _ = model.parse(baseline)
    phase = [(old_lut[raw] >> 8) & 31 for raw in range(256)]

    # Controller reads Phase5 from bits 0..4. Both workers emit Golden DAC
    # from bits 8..13, irrespective of the retained middle sample.
    safe_360 = {}
    words = []
    for i in range(1024):
        p = (i >> 5) & 31
        c = i & 31
        delta = wrap32(c - p)
        # Phase5c (Correction & Static Squelch):
        # Twelve bins = 135 degrees over the 50 ns endpoint interval,
        # equivalent to 7.5 MHz. The threshold is empirical squelch,
        # not proof that all these transitions are impossible video.
        dac = old_lut[i] & 63
        if abs(delta) >= 12:
            safe_360[(p, c)] = dac
        words.append(phase[i & 255] | (dac << 8))

    head = baseline.split("\nlut ", 1)[0]
    asm = head + "\nlut " + " ".join(map(str, words)) + "\n\n"

    asm += """# Both workers emit L8..L13: middle-sample routing does not alter
# the live DAC value. True winding correction needs a hardware-validated path.
controller_0:
    set 26..30 L0..L4,
    set 16..20 L0..L4,
    set 21..25 O26..O30,
    set 31 L,
    if 7 worker_360

worker_golden:
    set 0..5 L8..L13,
    set 6..7 L,
    set 8..13 L8..L13,
    set 14..15 L,
    set 16..23 8..15,
    set 24..25 L,
    set 26..30 O26..O30,
    set 31 L,
    read 16,
    write 16,
    jmp controller_0

worker_360:
    set 0..5 L8..L13,
    set 6..7 L,
    set 8..13 L8..L13,
    set 14..15 L,
    set 16..23 8..15,
    set 24..25 L,
    set 26..30 O26..O30,
    set 31 L,
    read 16,
    write 16,
    jmp controller_0
"""
    return baseline, asm, old_lut, phase, words, safe_360


def main():
    print(f"Generating Phase5-360 BitScrambler program: {TARGET}")
    _, asm, _, _, words, safe_360 = build()
    TARGET.write_text(asm, encoding="utf-8")
    print(f"  Total LUT words: {len(words)}")
    print(f"  Endpoint squelch pairs: {len(safe_360)} (no live winding correction)")
    print(f"  Output written to: {TARGET}")


if __name__ == "__main__":
    main()
