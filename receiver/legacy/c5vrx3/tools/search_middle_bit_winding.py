#!/usr/bin/env python3
"""Bound safe winding coverage when Golden's pair LUT gets one middle bit."""

from collections import defaultdict
from itertools import combinations
from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "bs_model", ROOT / "legacy/c5vrx2/tools/bs_model.py"
)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)
_, lut, _, _ = model.parse((ROOT / "main/fm.bsasm").read_text())
phase = [(lut[raw] >> 8) & 31 for raw in range(256)]


def wrap(delta):
    return ((delta + 16) & 31) - 16


def winding(previous, middle, current):
    return (wrap(middle - previous) + wrap(current - middle)
            - wrap(current - previous)) // 32


def main():
    print("bits,nonzero_events,safe_corrections,coverage_percent")
    for bits in (selected for width in (1, 2, 3)
                 for selected in combinations(range(8), width)):
        groups = defaultdict(set)
        for p in range(32):
            for c in range(32):
                for raw in range(256):
                    key = (p, c, tuple((raw >> bit) & 1 for bit in bits))
                    groups[key].add(winding(p, phase[raw], c))

        total = safe = 0
        for p in range(32):
            for c in range(32):
                for raw in range(256):
                    k = winding(p, phase[raw], c)
                    if k:
                        total += 1
                        key = (p, c, tuple((raw >> bit) & 1 for bit in bits))
                        if groups[key] == {k}:
                            safe += 1
        print(f"{''.join(map(str, bits))},{total},{safe},"
              f"{100 * safe / total:.3f}")


if __name__ == "__main__":
    main()
