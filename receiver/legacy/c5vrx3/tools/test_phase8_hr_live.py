#!/usr/bin/env python3
"""Exhaustive source-driven validation of the issue #103 live mapping."""

from gen_phase8_hr import build as build_oracle, phase8
from gen_phase8_hr_live import TARGET, BIAS, MULT, build
from test_phase8_hr import de_bruijn_2, model, simulate


def main():
    source = build()
    assert TARGET.read_text(encoding="utf-8") == source
    cfg, lut, blocks, _ = model.parse(source)
    _, _, oracle_blocks, _ = model.parse(build_oracle())
    assert len(blocks) == 8 and blocks == oracle_blocks
    assert cfg["eof_on"] == "downstream" and cfg["trailing_bytes"] == "0"
    assert all("jmp" not in str(block) for block in blocks)

    for bank in range(4):
        for raw in range(256):
            p = phase8(raw)
            expected_word = ((BIAS - MULT * p) & 255) | (((MULT * p) & 255) << 8)
            assert lut[bank * 256 + raw] == expected_word

    sequence = de_bruijn_2(256)
    stream = bytearray()
    for endpoint in sequence:
        stream.extend((0, endpoint))
    stream.extend(b"\0\0\0\0")
    observed = simulate(source, stream)
    linear = negative_wrap = positive_wrap = 0
    counts = [0] * 64
    for i in range(1, len(sequence)):
        delta = ((phase8(sequence[i]) - phase8(sequence[i - 1]) + 128) & 255) - 128
        value = BIAS + MULT * delta
        code = (value & 255) >> 2
        assert observed[i + 1] == code
        counts[code] += 1
        if value < 0:
            negative_wrap += 1
        elif value > 255:
            positive_wrap += 1
        else:
            linear += 1
            assert code == value // 4
    assert linear + negative_wrap + positive_wrap == 65536
    assert ((BIAS + MULT * -128) & 255) >> 2 == 0
    assert ((BIAS + MULT * 127) & 255) >> 2 == 63
    assert ((BIAS + MULT * -32) & 255) >> 2 == 24
    assert ((BIAS + MULT * 88) & 255) >> 2 == 54
    print(f"Phase8-HR live: all 65,536 raw pairs, 8 slots, 2 bundles/pair, "
          f"duplicated DAC PASS; linear={linear}, negative_wrap={negative_wrap}, "
          f"positive_wrap={positive_wrap}, used_DAC_codes={sum(bool(n) for n in counts)}")


if __name__ == "__main__":
    main()
