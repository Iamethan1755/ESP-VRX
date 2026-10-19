#!/usr/bin/env python3
"""Generate and verify the alternating middle/endpoint relative-mux probe.

The probe runs four controller/worker pairs across all eight C5 instruction
slots. Controller 0/2 sets A=0; worker 0/2 selects the middle sample from
FIFO bits 0..7 via relative mux 0+a..7+a. Controller 1/3 sets A=8; worker 1/3
selects the endpoint sample from FIFO bits 8..15 via 0+a..7+a.

In each controller bundle, L0..4 holds the Phase5 result of the raw byte
addressed by the previous worker. The controller latches this into O26..30.
In each worker bundle, O26..30 is emitted as duplicated output bytes
[Phase5, Phase5], consuming two bytes (16 bits) and writing two bytes (16 bits)
per 50 ns pair.
"""

from pathlib import Path
import importlib.util
import random


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "main/fm.bsasm"
TARGET = ROOT / "main/bs_relative_middle_probe.bsasm"
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
    head_lines = []
    for line in head.splitlines():
        if line.startswith("cfg trailing_bytes"):
            head_lines.append("cfg trailing_bytes 10")
        else:
            head_lines.append(line)
    asm = "\n".join(head_lines) + "\nlut " + " ".join(map(str, words)) + "\n\n"

    for i in range(4):
        a_val = 0 if (i % 2 == 0) else 8
        adda_val = 0 if (i % 2 == 0) else -8
        asm += f"""controller_{i}:
    # L0..4 = exact Phase5 of previous worker's selected raw byte.
    # Latch into O26..30; set A={a_val} for upcoming worker_{i}.
    set 26..30 L0..L4,
    ldctda {a_val}

worker_{i}:
    # A={a_val}: routes {'middle byte (0..7)' if a_val == 0 else 'endpoint byte (8..15)'} into LUT address (out[23:16]).
    # Emits previous Phase5 from O26..30 to duplicated output [Phase5, Phase5].
    set 0..4 O26..O30,
    set 8..12 O26..O30,
    set 16 0+a,
    set 17 1+a,
    set 18 2+a,
    set 19 3+a,
    set 20 4+a,
    set 21 5+a,
    set 22 6+a,
    set 23 7+a,
    read 16,
    write 16,
    adda {adda_val}

"""
    return asm, phase, words


def simulate(asm, raw, count, initial=(0, 0, 0)):
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
        if opcode[0] == "ldctda":
            a = int(opcode[1]) & 65535
        elif opcode[0] == "adda":
            a = (a + int(opcode[1])) & 65535
        else:
            raise AssertionError(opcode)
        out = new
        look = lut[(out >> 16) & 1023]
        pos += read // 8
        result.extend((out >> (8 * j)) & 255
                      for j in range(write // 8))
        pc = (pc + 1) & 7
    return result[:count]


def verify(asm, phase):
    # 1. Deterministic probe vector (matches C implementation)
    s_input = bytearray(256)
    for pair in range(128):
        s_input[2 * pair] = (pair * 37 + 5) & 255
        s_input[2 * pair + 1] = (pair * 13 + 0x43) & 255

    s_output = simulate(asm, bytes(s_input), 256, initial=(0, 0, 0))
    for pair in range(1, 128):
        src = pair - 1
        expected = phase[s_input[2 * src]] if (src % 2 == 0) else phase[s_input[2 * src + 1]]
        assert s_output[2 * pair] == expected
        assert s_output[2 * pair + 1] == expected

    # 2. 256 random streams of 512 bytes each
    for seed in range(256):
        rng = random.Random(seed)
        raw = bytes(rng.randrange(256) for _ in range(512))
        out = simulate(asm, raw, 512, initial=(rng.getrandbits(32), 0, rng.getrandbits(16)))
        for pair in range(1, 256):
            src = pair - 1
            expected = phase[raw[2 * src]] if (src % 2 == 0) else phase[raw[2 * src + 1]]
            assert out[2 * pair] == expected, f"seed={seed} pair={pair}"
            assert out[2 * pair + 1] == expected, f"seed={seed} pair={pair}"


def main():
    asm, phase, words = build()
    verify(asm, phase)
    TARGET.write_text(asm, encoding="utf-8")
    print("PASS: relative middle-sample probe generated and verified; 0 mismatches across 256 streams")


if __name__ == "__main__":
    main()
