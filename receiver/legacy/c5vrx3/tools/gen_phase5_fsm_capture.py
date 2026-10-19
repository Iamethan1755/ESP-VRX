#!/usr/bin/env python3
"""Pack Golden Phase5 plus lossless middle IQ capture into two live bundles.

Four A/B pairs fill all eight C5 instruction slots. Slot 7 falls through to
slot 0 on the tested C5 silicon. A's opcode stores the endpoint phase in A;
B's opcode stores both raw bytes in B. This preserves Golden DAC bytes while
making the middle byte available for a future winding decision.
"""

from pathlib import Path
import importlib.util
import random


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "main/fm.bsasm"
MODEL_PATH = ROOT / "legacy/c5vrx2/tools/bs_model.py"

spec = importlib.util.spec_from_file_location("bs_model", MODEL_PATH)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def build():
    baseline = BASE.read_text(encoding="utf-8")
    _, golden_lut, _, _ = model.parse(baseline)
    phase = [(golden_lut[i] >> 8) & 31 for i in range(256)]
    words = [((golden_lut[i] & ~(31 << 8)) |
              (phase[i & 255] << 8)) for i in range(1024)]
    for i in range(1024):
        assert (words[i] & 63) == (golden_lut[i] & 63)
        assert ((words[i] >> 8) & 31) == phase[i & 255]

    head = baseline.split("\nlut ", 1)[0]
    asm = head + "\nlut " + " ".join(map(str, words)) + "\n\n"
    for i in range(4):
        asm += f"""address_delta_{i}:
    # L is Phase5(raw endpoint); A0..4 holds the previous endpoint phase.
    # LUT address remains exactly Golden: previous5:current5.
    set 16..20 L8..L12,
    set 21..25 A0..A4,
    set 26..30 L8..L12,
    ldctial

emit_{i}:
    # Low LUT bits are Golden DAC. Address next raw endpoint while retaining
    # the *entire* first IQ byte of this pair in O24..31 / B8..15.
    set 0..5 L0..L5,
    set 6..7 L,
    set 8..13 L0..L5,
    set 14..15 L,
    set 16..23 8..15,
    set 24..31 0..7,
    read 16,
    write 16,
    ldctib

"""
    return baseline, asm, golden_lut, phase


def simulate_fsm(asm, raw, count, *, trace=False):
    """Source-bit model with C5's measured 8-slot IP wrap and two counters."""
    _, lut, blocks, _ = model.parse(asm)
    assert len(blocks) == 8
    out = a = b = look = pos = pc = 0
    result = []
    captures = []

    def expand(token):
        if ".." not in token:
            return [token]
        lo, hi = token.split("..")
        prefix = lo[0] if lo[0].isalpha() else ""
        return [prefix + str(i) for i in range(int(lo[len(prefix):]),
                                                int(hi[len(prefix):]) + 1)]

    while len(result) < count:
        new = 0
        read = write = 0
        opcode = None
        for line in blocks[pc]:
            bits = line.split()
            if bits[0] == "set":
                dst, src = expand(bits[1]), expand(bits[2])
                if len(src) == 1:
                    src *= len(dst)
                assert len(dst) == len(src)
                for d, s in zip(dst, src):
                    if s == "l":
                        value = 0
                    elif s[0] in "olab":
                        value = ({"o": out, "l": look, "a": a, "b": b}[s[0]]
                                 >> int(s[1:])) & 1
                    else:
                        bit = int(s)
                        index = pos + bit // 8
                        value = ((raw[index] >> (bit % 8)) & 1
                                 if index < len(raw) else 0)
                    new |= value << int(d)
            elif bits[0] == "read":
                read = int(bits[1])
            elif bits[0] == "write":
                write = int(bits[1])
            else:
                opcode = bits[0]
        if opcode == "ldctial":
            a = (a & 0xff00) | ((new >> 16) & 255)
        elif opcode == "ldctib":
            b = (new >> 16) & 65535
            if trace:
                captures.append((pos, b))
        else:
            raise AssertionError(opcode)
        out = new
        look = lut[(out >> 16) & 1023]
        pos += read // 8
        result.extend((out >> (8 * j)) & 255 for j in range(write // 8))
        pc = (pc + 1) & 7
    return result[:count], captures


def verify(baseline, asm, golden_lut, phase):
    _, words, blocks, labels = model.parse(asm)
    assert len(blocks) == 8
    assert labels == {name: index for index, name in enumerate(
        value for i in range(4) for value in
        (f"address_delta_{i}", f"emit_{i}"))}
    for seed in range(256):
        rng = random.Random(seed)
        raw = bytes(rng.randrange(256) for _ in range(512))
        golden = model.simulate(baseline, raw, 512)
        actual, captures = simulate_fsm(asm, raw, 512, trace=True)
        assert actual[4:] == golden[4:], (seed, actual[:24], golden[:24])
        for source_pos, packed in captures:
            if source_pos + 1 < len(raw):
                assert packed == (raw[source_pos + 1] |
                                  (raw[source_pos] << 8)), (seed, source_pos)
        assert all(((words[i] >> 8) & 31) == phase[i & 255]
                   for i in range(1024))
        assert all((words[i] & 63) == (golden_lut[i] & 63)
                   for i in range(1024))


def main():
    baseline, asm, golden_lut, phase = build()
    verify(baseline, asm, golden_lut, phase)
    target = ROOT / "main/fm_phase5_fsm_capture.bsasm"
    target.write_text(asm, encoding="utf-8")
    print("PASS: eight slots / two bundles per 50 ns; Golden DAC exact; "
          "all 16 raw IQ bits retained in B at every pair")


if __name__ == "__main__":
    main()
