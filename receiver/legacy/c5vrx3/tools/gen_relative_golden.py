#!/usr/bin/env python3
"""Generate a two-bundle live Golden variant using Counter-A relative mux.

The phase and FM tables share the same 1024x16 LUT: low five bits are the
Phase5 code for address.low8, and high six bits are the exact Golden DAC for
the full ten-bit pair address. Controller uses normal L0..4; worker with A=8
uses L0+a..L5+a, which physically selects L8..13. The same relative offset
routes raw FIFO bits 8..15 through 0+a..7+a.
"""

from pathlib import Path
import importlib.util
import random


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "main/fm.bsasm"
TARGET = ROOT / "main/fm_relative_golden.bsasm"
MODEL_PATH = ROOT / "legacy/c5vrx2/tools/bs_model.py"

spec = importlib.util.spec_from_file_location("bs_model", MODEL_PATH)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def build():
    baseline = BASE.read_text(encoding="utf-8")
    _, old_lut, _, _ = model.parse(baseline)
    phase = [(old_lut[raw] >> 8) & 31 for raw in range(256)]
    words = [(phase[i & 255] | ((old_lut[i] & 63) << 8))
             for i in range(1024)]
    head = baseline.split("\nlut ", 1)[0]
    asm = head + "\nlut " + " ".join(map(str, words)) + "\n\n"
    for i in range(4):
        if i == 0:
            ctrl_body = """    set 26..30 L0..L4,
    set 16..20 L0..L4,
    set 21..25 O26..O30,
    ldctda 8"""
        else:
            ctrl_body = """    set 26..30 L0..L4,
    set 16..20 L0..L4,
    set 21..25 O26..O30"""
        asm += f"""controller_{i}:
    # L0..4 = exact Phase5 of endpoint raw; O26..30 = previous phase.
    # Golden's 10-bit pair address is unchanged.
{ctrl_body}

worker_{i}:
    # Static A=8: relative mux selects L8..13 (DAC) and FIFO8..15 (endpoint).
    # Source O26..30 is outside the relative mux range and remains unchanged.
    # No counter reset needed; ALU opcodes are 100% freed for useful arithmetic!
    set 0 L0+a,
    set 1 L1+a,
    set 2 L2+a,
    set 3 L3+a,
    set 4 L4+a,
    set 5 L5+a,
    set 8 L0+a,
    set 9 L1+a,
    set 10 L2+a,
    set 11 L3+a,
    set 12 L4+a,
    set 13 L5+a,
    set 26..30 O26..O30,
    set 16 0+a,
    set 17 1+a,
    set 18 2+a,
    set 19 3+a,
    set 20 4+a,
    set 21 5+a,
    set 22 6+a,
    set 23 7+a,
    read 16,
    write 16

"""
    return baseline, asm, old_lut, phase, words


def simulate(asm, raw, count, initial):
    _, lut, blocks, _ = model.parse(asm)
    assert len(blocks) == 8
    out, a, look = initial
    pos = pc = 0
    result = []

    def expand(token):
        if ".." not in token:
            return [token]
        lo, hi = token.split("..")
        suffix = "+a" if lo.endswith("+a") else ""
        lo = lo.removesuffix("+a")
        hi = hi.removesuffix("+a")
        prefix = lo[0] if lo[0].isalpha() else ""
        return [prefix + str(i) + suffix
                for i in range(int(lo[len(prefix):]),
                               int(hi[len(prefix):]) + 1)]

    while len(result) < count:
        new = 0
        read = write = 0
        opcode = None
        for line in blocks[pc]:
            parts = line.split()
            if parts[0] == "set":
                dst, src = expand(parts[1]), expand(parts[2])
                if len(src) == 1:
                    src *= len(dst)
                assert len(dst) == len(src)
                for d, s in zip(dst, src):
                    relative = s.endswith("+a")
                    s = s.removesuffix("+a")
                    if s == "l":
                        value = 0
                    elif s.startswith("o"):
                        value = (out >> int(s[1:])) & 1
                    elif s.startswith("l"):
                        index = int(s[1:])
                        if relative:
                            index = (((48 + index + a) & 31) + 32) - 48
                        value = (look >> index) & 1
                    else:
                        bit = int(s)
                        if relative:
                            bit = (bit + a) & 31
                        ix = pos + bit // 8
                        value = ((raw[ix] >> (bit % 8)) & 1
                                 if ix < len(raw) else 0)
                    new |= value << int(d)
            elif parts[0] == "read":
                read = int(parts[1])
            elif parts[0] == "write":
                write = int(parts[1])
            else:
                opcode = parts
        if opcode:
            if opcode[0] == "ldctda":
                a = int(opcode[1]) & 65535
            elif opcode[0] == "adda":
                a = (a + int(opcode[1])) & 65535
            elif opcode[0] != "nop":
                raise AssertionError(opcode)
        out = new
        look = lut[(out >> 16) & 1023]
        pos += read // 8
        result.extend((out >> (8 * j)) & 255
                      for j in range(write // 8))
        pc = (pc + 1) & 7
    return result[:count]


def verify(baseline, asm, old_lut, phase, words):
    _, _, blocks, _ = model.parse(asm)
    assert len(blocks) == 8
    assert all((words[i] & 31) == phase[i & 255] for i in range(1024))
    assert all(((words[i] >> 8) & 63) == (old_lut[i] & 63)
               for i in range(1024))
    for seed in range(256):
        rng = random.Random(seed)
        raw = bytes(rng.randrange(256) for _ in range(512))
        initial_out = rng.getrandbits(32)
        initial_look = rng.getrandbits(16)
        golden = model.simulate(baseline, raw, 512,
                                initial=(initial_out, 0, 0, initial_look))
        actual = simulate(asm, raw, 512,
                          initial=(initial_out, rng.getrandbits(16), initial_look))
        assert actual[4:] == golden[4:], (seed, actual[:16], golden[:16])


def main():
    baseline, asm, old_lut, phase, words = build()
    verify(baseline, asm, old_lut, phase, words)
    TARGET.write_text(asm, encoding="utf-8")
    print("PASS: relative Golden is byte-exact after startup in 256 random "
          "streams; 8 slots / 2 bundles per pair")


if __name__ == "__main__":
    main()
