#!/usr/bin/env python3
"""Bit-exact check of main/fm_hc.bsasm against its reference model.

Runs the emitted assembly through the BitScrambler model on random raw
streams and compares every steady-state DAC pair with the reference state
recursion (decoder addressed by raw + retained quadrant, then the pair
table). Also checks the C header matches the program LUT, the two-slot
schedule, and that every decoder bank is exercised.
"""
import importlib.util
from pathlib import Path
import random
import re

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "bs_model", ROOT / "legacy/c5vrx2/tools/bs_model.py")
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)

asm = (ROOT / "main/fm_hc.bsasm").read_text(encoding="utf-8")
_, lut, blocks, labels = model.parse(asm)
assert len(blocks) == 2 and labels == {"address_delta": 0, "emit": 1}
assert len(lut) == 1024

hdr = (ROOT / "main/fm_hc_lut.h").read_text(encoding="utf-8")


def c_array(name):
    body = hdr.split(name, 1)[1].split("{", 1)[1].split("}", 1)[0]
    return [int(v) for v in re.findall(r"\d+", body)]


dec, pair = c_array("c5vrx_hc_decoder"), c_array("c5vrx_hc_pair_code")
assert all(((lut[a] >> 8) & 31) == dec[a] for a in range(1024))
assert all((lut[a] & 63) == pair[a] for a in range(1024))

banks = set()
for seed in range(256):
    rng = random.Random(seed)
    raw = bytes(rng.randrange(256) for _ in range(256))
    initial = (rng.getrandbits(32), 0, 0, rng.getrandbits(16))
    observed = model.simulate(asm, raw, 256, initial=initial)
    # Reference recursion from the hardware's first decoded endpoint. The
    # first pairs depend on the unknown initial register state.
    # The startup bank is unknown (random registers): the reference is
    # certain only after an endpoint whose decode is bank-independent.
    state, certain, checked = 0, False, 0
    for p in range(1, 128):
        endpoint = raw[2 * p - 1]
        options = {dec[(b << 8) | endpoint] for b in range(4)}
        prev, prev_certain = state, certain
        if len(options) == 1:
            state, certain = options.pop(), True
        elif certain:
            state = dec[((prev >> 3) << 8) | endpoint]
        if prev_certain and certain and p >= 3:
            banks.add(prev >> 3)
            code = pair[(prev << 5) | state]
            assert observed[2 * p:2 * p + 2] == [code, code], (seed, p)
            checked += 1
    assert checked > 100, checked
assert banks == {0, 1, 2, 3}
print("PASS: fm_hc.bsasm 2 slots, header == LUT, DAC exact vs reference "
      "recursion on 256 random streams, all 4 decoder banks used")
