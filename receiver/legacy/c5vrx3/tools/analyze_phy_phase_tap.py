#!/usr/bin/env python3
"""Rank MODEM_DIAG five-lane phase candidates from a boot-probe serial log.

Asynchronous GPIO sampling and separate batches do not establish sample-wise
identity with raw IQ. This tool only identifies lane groups worth validating
with a source-clocked PARLIO capture and a controlled RF stimulus.
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

from sim_phase5_360 import PHASE5_TABLE

TRACE_RE = re.compile(
    r"PHY_TAP config=(\d+) lanes=(IQ_REF/)?(\d+)\.\.(\d+) "
    r"samples=(\d+).*? hex=([0-9a-fA-F]+)"
)


def summarize(values: list[int]) -> tuple[int, float, float]:
    counts = [0] * 32
    for value in values:
        counts[value] += 1
    entropy = -sum((count / len(values)) * math.log2(count / len(values))
                   for count in counts if count)
    delta = [abs(((b - a + 16) & 31) - 16)
             for a, b in zip(values, values[1:])]
    return sum(bool(c) for c in counts), entropy, sum(delta) / len(delta)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="serial log with PHY_TAP lines")
    args = parser.parse_args()
    captures: dict[int, dict[tuple[int, bool], list[int]]] = {}
    for line in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = TRACE_RE.search(line)
        if match is None:
            continue
        config, reference, first, last, count, encoded = match.groups()
        values = list(bytes.fromhex(encoded))
        if int(last) - int(first) != 7 or len(values) != int(count):
            raise ValueError(f"incomplete or malformed trace: {line[:95]}")
        captures.setdefault(int(config), {})[(int(first), bool(reference))] = values

    if not captures:
        parser.error("no complete PHY_TAP hex traces found")
    for config, traces in sorted(captures.items()):
        reference = traces.get((0, True))
        if reference is None:
            print(f"selector={config}: missing IQ reference; skipping")
            continue
        ref_unique, ref_entropy, ref_step = summarize(
            [PHASE5_TABLE[raw] for raw in reference]
        )
        ranked = []
        for (first, is_reference), trace in traces.items():
            if is_reference:
                continue
            for offset in range(4):
                group = [(raw >> offset) & 31 for raw in trace]
                unique, entropy, step = summarize(group)
                if unique < 16:
                    continue
                # This is an ordering heuristic, not a phase-correlation test.
                distance = abs(entropy - ref_entropy) + abs(step - ref_step) / 8
                ranked.append((distance, first + offset, unique, entropy, step))
        print(f"selector={config} reference unique={ref_unique} "
              f"entropy={ref_entropy:.2f} step={ref_step:.2f}")
        for _, first, unique, entropy, step in sorted(ranked)[:8]:
            print(f"  lanes={first}..{first + 4} unique={unique:2d} "
                  f"entropy={entropy:.2f} step={step:.2f} "
                  "status=CANDIDATE_ONLY")
        if not ranked:
            print("  no 5-bit lane group with >=16 observed values")
    print("CPU sampling is asynchronous; validate any candidate with "
          "source-clocked PARLIO before changing the demodulator.")


if __name__ == "__main__":
    main()
