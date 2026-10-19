#!/usr/bin/env python3
"""Generate and validate the C5VRX Trajectory v2 two-stage LUT.

Trajectory v2 keeps the live 40 MB/s data plane at two steady-state
BitScrambler bundles per 50 ns output while using both fields of the 1024x16
LUT as two different learned stages.

Stage 1 lookup address (issued while prefetching a Q4/I4 pair):
    current raw Q4/I4 byte                     8 bits
    middle raw-I sign                          1 bit
    previous Phase5 MSB                        1 bit
                                                 ----
                                                 10 bits

The LUT high/spare bits return:
    actual current Phase5                      5 bits
    learned trajectory token                   5 bits

Stage 2 lookup address:
    previous actual Phase5                     5 bits
    learned trajectory token                   5 bits
                                                 ----
                                                 10 bits

The LUT low six bits return the final DAC code.

Training uses a deterministic physical-FM prior with local slope continuity,
amplitude fades and Q4 noise.  The target is the clean two-adjacent trajectory:

    target = map_to_cvbs(d0 + d1)

where d0+d1 is DELIBERATELY NOT wrapped again.

The learned token is optimized with a small L1 Lloyd loop.  This lets clean,
high-confidence states retain Phase5-quality precision while noisy states use
the middle sample and amplitude-bearing raw-Q4 byte to infer the trajectory.

--write deterministically regenerates main/fm_traj.bsasm and
main/trajectory_v2_lut.h.  CI then requires a zero git diff.
"""
from __future__ import annotations

import argparse
import hashlib
import math
import re
import sys
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
ASM = ROOT / "main" / "fm_traj.bsasm"
HEADER = ROOT / "main" / "trajectory_v2_lut.h"
TAU = 2.0 * math.pi
TRAIN_SAMPLES = 140_000
TRAIN_ITERATIONS = 7
TRAIN_SEED = 0x314159
EXPECTED_DIGEST = "30bf7f6936e70ef91d0b5cd5d60db8c1f5675d27fcaa6855d77ed5487f5205e0"


def iround(x: float) -> int:
    return int(math.floor(x + 0.5)) if x >= 0.0 else -int(math.floor(-x + 0.5))


def s4(v: int) -> int:
    return v - 16 if v & 8 else v


def phase_rad(byte: int) -> float:
    # Centre of the discarded 6-bit ADC bucket, matching production Phase5.
    qc = byte & 0x0F
    ic = (byte >> 4) & 0x0F
    q = qc * 64.0 + 31.5
    i = ic * 64.0 + 31.5
    if q >= 512.0:
        q -= 1024.0
    if i >= 512.0:
        i -= 1024.0
    return math.atan2(q, i)


def phase5(byte: int) -> int:
    return iround(phase_rad(byte) * 32.0 / TAU) & 31


def signed_phase5_delta(previous: int, current: int) -> int:
    delta = current - previous
    if delta > 15:
        delta -= 32
    elif delta < -16:
        delta += 32
    return delta


