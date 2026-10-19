#!/usr/bin/env python3
"""Exhaustive output/cadence regression across all raw Q4/I4 endpoint pairs."""
from gen_phase8_hr_live import build, TARGET, phase8
from test_phase8_hr import simulate, de_bruijn_2, model


def main():
    source = build(8)
    assert TARGET.with_name("fm_phase8_8bit.bsasm").read_text(encoding="utf8") == source
    cfg, lut, blocks, _ = model.parse(source)
    cfg6, lut6, blocks6, _ = model.parse(build())
    assert cfg == cfg6 and lut == lut6 and len(blocks) == len(blocks6) == 8
    sequence = de_bruijn_2(256)
    raw = bytearray()
    for endpoint in sequence:
        raw.extend((0, endpoint))
    raw.extend(b"\0\0\0\0")
    result = simulate(source, raw, dac_bits=8)
    baseline = simulate(build(), raw)
    seen = set()
    for i in range(1, len(sequence)):
        expected = (128 + phase8(sequence[i]) - phase8(sequence[i-1])) & 255
        assert result[i+1] == expected
        assert result[i+1] >> 2 == baseline[i+1]
        seen.add(expected)
    assert len(sequence)-1 == 65536
    assert any(x & 3 for x in seen)  # Additional bits carry information.
    print(f"8-bit Phase8 PASS: 65,536 endpoint pairs, {len(seen)} distinct codes, "
          "duplicated bytes, unchanged two-bundle cadence; no hardware claim")


if __name__ == "__main__":
    main()
