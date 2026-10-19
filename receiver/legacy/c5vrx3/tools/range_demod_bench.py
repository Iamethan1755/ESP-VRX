#!/usr/bin/env python3
"""C5VRX weak-signal demod/range benchmark.

Consumes raw packed Q4/I4 bytes from MODEM_DIAG captures and compares:
  * current 50 ns Phase5 endpoint discriminator;
  * exact adjacent 25 ns + 25 ns pair-sum;
  * live two-bundle Trajectory v2 compressed adjacent reconstruction;
  * confidence-aware adjacent repair / PLL-lite holdover (offline experiments);
  * a second-order PLL tracker (offline threshold-extension upper bound).

This tool is deliberately offline. It is used to prove an algorithm on the
same capture before any realtime BitScrambler/M2M path is promoted.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

TAU = 2.0 * math.pi
ROOT = Path(__file__).resolve().parents[1]

# Exact Phase5 mapping mirrored from main/video.c / fm.bsasm.
PHASE5 = [
     4,  6,  7,  7,  7,  8,  8,  8, 24, 24, 24, 25, 25, 25, 26, 28,
     2,  4,  5,  6,  6,  7,  7,  7, 25, 25, 25, 26, 26, 27, 28, 30,
     1,  3,  4,  5,  5,  6,  6,  6, 26, 26, 26, 27, 27, 28, 29, 31,
     1,  2,  3,  4,  5,  5,  5,  6, 26, 27, 27, 27, 28, 29, 30, 31,
     1,  2,  3,  3,  4,  5,  5,  5, 27, 27, 27, 28, 29, 29, 30, 31,
     0,  1,  2,  3,  3,  4,  4,  5, 27, 28, 28, 28, 29, 30, 31,  0,
     0,  1,  2,  3,  3,  4,  4,  4, 28, 28, 28, 29, 29, 30, 31,  0,
     0,  1,  2,  2,  3,  3,  4,  4, 28, 28, 29, 29, 30, 30, 31,  0,
    16, 15, 14, 14, 13, 13, 12, 12, 20, 20, 19, 19, 18, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 12, 20, 20, 20, 19, 19, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 11, 21, 20, 20, 19, 19, 18, 17, 16,
    15, 14, 13, 13, 12, 12, 11, 11, 21, 21, 21, 20, 19, 19, 18, 17,
    15, 14, 13, 12, 11, 11, 11, 10, 22, 21, 21, 21, 20, 19, 18, 17,
    15, 13, 12, 11, 11, 10, 10, 10, 22, 22, 22, 21, 21, 20, 19, 17,
    14, 12, 11, 10, 10,  9,  9,  9, 23, 23, 23, 22, 22, 21, 20, 18,
    12, 10,  9,  9,  9,  8,  8,  8, 24, 24, 24, 23, 23, 23, 22, 20,
]


def _parse_asm_lut(path: Path) -> List[int]:
    import re
    text = path.read_text()
    match = re.search(r"^lut (.*)$", text, re.MULTILINE)
    if not match:
        raise RuntimeError(f"no embedded LUT in {path}")
    return [int(v) for v in match.group(1).split()]


def _parse_header_array(path: Path, name: str) -> List[int]:
    import re
    text = path.read_text()
    start = text.find(name)
    if start < 0:
        raise RuntimeError(f"{name} missing from {path}")
    begin = text.find("{", start)
    end = text.find("};", begin)
    return [int(v) for v in re.findall(r"\d+", text[begin + 1:end])]


GOLDEN_LUT = _parse_asm_lut(ROOT / "main" / "fm.bsasm")
TRAJECTORY_V2_DAC = _parse_header_array(
    ROOT / "main" / "trajectory_v2_lut.h", "c5vrx_trajectory_v2_dac")
TRAJECTORY_V2_TOKEN = _parse_header_array(
    ROOT / "main" / "trajectory_v2_lut.h", "c5vrx_trajectory_v2_token")
TRAJECTORY_V2_CONFIDENCE = _parse_header_array(
    ROOT / "main" / "trajectory_v2_lut.h", "c5vrx_trajectory_v2_confidence")


def s4(v: int) -> int:
    return v - 16 if v & 8 else v


def unpack(byte: int) -> Tuple[int, int]:
    q = s4(byte & 0xF)
    i = s4((byte >> 4) & 0xF)
    return i, q


def power(byte: int) -> int:
    i, q = unpack(byte)
    return i * i + q * q


def phase_rad(byte: int) -> float:
    # Use the centre of the discarded 6-bit ADC bucket, matching the
    # production Phase5/trajectory LUT geometry rather than integer Q4 codes.
    qc = byte & 0x0F
    ic = (byte >> 4) & 0x0F
    q = qc * 64.0 + 31.5
    i = ic * 64.0 + 31.5
    if q >= 512.0:
        q -= 1024.0
    if i >= 512.0:
        i -= 1024.0
    return math.atan2(q, i)


def wrap(x: float) -> float:
    while x > math.pi:
        x -= TAU
    while x <= -math.pi:
        x += TAU
    return x


def d5(a: int, b: int) -> int:
    d = (b & 31) - (a & 31)
    if d > 15:
        d -= 32
    elif d < -16:
        d += 32
    return d


def median3(a: float, b: float, c: float) -> float:
    return sorted((a, b, c))[1]


def iround(x: float) -> int:
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def map_pair_sum_rad(rad: float) -> int:
    """Production P20/G2 mapping. rad may exceed +/-pi: DO NOT wrap it."""
    phase8 = iround(rad * 256.0 / TAU)
    n = phase8 * 3
    correction = -((-n + 2) // 4) if n < 0 else (n + 2) // 4
    return max(0, min(63, 20 + correction))


def golden_code(previous_raw: int, current_raw: int) -> int:
    address = (PHASE5[previous_raw] << 5) | PHASE5[current_raw]
    return GOLDEN_LUT[address] & 63


def trajectory_v2_stage1_address(
    previous_raw: int, middle_raw: int, current_raw: int
) -> int:
    previous_phase5 = PHASE5[previous_raw]
    return (
        current_raw
        | (((middle_raw >> 7) & 1) << 8)
        | (((previous_phase5 >> 4) & 1) << 9)
    )


def trajectory_v2_stage2_address(previous_raw: int, token: int) -> int:
    return PHASE5[previous_raw] | ((token & 31) << 5)


def trajectory_v2_code(previous_raw: int, middle_raw: int, current_raw: int) -> int:
    stage1 = trajectory_v2_stage1_address(previous_raw, middle_raw, current_raw)
    token = TRAJECTORY_V2_TOKEN[stage1]
    return TRAJECTORY_V2_DAC[
        trajectory_v2_stage2_address(previous_raw, token)]


def exact_adjacent_pair_code(previous_raw: int, middle_raw: int, current_raw: int) -> int:
    d0 = wrap(phase_rad(middle_raw) - phase_rad(previous_raw))
    d1 = wrap(phase_rad(current_raw) - phase_rad(middle_raw))
    return map_pair_sum_rad(d0 + d1)


@dataclass
class PairMetrics:
    pairs: int = 0
    winding_disagree: int = 0
    strong_pairs: int = 0
    strong_winding: int = 0
    low_conf_pairs: int = 0
    endpoint_impulses: int = 0
    adjacent_impulses: int = 0
    repaired_impulses: int = 0


def phase5_pair_metrics(data: bytes, parity: int, low_power: int) -> PairMetrics:
    m = PairMetrics()
    phases = [PHASE5[b] for b in data]
    powers = [power(b) for b in data]

    # Endpoint path uses selected samples parity, parity+2, ... and therefore
    # discards the middle 40 MS/s sample before the branch decision.
    for end in range(parity + 2, len(data), 2):
        a, mid, c = end - 2, end - 1, end
        d0 = d5(phases[a], phases[mid])
        d1 = d5(phases[mid], phases[c])
        adjacent = d0 + d1
        endpoint = d5(phases[a], phases[c])
        m.pairs += 1
        if adjacent != endpoint:
            m.winding_disagree += 1
        strong = min(powers[a], powers[mid], powers[c]) >= 64
        if strong:
            m.strong_pairs += 1
            if adjacent != endpoint:
                m.strong_winding += 1
        low = min(powers[a], powers[mid], powers[c]) < low_power
        if low:
            m.low_conf_pairs += 1

        # "Impulse" is deliberately only a comparative tail metric here, not
        # a video-validity decision.
        if abs(endpoint) >= 12:
            m.endpoint_impulses += 1
        if abs(adjacent) >= 20:
            m.adjacent_impulses += 1

        # Offline confidence repair: only alter a delta when the involved IQ
        # is weak AND it is a strong local outlier. This is intentionally
        # conservative and exists to measure potential, not to claim live use.
        repaired0, repaired1 = float(d0), float(d1)
        if end >= parity + 4:
            pm2 = d5(phases[end - 4], phases[end - 3])
            pm1 = d5(phases[end - 3], phases[end - 2])
            local = median3(float(pm2), float(pm1), float(d0))
            if min(powers[a], powers[mid]) < low_power and abs(d0 - local) >= 8:
                repaired0 = local
        if end + 1 < len(data):
            nxt = d5(phases[c], phases[end + 1])
            local = median3(float(d0), float(d1), float(nxt))
            if min(powers[mid], powers[c]) < low_power and abs(d1 - local) >= 8:
                repaired1 = local
        if abs(repaired0 + repaired1) >= 20:
            m.repaired_impulses += 1
    return m


@dataclass
class TrajectoryMetrics:
    pairs: int = 0
    golden_abs_error_sum: int = 0
    trajectory_abs_error_sum: int = 0
    golden_ge8: int = 0
    golden_ge16: int = 0
    golden_ge32: int = 0
    trajectory_ge8: int = 0
    trajectory_ge16: int = 0
    trajectory_ge32: int = 0
    confidence_sum: int = 0
    confidence_lt64: int = 0
    golden_errors: List[int] | None = None
    trajectory_errors: List[int] | None = None


def trajectory_metrics(data: bytes, parity: int) -> TrajectoryMetrics:
    out = TrajectoryMetrics(golden_errors=[], trajectory_errors=[])
    for end in range(parity + 2, len(data), 2):
        p, m, c = data[end - 2], data[end - 1], data[end]
        truth = exact_adjacent_pair_code(p, m, c)
        g = golden_code(p, c)
        t = trajectory_v2_code(p, m, c)
        ge = abs(g - truth)
        te = abs(t - truth)
        out.pairs += 1
        out.golden_abs_error_sum += ge
        out.trajectory_abs_error_sum += te
        out.golden_errors.append(ge)
        out.trajectory_errors.append(te)
        out.golden_ge8 += ge >= 8
        out.golden_ge16 += ge >= 16
        out.golden_ge32 += ge >= 32
        out.trajectory_ge8 += te >= 8
        out.trajectory_ge16 += te >= 16
        out.trajectory_ge32 += te >= 32
        conf = TRAJECTORY_V2_CONFIDENCE[
            trajectory_v2_stage1_address(p, m, c)]
        out.confidence_sum += conf
        out.confidence_lt64 += conf < 64
    return out


def pll_lite_pair_codes(data: bytes, parity: int, low_power: int) -> List[int]:
    """Offline PLL-lite oracle.

    Adjacent deltas are exact full-Q4. A one-pole local-frequency predictor is
    updated only from trustworthy intervals. During a near-origin outlier the
    predictor coasts and replaces only a very large innovation. This is NOT
    the live pixel path; its purpose is to quantify how much stateful holdover
    could still buy beyond the two-bundle Trajectory v2 approximation.
    """
    phases = [phase_rad(b) for b in data]
    powers = [power(b) for b in data]
    predictor = 0.0
    have_predictor = False
    deltas: List[float] = []
    for i in range(1, len(data)):
        d = wrap(phases[i] - phases[i - 1])
        low = min(powers[i - 1], powers[i]) < low_power
        if low and have_predictor and abs(wrap(d - predictor)) >= math.pi / 2:
            d = predictor
        elif not low:
            predictor = d if not have_predictor else 0.75 * predictor + 0.25 * d
            have_predictor = True
        deltas.append(d)

    out: List[int] = []
    for end in range(parity + 2, len(data), 2):
        # deltas[k] is raw[k+1]-raw[k].
        out.append(map_pair_sum_rad(deltas[end - 2] + deltas[end - 1]))
    return out


@dataclass
class PllResult:
    phase_error_rms: float
    impulse_permille: float
    output: List[float]


def pll_demod(
    data: Sequence[int],
    sample_rate: float,
    loop_bw: float,
    max_dev: float,
) -> PllResult:
    wn = min(TAU * loop_bw / sample_rate, 0.5)
    zeta = 1.0 / math.sqrt(2.0)
    kp = 2.0 * zeta * wn
    ki = wn * wn
    freq_max = TAU * max_dev / sample_rate * 1.25

    nco_phase = 0.0
    freq = 0.0
    err_sq = 0.0
    out: List[float] = []
    impulses = 0

    for idx, b in enumerate(data):
        ph = phase_rad(b)
        e = wrap(ph - nco_phase)
        freq = max(-freq_max, min(freq_max, freq + ki * e))
        inst = freq + kp * e
        out.append(inst)
        nco_phase = wrap(nco_phase + inst)
        err_sq += (e * e - err_sq) / 256.0
        if idx > 0 and abs(inst) >= math.pi * 0.75:
            impulses += 1

    return PllResult(
        phase_error_rms=math.sqrt(max(0.0, err_sq)),
        impulse_permille=1000.0 * impulses / max(1, len(data) - 1),
        output=out,
    )


def discriminator(data: Sequence[int]) -> List[float]:
    if len(data) < 2:
        return []
    phases = [phase_rad(b) for b in data]
    return [wrap(phases[i] - phases[i - 1]) for i in range(1, len(phases))]


def percentile_abs(values: Sequence[float], p: float) -> float:
    if not values:
        return 0.0
    xs = sorted(abs(x) for x in values)
    pos = min(len(xs) - 1, max(0, int(round((len(xs) - 1) * p))))
    return xs[pos]


def analyze(data: bytes, args: argparse.Namespace) -> dict:
    parity = 1 if args.parity == "odd" else 0
    m = phase5_pair_metrics(data, parity, args.low_power)
    tm = trajectory_metrics(data, parity)
    pll_lite = pll_lite_pair_codes(data, parity, args.low_power)
    disc = discriminator(data)
    pll = pll_demod(data, args.sample_rate, args.loop_bw, args.max_deviation)

    origin = sum(power(b) <= 4 for b in data)
    clipped = sum(
        (lambda iq: iq[0] in (-8, 7) or iq[1] in (-8, 7))(unpack(b))
        for b in data
    )

    def pm(num: int, den: int) -> float:
        return 1000.0 * num / max(1, den)

    return {
        "samples": len(data),
        "q4_origin_permille": pm(origin, len(data)),
        "q4_clip_permille": pm(clipped, len(data)),
        "phase5_endpoint_pairs": m.pairs,
        "phase5_endpoint_winding_disagree_permille": pm(m.winding_disagree, m.pairs),
        "phase5_strong_winding_disagree_permille": pm(m.strong_winding, m.strong_pairs),
        "phase5_low_conf_pair_permille": pm(m.low_conf_pairs, m.pairs),
        "endpoint_impulse_permille": pm(m.endpoint_impulses, m.pairs),
        "adjacent_pairsum_impulse_permille": pm(m.adjacent_impulses, m.pairs),
        "confidence_repair_impulse_permille": pm(m.repaired_impulses, m.pairs),
        "golden_vs_exact_adjacent_mae_dac": tm.golden_abs_error_sum / max(1, tm.pairs),
        "trajectory_v2_vs_exact_adjacent_mae_dac": tm.trajectory_abs_error_sum / max(1, tm.pairs),
        "golden_hard_ge16_permille": pm(tm.golden_ge16, tm.pairs),
        "trajectory_v2_hard_ge16_permille": pm(tm.trajectory_ge16, tm.pairs),
        "golden_hard_ge32_permille": pm(tm.golden_ge32, tm.pairs),
        "trajectory_v2_hard_ge32_permille": pm(tm.trajectory_ge32, tm.pairs),
        "trajectory_v2_error_p95_dac": percentile_abs(tm.trajectory_errors or [], 0.95),
        "trajectory_v2_error_p99_dac": percentile_abs(tm.trajectory_errors or [], 0.99),
        "trajectory_v2_mean_confidence": tm.confidence_sum / max(1, tm.pairs),
        "trajectory_v2_conf_lt64_permille": pm(tm.confidence_lt64, tm.pairs),
        "pll_lite_output_abs_p99_dac": percentile_abs([v - 20 for v in pll_lite], 0.99),
        "full_q4_discriminator_abs_p95_rad": percentile_abs(disc, 0.95),
        "full_q4_discriminator_abs_p99_rad": percentile_abs(disc, 0.99),
        "pll_loop_bw_hz": args.loop_bw,
        "pll_phase_error_rms_rad": pll.phase_error_rms,
        "pll_impulse_permille": pll.impulse_permille,
    }


def synthetic_self_test() -> None:
    rng = random.Random(0xC5)
    fs = 40_000_000.0
    n = 20_000
    phase = 0.0
    raw = bytearray()
    truth: List[float] = []

    for k in range(n):
        # Wide-FM-ish deterministic modulation, safely below Nyquist.
        inst = 0.42 * math.sin(TAU * k / 173.0) + 0.08 * math.sin(TAU * k / 41.0)
        phase = wrap(phase + inst)
        truth.append(inst)
        amp = 6.0
        i = amp * math.cos(phase) + rng.gauss(0.0, 0.35)
        q = amp * math.sin(phase) + rng.gauss(0.0, 0.35)
        qi = max(-8, min(7, int(round(q)))) & 0xF
        ii = max(-8, min(7, int(round(i)))) & 0xF
        raw.append((ii << 4) | qi)

    disc = discriminator(raw)
    assert len(disc) == n - 1
    # On clean-ish synthetic data the adjacent full-Q4 discriminator should
    # track the known instantaneous frequency with bounded quantization error.
    mse = sum((disc[k - 1] - truth[k]) ** 2 for k in range(1, n)) / (n - 1)
    assert mse < 0.08, mse

    pll = pll_demod(raw, fs, 2_500_000.0, 6_000_000.0)
    assert math.isfinite(pll.phase_error_rms)
    assert len(pll.output) == n

    # Strong/clean guard: the experimental path must not buy weak-signal
    # behavior by making an ordinary high-confidence signal worse.
    clean_golden_abs = 0
    clean_trajectory_abs = 0
    clean_golden_hard = 0
    clean_trajectory_hard = 0
    clean_pairs = 0
    for end in range(3, len(raw), 2):
        truth_code = map_pair_sum_rad(truth[end - 1] + truth[end])
        g = golden_code(raw[end - 2], raw[end])
        t = trajectory_v2_code(raw[end - 2], raw[end - 1], raw[end])
        ge = abs(g - truth_code)
        te = abs(t - truth_code)
        clean_golden_abs += ge
        clean_trajectory_abs += te
        clean_golden_hard += ge >= 16
        clean_trajectory_hard += te >= 16
        clean_pairs += 1
    assert clean_trajectory_abs <= clean_golden_abs, (
        clean_trajectory_abs, clean_golden_abs)
    assert clean_trajectory_hard <= clean_golden_hard, (
        clean_trajectory_hard, clean_golden_hard)

    m = phase5_pair_metrics(raw, 1, 8)
    assert m.pairs > 1000

    # Deterministic near-threshold wide-FM sweep. This is a regression oracle,
    # not a C5 range claim. It specifically guards the reason Trajectory v2
    # exists: the compressed 2-bundle path must reduce the hard-error tail
    # against the full-Q4 exact-adjacent target in a noisy high-deviation case.
    weak_rng = random.Random(0x23C5)
    weak_raw = bytearray()
    weak_truth: List[float] = []
    weak_phase = 0.0
    for k in range(30000):
        inst = (0.72 * math.sin(TAU * k / 71.0) +
                0.34 * math.sin(TAU * k / 19.0))
        weak_phase = wrap(weak_phase + inst)
        weak_truth.append(inst)
        amp = 4.8
        i = amp * math.cos(weak_phase) + weak_rng.gauss(0.0, 0.60)
        q = amp * math.sin(weak_phase) + weak_rng.gauss(0.0, 0.60)
        qi = max(-8, min(7, int(round(q)))) & 0xF
        ii = max(-8, min(7, int(round(i)))) & 0xF
        weak_raw.append((ii << 4) | qi)

    # The weak-signal LUT deliberately uses a clean-trajectory holdover prior
    # when Q4 collapses near origin. Therefore the synthetic regression must
    # compare against the known clean FM trajectory, not against the same
    # noisy Q4 samples that the holdover is intended to repair.
    golden_hard = 0
    trajectory_hard = 0
    golden_abs = 0
    trajectory_abs = 0
    pairs = 0
    for end in range(3, len(weak_raw), 2):
        truth_code = map_pair_sum_rad(weak_truth[end - 1] + weak_truth[end])
        g = golden_code(weak_raw[end - 2], weak_raw[end])
        t = trajectory_v2_code(weak_raw[end - 2], weak_raw[end - 1], weak_raw[end])
        ge = abs(g - truth_code)
        te = abs(t - truth_code)
        golden_abs += ge
        trajectory_abs += te
        golden_hard += ge >= 16
        trajectory_hard += te >= 16
        pairs += 1

    assert pairs > 10000
    assert trajectory_hard < golden_hard, (trajectory_hard, golden_hard)
    assert trajectory_abs < golden_abs, (trajectory_abs, golden_abs)
    assert len(TRAJECTORY_V2_DAC) == 1024
    assert len(TRAJECTORY_V2_TOKEN) == 1024
    assert len(TRAJECTORY_V2_CONFIDENCE) == 1024

    print(
        "range_demod_bench self-test passed: "
        f"disc_mse={mse:.5f} pairs={m.pairs} "
        f"clean_mae golden={clean_golden_abs/clean_pairs:.2f} "
        f"traj={clean_trajectory_abs/clean_pairs:.2f} "
        f"winding_pm={1000.0*m.winding_disagree/max(1,m.pairs):.2f} "
        f"weak_clean_ge16 golden={1000.0*golden_hard/pairs:.1f}pm "
        f"traj={1000.0*trajectory_hard/pairs:.1f}pm "
        f"mae golden={golden_abs/pairs:.2f} traj={trajectory_abs/pairs:.2f}"
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("capture", nargs="?", type=Path, help="raw packed Q4/I4 capture")
    p.add_argument("--parity", choices=("odd", "even"), default="odd")
    p.add_argument("--low-power", type=int, default=8)
    p.add_argument("--sample-rate", type=float, default=40_000_000.0)
    p.add_argument("--loop-bw", type=float, default=2_500_000.0)
    p.add_argument("--max-deviation", type=float, default=6_000_000.0)
    p.add_argument("--json", action="store_true")
    p.add_argument("--self-test", action="store_true")
    return p.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    if args.self_test:
        synthetic_self_test()
        return 0
    if args.capture is None:
        print("capture path required (or use --self-test)", file=sys.stderr)
        return 2
    data = args.capture.read_bytes()
    if len(data) < 64:
        print("capture is too short", file=sys.stderr)
        return 2
    result = analyze(data, args)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        for key, value in result.items():
            if isinstance(value, float):
                print(f"{key:46s} {value:.4f}")
            else:
                print(f"{key:46s} {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