def scale_rad(rad: float) -> int:
    # Production P20/G2 calibration.
    phase8 = iround(rad * 256.0 / TAU)
    n = phase8 * 3
    correction = -((-n + 2) // 4) if n < 0 else (n + 2) // 4
    return max(0, min(63, 20 + correction))


def stage1_address(previous_phase5: int, middle_raw: int, current_raw: int) -> int:
    return (
        current_raw
        | (((middle_raw >> 7) & 1) << 8)
        | (((previous_phase5 >> 4) & 1) << 9)
    )


def stage2_address(previous_phase5: int, token: int) -> int:
    return previous_phase5 | ((token & 31) << 5)


def _xorshift32(state: int) -> tuple[int, float]:
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    state ^= (state << 5) & 0xFFFFFFFF
    state &= 0xFFFFFFFF
    return state, state / 4294967296.0


def _q4_round(x: float) -> int:
    # Matches the deterministic canonical trainer (JS-style nearest integer).
    return math.floor(x + 0.5)


def regenerate() -> tuple[list[int], list[int], list[int], list[int]]:
    phases5 = [phase5(b) for b in range(256)]
    groups: list[list[tuple[int, int]]] = [[] for _ in range(1024)]

    state = TRAIN_SEED
    spare: float | None = None

    def uniform() -> float:
        nonlocal state
        state, value = _xorshift32(state)
        return value

    def gaussian() -> float:
        nonlocal spare
        if spare is not None:
            value, spare = spare, None
            return value
        u = max(1e-12, uniform())
        v = uniform()
        radius = math.sqrt(-2.0 * math.log(u))
        angle = TAU * v
        spare = radius * math.sin(angle)
        return radius * math.cos(angle)

    def quant(phi: float, amplitude: float, sigma: float) -> int:
        i = amplitude * math.cos(phi) + sigma * gaussian()
        q = amplitude * math.sin(phi) + sigma * gaussian()
        ii = max(-8, min(7, _q4_round(i))) & 0x0F
        qq = max(-8, min(7, _q4_round(q))) & 0x0F
        return (ii << 4) | qq

    for _ in range(TRAIN_SAMPLES):
        phi0 = (uniform() * 2.0 - 1.0) * math.pi

        # Mostly smooth local WBFM motion, with bounded wider excursions.
        if uniform() < 0.18:
            slope = (uniform() * 2.0 - 1.0) * 1.10
        else:
            slope = max(-1.15, min(1.15, 0.48 * gaussian()))
        accel = max(-0.35, min(0.35, 0.10 * gaussian()))
        d0 = max(-1.20, min(1.20, slope - 0.5 * accel))
        d1 = max(-1.20, min(1.20, slope + 0.5 * accel))

        amplitude = 1.3 + uniform() * 5.7
        if uniform() < 0.16:
            amplitude = 0.4 + uniform() * 2.1
        sigma = 0.05 + uniform() * 1.05

        previous_raw = quant(phi0, amplitude, sigma)
        middle_raw = quant(phi0 + d0, amplitude, sigma)
        current_raw = quant(phi0 + d0 + d1, amplitude, sigma)
        previous = phases5[previous_raw]
        address = stage1_address(previous, middle_raw, current_raw)
        groups[address].append((previous, scale_rad(d0 + d1)))

    # The token starts phase-like, then learns a compact trajectory state.
    token = [phases5[address & 0xFF] for address in range(1024)]
    stage2 = [[20 for _ in range(32)] for _ in range(32)]

    def refit_stage2() -> None:
        cells: list[list[list[int]]] = [
            [[] for _ in range(32)] for _ in range(32)
        ]
        for address, samples in enumerate(groups):
            tok = token[address]
            for previous, target in samples:
                cells[previous][tok].append(target)

        for previous in range(32):
            for tok in range(32):
                values = cells[previous][tok]
                if values:
                    values.sort()
                    stage2[previous][tok] = values[len(values) // 2]
                else:
                    # Safe deterministic fallback for unvisited cells.
                    delta = signed_phase5_delta(previous, tok)
                    stage2[previous][tok] = scale_rad(delta * TAU / 32.0)

    for _ in range(TRAIN_ITERATIONS):
        refit_stage2()
        changed = 0
        for address, samples in enumerate(groups):
            if not samples:
                continue
            best_token = token[address]
            best_loss: int | None = None
            for candidate in range(32):
                loss = sum(
                    abs(stage2[previous][candidate] - target)
                    for previous, target in samples
                )
                if best_loss is None or loss < best_loss:
                    best_loss = loss
                    best_token = candidate
            if best_token != token[address]:
                token[address] = best_token
                changed += 1
        if changed == 0:
            break

    # The last token update must also be reflected in the final stage-2 table.
    refit_stage2()

    confidence: list[int] = []
    for address, samples in enumerate(groups):
        if not samples:
            confidence.append(0)
            continue
        tok = token[address]
        mae = sum(
            abs(stage2[previous][tok] - target)
            for previous, target in samples
        ) / len(samples)
        confidence.append(
            max(0, min(255, iround(255.0 * (1.0 - min(mae, 24.0) / 24.0))))
        )

    dac = [0] * 1024
    words = [0] * 1024
    for address in range(1024):
        previous = address & 31
        tok = (address >> 5) & 31
        dac[address] = stage2[previous][tok]

        # Same physical 16-bit word serves both lookup stages:
        #   low 0..5: stage-2 DAC
        #   6,7,13..15: stage-1 learned token
        #   8..12: stage-1 actual current Phase5
        stage1_phase = phases5[address & 0xFF]
        stage1_token = token[address]
        word = dac[address]
        word |= (stage1_token & 0x03) << 6
        word |= stage1_phase << 8
        word |= ((stage1_token >> 2) & 0x07) << 13
        words[address] = word

    return words, dac, token, confidence


def parse_lut_words(text: str) -> list[int]:
    match = re.search(r"^lut (.*)$", text, re.MULTILINE)
    if not match:
        raise AssertionError("fm_traj.bsasm has no embedded LUT")
    return [int(v) for v in match.group(1).split()]


def parse_header_array(text: str, name: str) -> list[int]:
    match = re.search(
        rf"static const uint8_t {re.escape(name)}\[1024\]\s*=\s*\{{(.*?)\}};",
        text,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing {name}[1024]")
    return [int(v) for v in re.findall(r"\d+", match.group(1))]


def instruction_blocks(text: str) -> dict[str, list[str]]:
    blocks: dict[str, list[str]] = {}
    current: list[str] | None = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip().rstrip(",")
        if not line or line.startswith(("cfg ", "lut ")):
            continue
        if line.endswith(":"):
            current = []
            blocks[line[:-1]] = current
        elif current is not None:
            current.append(line)
    return blocks


def self_test() -> None:
    asm = ASM.read_text()
    header = HEADER.read_text()
    words = parse_lut_words(asm)
    dac = parse_header_array(header, "c5vrx_trajectory_v2_dac")
    token = parse_header_array(header, "c5vrx_trajectory_v2_token")
    confidence = parse_header_array(header, "c5vrx_trajectory_v2_confidence")

    assert len(words) == len(dac) == len(token) == len(confidence) == 1024
    assert all(0 <= value <= 63 for value in dac)
    assert all(0 <= value <= 31 for value in token)
    assert all(0 <= value <= 255 for value in confidence)
    assert len(set(dac)) == 64

    for address, word in enumerate(words):
        assert (word & 0x3F) == dac[address]
        assert ((word >> 8) & 31) == phase5(address & 0xFF)
        packed_token = (
            ((word >> 6) & 0x03)
            | (((word >> 13) & 0x07) << 2)
        )
        assert packed_token == token[address]

    blocks = instruction_blocks(asm)
    assert set(blocks) == {"prime_lookup", "prime_store", "trajectory", "emit"}
    assert blocks["prime_lookup"][-1] == "read 16"
    assert blocks["prime_store"][-1] == "read 16"
    assert not any(
        op.startswith(("read ", "write ", "jmp "))
        for op in blocks["trajectory"]
    )
    assert blocks["emit"][-3:] == ["read 16", "write 16", "jmp trajectory"]
    assert "cfg eof_on downstream" in asm
    assert "cfg trailing_bytes 0" in asm
    assert "cfg lut_width_bits 16" in asm

    digest = hashlib.sha256(bytes(dac) + bytes(token) + bytes(confidence)).hexdigest()
    assert digest == EXPECTED_DIGEST, f"Trajectory v2 table drift: {digest}"

    print(
        "Trajectory v2 self-test passed: "
        f"two-stage 1024x16, 64 DAC levels, confidence "
        f"{min(confidence)}..{max(confidence)}, sha256={digest[:16]}"
    )


def _rows(values: list[int]) -> str:
    return "\n".join(
        "    " + ", ".join(map(str, values[i:i + 16])) + ","
        for i in range(0, len(values), 16)
    )


def write_generated() -> None:
    words, dac, token, confidence = regenerate()

    asm = ASM.read_text()
    asm = re.sub(
        r"^lut .*$",
        "lut " + " ".join(map(str, words)),
        asm,
        count=1,
        flags=re.MULTILINE,
    )
    ASM.write_text(asm)

    header = f"""/* Generated Trajectory v2 reference tables.
 * Source model: tools/train_trajectory_v2.py
 *
 * Stage 1: current raw Q4/I4 + middle I-sign + previous Phase5 MSB
 *          -> actual current Phase5 + learned 5-bit trajectory token.
 * Stage 2: previous Phase5 + learned token -> 6-bit CVBS DAC.
 *
 * The target is the clean two-adjacent trajectory d0+d1 with no second wrap.
 * Confidence is the inverse stage-1 residual and is supervisory only.
 */
#pragma once
#include <stdint.h>

static const uint8_t c5vrx_trajectory_v2_dac[1024] = {{
{_rows(dac)}
}};

static const uint8_t c5vrx_trajectory_v2_token[1024] = {{
{_rows(token)}
}};

static const uint8_t c5vrx_trajectory_v2_confidence[1024] = {{
{_rows(confidence)}
}};
"""
    HEADER.write_text(header)

    digest = hashlib.sha256(bytes(dac) + bytes(token) + bytes(confidence)).hexdigest()
    print(
        f"regenerated Trajectory v2: 64 DAC levels, "
        f"two-stage digest={digest}"
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--write", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    if args.write:
        write_generated()
    if args.self_test or not args.write:
        self_test()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
