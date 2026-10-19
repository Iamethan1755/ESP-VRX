#!/usr/bin/env python3
"""Source-driven 8-slot cadence and exhaustive raw endpoint-pair oracle."""

import importlib.util
from pathlib import Path

from gen_phase8_hr import TARGET, BIAS, build, packed_term, phase8


ROOT = Path(__file__).resolve().parents[1]
MODEL_PATH = ROOT / "tools/bs_model.py"
spec = importlib.util.spec_from_file_location("bs_model", MODEL_PATH)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def expand(token):
    if ".." not in token:
        return [token]
    first, last = token.split("..")
    prefix = first[0] if first[0].isalpha() else ""
    return [prefix + str(bit) for bit in range(int(first[len(prefix):]),
                                             int(last[len(prefix):]) + 1)]


def simulate(source, raw):
    _, lut, blocks, _ = model.parse(source)
    assert len(blocks) == 8
    out = counter = look = position = pc = 0
    outputs = []
    bundles = 0
    while len(outputs) < len(raw) // 2:
        new = 0
        read = write = 0
        opcode = None
        used = set()
        for line in blocks[pc]:
            parts = line.split()
            if parts[0] == "set":
                targets, sources = expand(parts[1]), expand(parts[2])
                if len(sources) == 1:
                    sources *= len(targets)
                assert len(targets) == len(sources)
                for target, source in zip(targets, sources):
                    bit = int(target)
                    assert bit not in used
                    used.add(bit)
                    if source == "l":
                        value = 0
                    elif source[0] in "ola":
                        value = ({"o": out, "l": look, "a": counter}[source[0]] >>
                                 int(source[1:])) & 1
                    else:
                        offset = position + int(source) // 8
                        value = (raw[offset] >> (int(source) % 8)) & 1
                    new |= value << bit
            elif parts[0] == "read":
                read = int(parts[1])
            elif parts[0] == "write":
                write = int(parts[1])
            else:
                opcode = parts[0]
        assert opcode == ("addctia" if pc % 2 == 0 else "ldctia")
        operand = (new >> 16) & 65535
        counter = (counter + operand if opcode == "addctia" else operand) & 65535
        out = new
        look = lut[(out >> 16) & 1023]
        position += read // 8
        bundles += 1
        if write:
            assert write == 16 and read == 16
            code = out & 255
            assert code < 64 and ((out >> 8) & 255) == code
            outputs.append(code)
            assert bundles == 2 * len(outputs)
        else:
            assert not read
        pc = (pc + 1) & 7
    return outputs


def de_bruijn_2(n):
    # Edge sequence of a complete directed graph, including self-loops.
    # Hierholzer yields every ordered raw pair exactly once.
    next_edge = [0] * n
    stack = [0]
    circuit = []
    while stack:
        node = stack[-1]
        if next_edge[node] == n:
            circuit.append(stack.pop())
        else:
            stack.append(next_edge[node])
            next_edge[node] += 1
    sequence = circuit[::-1]
    assert len(sequence) == n * n + 1
    return sequence


def main():
    source = build()
    assert TARGET.read_text(encoding="utf-8") == source
    _, lut, _, _ = model.parse(source)
    for bank in range(4):
        for raw in range(256):
            assert lut[bank * 256 + raw] == packed_term(raw)
    sequence = de_bruijn_2(256)
    raw = bytearray()
    for endpoint in sequence:
        raw.extend((0, endpoint))
    raw.extend(b"\0\0\0\0")
    outputs = simulate(source, raw)
    wrapped = 0
    for i in range(1, len(sequence)):
        delta = ((phase8(sequence[i]) - phase8(sequence[i - 1]) + 128) & 255) - 128
        expected = ((BIAS + 3 * delta) & 255) >> 2
        assert outputs[i + 1] == expected, (i, outputs[i + 1], expected)
        if not 0 <= BIAS + 3 * delta <= 255:
            wrapped += 1
    assert wrapped > 0
    # The finite silicon oracle must use the same output pipeline alignment.
    probe_raw = [0x70, 0x73, 0x77, 0x37, 0x07, 0x0b, 0x0f, 0x3f,
                 0x8f, 0xcf, 0xff, 0xfb, 0xf7, 0xb7, 0x87, 0x83]
    probe = [probe_raw[(pair * 7 + pair // 16) & 15] for pair in range(128)]
    stream = bytearray()
    for endpoint in probe:
        stream.extend((0, endpoint))
    stream.extend(b"\0\0\0\0")
    observed = simulate(source, stream)
    for pair in range(2, 128):
        delta = ((phase8(probe[pair - 1]) - phase8(probe[pair - 2]) + 128) & 255) - 128
        assert observed[pair] == (((BIAS + 3 * delta) & 255) >> 2)
    print(f"Phase8-HR: 65,536 raw endpoint pairs, 2 bundles/pair, duplicated DAC PASS; "
          f"unsafe 8-bit wraps={wrapped}")


if __name__ == "__main__":
    main()
