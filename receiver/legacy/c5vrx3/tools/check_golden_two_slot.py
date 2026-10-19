#!/usr/bin/env python3
"""Check Golden's two-slot startup and steady-state byte alignment."""

import importlib.util
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "legacy/c5vrx2/tools/bs_model.py"
spec = importlib.util.spec_from_file_location("bs_model", path)
model = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(model)

asm = (ROOT / "main/fm.bsasm").read_text()
_, lut, blocks, labels = model.parse(asm)
assert len(blocks) == 2
assert labels == {"address_delta": 0, "emit": 1}

for seed in range(256):
    rng = random.Random(seed)
    raw = bytes(rng.randrange(256) for _ in range(128))
    initial = (rng.getrandbits(32), 0, 0, rng.getrandbits(16))
    observed = model.simulate(asm, raw, 128, initial=initial)
    for pair in range(2, 64):
        prev = (lut[raw[2 * pair - 3]] >> 8) & 31
        curr = (lut[raw[2 * pair - 1]] >> 8) & 31
        dac = lut[(prev << 5) | curr] & 63
        assert observed[2 * pair:2 * pair + 2] == [dac, dac], (seed, pair)

print("PASS: 2 instruction slots; Golden DAC exact after 2 startup pairs")
