#!/usr/bin/env python3
"""Correlate DIAG[4:9] and DIAG[16:17] against aligned 80 MS/s RF dump ring.

Verifies whether MODEM_DIAG lanes 4 and 5 are the lower bits Q[4:5] of a 6-bit Q bus.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np

Q6_BEGIN_RE = re.compile(
    r"Q6_DUMP BEGIN samples=(\d+) dump_words=(\d+) stop_ptr=(\d+) elapsed_us=(\d+)"
)
Q6_GPIO_RE = re.compile(r"Q6_GPIO hex=([0-9a-fA-F]+)")
Q6_RING_CHUNK_RE = re.compile(r"Q6_RING chunk=(\d+) hex=([0-9a-fA-F]+)")

# Bit roles:
# bit 0: DIAG[4]  <-> dump Q[4]  (CANDIDATE)
# bit 1: DIAG[5]  <-> dump Q[5]  (CANDIDATE)
# bit 2: DIAG[6]  <-> dump Q[6]  (PROVEN REFERENCE)
# bit 3: DIAG[7]  <-> dump Q[7]  (PROVEN REFERENCE)
# bit 4: DIAG[8]  <-> dump Q[8]  (PROVEN REFERENCE)
# bit 5: DIAG[9]  <-> dump Q[9]  (PROVEN REFERENCE)
# bit 6: DIAG[16] <-> dump I[6]  (PROVEN REFERENCE)
# bit 7: DIAG[17] <-> dump I[7]  (PROVEN REFERENCE)
REFERENCE_BITS = (2, 3, 4, 5, 6, 7)
CANDIDATE_BITS = (0, 1)

SIGNAL_NAMES = {
    0: ("DIAG[4]",  "Candidate Q[4]"),
    1: ("DIAG[5]",  "Candidate Q[5]"),
    2: ("DIAG[6]",  "Proven Q[6]"),
    3: ("DIAG[7]",  "Proven Q[7]"),
    4: ("DIAG[8]",  "Proven Q[8]"),
    5: ("DIAG[9]",  "Proven Q[9]"),
    6: ("DIAG[16]", "Proven I[6]"),
    7: ("DIAG[17]", "Proven I[7]"),
}


def parse_capture(log_text: str):
    m_begin = Q6_BEGIN_RE.search(log_text)
    if not m_begin:
        raise ValueError("Q6_DUMP BEGIN delimiter not found")
    gpio_samples = int(m_begin.group(1))
    dump_words = int(m_begin.group(2))
    stop_ptr = int(m_begin.group(3))
    elapsed_us = int(m_begin.group(4))

    m_gpio = Q6_GPIO_RE.search(log_text)
    if not m_gpio:
        raise ValueError("Q6_GPIO hex line not found")
    gpio_bytes = bytes.fromhex(m_gpio.group(1))
    if len(gpio_bytes) != gpio_samples:
        raise ValueError(f"expected {gpio_samples} gpio bytes, got {len(gpio_bytes)}")

    chunks: dict[int, bytes] = {}
    for m in Q6_RING_CHUNK_RE.finditer(log_text):
        idx = int(m.group(1))
        chunks[idx] = bytes.fromhex(m.group(2))

    if not chunks:
        raise ValueError("no Q6_RING chunks found")

    ring_bytes = b"".join(chunks[i] for i in sorted(chunks.keys()))
    if len(ring_bytes) != dump_words:
        raise ValueError(f"expected {dump_words} ring bytes, got {len(ring_bytes)}")

    return {
        "gpio_samples": gpio_samples,
        "dump_words": dump_words,
        "stop_ptr": stop_ptr,
        "elapsed_us": elapsed_us,
        "gpio": np.frombuffer(gpio_bytes, dtype=np.uint8),
        "ring": np.frombuffer(ring_bytes, dtype=np.uint8),
    }


def correlate_q6(data: dict) -> dict:
    gpio = data["gpio"]
    ring = data["ring"]
    elapsed_us = data["elapsed_us"]
    stop_ptr = data["stop_ptr"]

    gpio_rate_hz = (gpio.size / (elapsed_us / 1e6)) if elapsed_us > 0 else 4.5e6
    nominal_slope = 79_970_000.0 / gpio_rate_hz
    N_ring = ring.size
    N_gpio = min(gpio.size, 1024, int((N_ring - 1) / max(nominal_slope, 1.0)))
    gpio_slice = gpio[-N_gpio:]

    # Convert bipolar bits: 0 -> +1, 1 -> -1
    gpio_bits = [1 - 2 * ((gpio_slice >> b) & 1).astype(np.int8) for b in range(8)]
    ring_bits = [1 - 2 * ((ring >> b) & 1).astype(np.int8) for b in range(8)]
    ring_ffts = [np.fft.fft(ring_bits[b].astype(float)) for b in range(8)]

    best_score = -2.0
    best_slope = nominal_slope
    best_offset = 0

    # Search slope grid around nominal
    steps = 120
    slope_search = np.linspace(nominal_slope - 1.2, nominal_slope + 1.2, steps)

    for slope in slope_search:
        # Map reversed GPIO index: sample N_gpio-1 is closest to stop_ptr
        # pos(k) = offset - slope * k
        pos = (-np.rint(slope * np.arange(N_gpio)).astype(int)) % N_ring
        total_corr = np.zeros(N_ring)

        for b in REFERENCE_BITS:
            sparse = np.zeros(N_ring)
            sparse[pos] = gpio_bits[b][-1::-1]
            total_corr += np.fft.ifft(
                np.conj(np.fft.fft(sparse)) * ring_ffts[b]
            ).real

        offset = int(np.argmax(total_corr))
        score = float(total_corr[offset]) / (len(REFERENCE_BITS) * N_gpio)
        if score > best_score:
            best_score = score
            best_slope = float(slope)
            best_offset = offset

    # Fine slope refinement stage for high-frequency bits
    fine_search = np.linspace(best_slope - 0.06, best_slope + 0.06, 121)
    for slope in fine_search:
        pos = (-np.rint(slope * np.arange(N_gpio)).astype(int)) % N_ring
        total_corr = np.zeros(N_ring)
        for b in REFERENCE_BITS:
            sparse = np.zeros(N_ring)
            sparse[pos] = gpio_bits[b][-1::-1]
            total_corr += np.fft.ifft(
                np.conj(np.fft.fft(sparse)) * ring_ffts[b]
            ).real
        offset = int(np.argmax(total_corr))
        score = float(total_corr[offset]) / (len(REFERENCE_BITS) * N_gpio)
        if score > best_score:
            best_score = score
            best_slope = float(slope)
            best_offset = offset

    # Compute per-bit match percentage with optimal alignment
    indices = (best_offset - np.rint(best_slope * np.arange(N_gpio)).astype(int)) % N_ring
    obs = gpio_slice[-1::-1]
    ref = ring[indices]

    matches = {}
    for b in range(8):
        b_obs = (obs >> b) & 1
        b_ref = (ref >> b) & 1
        matches[b] = float(np.mean(b_obs == b_ref)) * 100.0

    return {
        "score": best_score,
        "slope": best_slope,
        "offset": best_offset,
        "relative_stop_offset": (best_offset - stop_ptr) % N_ring,
        "matches": matches,
        "gpio_samples": N_gpio,
        "gpio_rate_mhz": gpio_rate_hz / 1e6,
    }


def self_test():
    """Verify analyzer on synthetic aligned data with known Bit 0 and Bit 1 values."""
    print("Running self-test with synthetic Q6 capture...")
    np.random.seed(42)
    N_ring = 8192
    N_gpio = 512
    slope = 16.0
    offset = 2100

    # Generate synthetic RF ring with 6 bits of Q and 2 bits of I
    synth_ring = np.random.randint(0, 256, size=N_ring, dtype=np.uint8)

    # Subsample to GPIO at the given slope and offset, with 5% simulated bitflip noise
    indices = (offset - np.rint(slope * np.arange(N_gpio)).astype(int)) % N_ring
    synth_gpio = synth_ring[indices][-1::-1].copy()

    # Add 5% noise to all bits
    for b in range(8):
        flips = np.random.rand(N_gpio) < 0.05
        synth_gpio ^= (flips.astype(np.uint8) << b)

    data = {
        "gpio": synth_gpio,
        "ring": synth_ring,
        "elapsed_us": int(N_gpio / (79.97e6 / slope) * 1e6),
        "stop_ptr": 4350,
    }

    res = correlate_q6(data)
    assert abs(res["slope"] - slope) < 0.05, f"slope mismatch: {res['slope']} vs {slope}"
    assert abs(res["offset"] - offset) <= 2, f"offset mismatch: {res['offset']} vs {offset}"
    for b in range(8):
        assert res["matches"][b] > 90.0, f"bit {b} match too low: {res['matches'][b]:.1f}%"
    print("Self-test PASS: slope, offset, and all 8 bit correlations verified (>90% match).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="?", help="capture log file")
    parser.add_argument("--self-test", action="store_true", help="run synthetic self-test")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return

    if not args.log:
        parser.error("log file required (or use --self-test)")

    log_text = args.log.read_text(encoding="utf-8", errors="replace")
    data = parse_capture(log_text)
    res = correlate_q6(data)

    ref_avg = np.mean([res["matches"][b] for b in REFERENCE_BITS])
    cand_q4 = res["matches"][0]
    cand_q5 = res["matches"][1]

    print("=" * 76)
    print(" C5VRX-3 ALIGNED DIAG[4:5] <-> DUMP Q[4:5] CORRELATION REPORT")
    print("=" * 76)
    print(f"Sampling Alignment Lock:")
    print(f"  RF/GPIO Timing Ratio (Slope): {res['slope']:.3f} (GPIO rate: {res['gpio_rate_mhz']:.2f} MS/s)")
    print(f"  Ring Lock Offset:             {res['offset']} (relative to stop_ptr: {res['relative_stop_offset']})")
    print(f"  Correlation Peak Score:       {res['score']:.3f} (max 1.000)")
    print(f"  Reference Lanes Mean Match:   {ref_avg:.1f}%")
    print("-" * 76)
    print(f"{'Bit':<6} | {'Signal Name':<12} | {'Role':<18} | {'Exact Match':<12} | {'Verdict':<15}")
    print("-" * 76)

    for b in range(8):
        name, role = SIGNAL_NAMES[b]
        pct = res["matches"][b]
        if b in REFERENCE_BITS:
            verdict = "PASS (Ref)" if pct >= 80.0 else "FAIL (Ref)"
        else:
            verdict = "PROVEN Q-BIT" if pct >= 80.0 else "REJECTED (~50%)" if pct <= 60.0 else "INCONCLUSIVE"
        print(f"bit{b:<3} | {name:<12} | {role:<18} | {pct:5.1f}%       | {verdict:<15}")

    print("=" * 76)
    if ref_avg < 80.0:
        print("[WARNING] Reference alignment was weak (<80%). Check RF carrier / VTX status.")
    elif cand_q4 >= 80.0 and cand_q5 >= 80.0:
        print("[VERDICT] SUCCESS: DIAG[4] and DIAG[5] are confirmed as dump Q[4] and Q[5]!")
        print("          DIAG[4:9] forms a genuine 6-bit Q baseband bus.")
    else:
        print(f"[VERDICT] DIAG[4:5] bit match (Q4={cand_q4:.1f}%, Q5={cand_q5:.1f}%) does not track dump Q.")
        print("          DIAG[4:5] are NOT the lower bits of the Q ADC.")


if __name__ == "__main__":
    main()
