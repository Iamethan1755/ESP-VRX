#!/usr/bin/env python3
"""Build an exact, conservative middle-quadrant winding oracle.

This is an offline information-capacity probe, not a runnable BitScrambler
program. A 1024x16 word stores Golden DAC6 plus four 1-bit quadrant flags and
one rail polarity bit. Runtime flag selection is a separate scheduling problem.
"""

from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "bs_model", ROOT / "legacy/c5vrx2/tools/bs_model.py"
)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)
_, golden, _, _ = model.parse((ROOT / "main/fm.bsasm").read_text())
phase = [(golden[raw] >> 8) & 31 for raw in range(256)]


def wrap32(delta):
    return ((delta + 16) & 31) - 16


def winding(previous, middle, current):
    endpoint = wrap32(current - previous)
    return (wrap32(middle - previous) + wrap32(current - middle)
            - endpoint) // 32


def quadrant(raw):
    # Packed Q4/I4: bit 3 and bit 7 are the two sign bits.
    return ((raw >> 3) & 1) | (((raw >> 7) & 1) << 1)


def build_table():
    words = []
    for p in range(32):
        for c in range(32):
            address = (p << 5) | c
            base = golden[address] & 63
            endpoint = wrap32(c - p)
            rail = 63 if endpoint < 0 else 0
            flags = 0
            for q in range(4):
                outcomes = {
                    winding(p, phase[raw], c)
                    for raw in range(256) if quadrant(raw) == q
                }
                if outcomes == {1} or outcomes == {-1}:
                    assert (outcomes == {1}) == (rail == 63)
                    flags |= 1 << q
            words.append(base | (flags << 6) | ((rail == 63) << 10))
    assert len(words) == 1024
    assert all(word < (1 << 16) for word in words)
    return words


def verify(words):
    winding_events = corrected = false_corrections = 0
    for p in range(32):
        for c in range(32):
            word = words[(p << 5) | c]
            base = golden[(p << 5) | c] & 63
            assert (word & 63) == base
            rail = 63 if (word >> 10) & 1 else 0
            for raw in range(256):
                k = winding(p, phase[raw], c)
                flag = (word >> (6 + quadrant(raw))) & 1
                output = rail if flag else base
                winding_events += k != 0
                corrected += (k != 0 and flag)
                false_corrections += (k == 0 and flag)
                if flag:
                    assert output == (63 if k == 1 else 0)
                else:
                    assert output == base
    assert false_corrections == 0
    print(f"entries={len(words)} bytes={len(words) * 2}")
    print(f"winding_events={winding_events} corrected={corrected} "
          f"coverage={100 * corrected / winding_events:.2f}% "
          f"false_corrections={false_corrections}")


if __name__ == "__main__":
    verify(build_table())
