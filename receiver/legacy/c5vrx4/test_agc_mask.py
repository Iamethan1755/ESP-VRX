#!/usr/bin/env python3
"""C5VRX by Twotoz and contributors: native AGC acquisition mask regression.

Runs the generated masked program and an unmasked reference with the same
Q3 decoder through tools/bs_model.py (source-driven dataflow model, not a
FIFO timing model) and checks, span by span:
  * no flags: identical DAC sequence to the unmasked three-bundle pipeline;
  * flagged spans hold the previous DAC value exactly;
  * every clean span after an acquisition is exact again (phase reseeded);
  * at most three bundles per three IQ bytes on every path.
"""
import importlib.util
from pathlib import Path
import random

import generate_phase8 as gen

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("bs_model", HERE / "tools/bs_model.py")
bs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bs)

SPANS = 1500


def stream(rng, flag_runs):
    """FM-like I4/Q3 samples (Q LSB = AGC flag) with given flagged runs."""
    raw, phase = [], rng.random() * 6.283
    flagged = set()
    for start, length in flag_runs:
        flagged.update(range(start, start + length))
    for n in range(SPANS * 3 + 16):
        phase += rng.uniform(-0.9, 0.9)
        radius = rng.uniform(2.0, 6.5)
        i = max(-8, min(7, round(radius * __import__('math').cos(phase) - 0.5)))
        q = max(-8, min(6, round(radius * __import__('math').sin(phase) - 0.5))) & ~1
        if n in flagged:  # acquisition: saturated or starved garbage
            i, q = rng.randrange(-8, 8), rng.randrange(-8, 8) & ~1
        raw.append(((i & 15) << 4) | (q & 15) | (1 if n in flagged else 0))
    return raw, flagged


def dac_per_span(out, first):
    """D_k for span k on the six DAC pins (bits 6/7 carry parity state):
    masked emits D_k at 3k+2, the reference at 3k+3."""
    return [out[3 * k + first] & 63 for k in range(SPANS - 2)]


def run(transfer):
    mask_text = gen.build_mask(transfer)
    ref_text = gen.build(False, False, transfer, words=gen.words_for_mask(transfer, identity=False))
    rng = random.Random(166 + len(transfer))
    count = SPANS * 3

    # 1) No flags: identical DAC sequence (masked output is one byte earlier).
    raw, _ = stream(rng, [])
    stats = {}
    masked = bs.simulate(mask_text, raw, count, stats=stats)
    ref = bs.simulate(ref_text, raw, count)
    dm, dr = dac_per_span(masked, 2), dac_per_span(ref, 3)
    assert dm[2:] == dr[2:], "unflagged masked output differs from the Q3 reference"
    for k in range(2, SPANS - 3):
        assert masked[3 * k + 3] & 63 == masked[3 * k + 4] & 63 == dm[k], "[D,D,D] broken"
    assert stats['bundles'] <= 1 + 3 * (count // 3 + 1), stats['bundles']

    # 2) Acquisition runs at every phase offset and length, incl. back-to-back.
    runs, pos = [], 30
    while pos < SPANS * 3 - 200:
        length = rng.choice([1, 2, 3, 4, 7, 24, 96, 136])
        runs.append((pos, length))
        pos += length + rng.choice([1, 2, 3, 5, 9, 40, 120, 400])
    raw, flagged = stream(rng, runs)
    stats = {}
    masked = bs.simulate(mask_text, raw, count, stats=stats)
    ref = bs.simulate(ref_text, raw, count)
    dm, dr = dac_per_span(masked, 2), dac_per_span(ref, 3)
    holds = exact = 0
    for k in range(3, SPANS - 3):
        # Span k: P=raw[3k+1], M1=3k+2, M2=3k+3, C=3k+4. Decision made at the
        # end of span k-1: flag(P_k) or flag(M1_k).
        hold = (3 * k + 1) in flagged or (3 * k + 2) in flagged
        if hold:
            assert dm[k] == dm[k - 1], f"span {k}: hold did not repeat the last DAC"
            holds += 1
        else:
            # Clean P: exact whenever C is clean too; a run starting at M2/C
            # is caught one span later (documented look-ahead limit).
            if (3 * k + 4) not in flagged and (3 * k + 3) not in flagged:
                assert dm[k] == dr[k], f"span {k}: clean span not exact after reseed"
                exact += 1
    trace = stats['trace']
    assert stats['bundles'] <= 1 + 3 * (count // 3 + 1), stats['bundles']
    hold_entry = trace.count(4)  # hold_reseed slot
    assert hold_entry == holds or abs(hold_entry - holds) <= 2, (hold_entry, holds)
    return holds, exact


def main():
    for transfer in ("std150", "cvbs150", "legacy"):
        text = gen.build_mask(transfer)
        _, lut, blocks, labels = bs.parse(text)
        assert len(blocks) == 6 and len(blocks) <= 8
        assert all(lut[768 + i] == i & 63 for i in range(256)), "identity bank 3"
        holds, exact = run(transfer)
        print(f"PASS agc_mask {transfer}: slots=6 holds={holds} exact_clean_spans={exact} "
              "unflagged=bit-exact cadence<=3 bundles/span")


if __name__ == "__main__":
    main()
