#!/usr/bin/env python3
"""Offline bench: history-conditioned two-bundle estimator vs Phase5 / Phase8.

Candidate (docs/long-range-two-bundle-research.md, PR #122 worktree), in the
Golden two-stage LUT shape:
  decoder  LUT[(prev_quadrant << 8) | raw].bits[12:8] -> 5-bit phase state
  pair     LUT[(prev_state << 5) | cur_state].bits[5:0] -> DAC code
The decoder is the MAP state given the raw Q4/I4 cell and the retained
quadrant; the pair table is the MMSE instantaneous frequency given the two
states, on Golden's output scale. Both are *trained* on one synthetic set and
*scored* on an independent one (other picture, seed, carrier offset).

Signal: synthetic PAL CVBS (sync, burst, flats, ramps, edges, chroma field) ->
FM (blank = 0 Hz, 100 IRE = +DEV_HZ) -> complex AWGN at the measured receiver
noise (sigma ~0.56 coarse step per axis at maximum gain) -> 10-bit ADC ->
coarse Q4/I4 bytes at 40 MS/s. Each demodulator is scored with its real 6-bit
DAC resolution. Scores are IRE (1 IRE = DEV_HZ / 100).
"""
from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FS = 40e6
LINE = 2560                       # 64 us at 40 MS/s
DEV_HZ = 4.0e6                    # 100 IRE
NOISE_STEP = 0.56                 # measured sigma per axis, coarse steps
TWO_PI = 2 * math.pi

sys.path.insert(0, str(ROOT / "tools"))
from range_demod_bench import PHASE5, GOLDEN_LUT  # noqa: E402

PHASE8 = np.array([int(v) for v in re.findall(
    r"\b\d+\b", (ROOT / "main/phase8_gain_lut.h").read_text().split("{", 1)[1]
    .split("}", 1)[0])], dtype=np.int64)
P5 = np.array(PHASE5, dtype=np.int64)
GOLD = np.array(GOLDEN_LUT, dtype=np.int64) & 63


# ---------------------------------------------------------------- signal ----
def cvbs_line(n: int, picture: int, rng: np.random.Generator) -> np.ndarray:
    """One PAL line in IRE at 40 MS/s (sync -30, blank 0, white 100)."""
    t = np.arange(LINE) / FS
    x = np.zeros(LINE)
    x[t < 4.7e-6] = -30.0
    burst = (t >= 5.6e-6) & (t < 7.85e-6)
    x[burst] = 20.0 * np.sin(TWO_PI * 4.43361875e6 * t[burst] + n * 0.5 * np.pi)
    act = (t >= 12e-6) & (t < 62e-6)
    u = (t[act] - 12e-6) / 50e-6
    if picture == 0:      # bars, ramp, chroma patch
        y = np.where(u < 0.25, np.floor(u * 16) / 4 * 90 + 7,
                     np.where(u < 0.5, (u - 0.25) * 4 * 93 + 7, 50.0))
        chroma = (u >= 0.5) & (u < 0.8)
        y = y + chroma * 25 * np.sin(TWO_PI * 4.43361875e6 * t[act])
        y = np.where(u >= 0.8, 7 + 88 * ((n // 20) % 2), y)
    else:                 # dark scene with text-like edges and a skin patch
        y = 10 + 8 * np.sin(TWO_PI * u * 3 + n * 0.07)
        edges = (np.floor(u * 40 + (n // 8)) % 7) == 0
        y = np.where(edges, 80.0, y)
        skin = (u > 0.3) & (u < 0.55)
        y = y + skin * (35 + 18 * np.sin(TWO_PI * 4.43361875e6 * t[act] + 1.1))
    x[act] = y + rng.normal(0, 0.5, act.sum())   # camera noise
    return x


def make_capture(lines: int, picture: int, radius: float, cfo_hz: float,
                 seed: int):
    rng = np.random.default_rng(seed)
    ire = np.concatenate([cvbs_line(n, picture, rng) for n in range(lines)])
    freq = cfo_hz + ire / 100.0 * DEV_HZ
    phase = np.cumsum(TWO_PI * freq / FS)
    amp = radius * 64.0
    sig = amp * np.exp(1j * phase)
    noise = NOISE_STEP * 64.0 * (rng.normal(size=sig.size) +
                                 1j * rng.normal(size=sig.size))
    z = sig + noise
    i = np.clip(np.floor(z.real), -512, 511).astype(np.int64) >> 6
    q = np.clip(np.floor(z.imag), -512, 511).astype(np.int64) >> 6
    raw = ((i & 15) << 4) | (q & 15)
    # Truth per output pair k (endpoints at odd samples): mean freq over
    # the 50 ns span, in IRE relative to the carrier offset.
    ends = np.arange(1, raw.size, 2)
    dphi = phase[ends[1:]] - phase[ends[:-1]]
    truth = (dphi / TWO_PI / 50e-9 - cfo_hz) / DEV_HZ * 100.0
    return raw, truth, ire[ends[1:]], phase[ends]


# ----------------------------------------------------------- demodulators ---
def hz_to_ire(hz, cfo_hz):
    return (hz - cfo_hz) / DEV_HZ * 100.0


def demod_phase8(raw, cfo_hz):
    e = raw[1::2]
    d = ((128 + PHASE8[e[1:]] - PHASE8[e[:-1]]) & 255) >> 2
    bins = d * 4 + 2 - 128                     # code centre, Phase8 bins
    return hz_to_ire(bins / 256.0 / 50e-9, cfo_hz)


def golden_code_to_hz(code):
    # Golden P20/G2: code = 20 + round(3*d8/4), d8 in Phase8 bins per 50 ns.
    return (code - 20) * 4.0 / 3.0 / 256.0 / 50e-9


def demod_golden(raw, cfo_hz):
    e = raw[1::2]
    code = GOLD[(P5[e[:-1]] << 5) | P5[e[1:]]]
    return hz_to_ire(golden_code_to_hz(code), cfo_hz)


def true_state(phase_rad):
    return np.floor((phase_rad % TWO_PI) / TWO_PI * 32 + 0.5).astype(np.int64) & 31


class HCEstimator:
    """Decoder MAP[(quadrant, raw)] and pair MMSE[(prev, cur)] tables."""

    def __init__(self, mmse=True):
        self.mmse = mmse
        self.dec = np.zeros(1024, dtype=np.int64)
        self.pair_code = np.zeros(1024, dtype=np.int64)

    def train(self, captures):
        dec_hist = np.zeros((1024, 32))
        pair_sum = np.zeros(1024)
        pair_n = np.zeros(1024)
        for raw, truth, _ire, cfo, phase_end in captures:
            e = raw[1::2]
            ts = true_state(phase_end)
            # Decoder: condition on the TRUE previous quadrant (teacher
            # forcing); the live loop then feeds its own estimate.
            qprev = ts[:-1] >> 3
            addr = (qprev << 8) | e[1:]
            np.add.at(dec_hist, (addr, ts[1:]), 1)
        # MAP with Golden's static state as the prior for unseen addresses.
        for a in range(1024):
            raw = a & 255
            h = dec_hist[a]
            self.dec[a] = int(np.argmax(h)) if h.sum() >= 8 else int(P5[raw])
        for raw, truth, _ire, cfo, _pe in captures:
            states = self.run_states(raw[1::2])
            addr = (states[:-1] << 5) | states[1:]
            hz = truth / 100.0 * DEV_HZ          # CFO removed in truth
            np.add.at(pair_sum, addr, hz + cfo)
            np.add.at(pair_n, addr, 1)
        for a in range(1024):
            prev, cur = a >> 5, a & 31
            d = ((cur - prev + 16) & 31) - 16
            nominal_hz = d * 8 / 256.0 / 50e-9        # state step = 8 bins
            hz = (pair_sum[a] / pair_n[a]
                  if self.mmse and pair_n[a] >= 16 else nominal_hz)
            code = 20 + round(hz * 50e-9 * 256.0 * 3.0 / 4.0)
            self.pair_code[a] = min(63, max(0, code))

    def run_states(self, e):
        states = np.empty(e.size, dtype=np.int64)
        prev = int(P5[e[0]])
        dec = self.dec
        for k in range(e.size):
            prev = int(dec[((prev >> 3) << 8) | int(e[k])])
            states[k] = prev
        return states

    def demod(self, raw, cfo_hz):
        s = self.run_states(raw[1::2])
        code = self.pair_code[(s[:-1] << 5) | s[1:]]
        return hz_to_ire(golden_code_to_hz(code), cfo_hz)


# ---------------------------------------------------------------- scoring ---
def fir_lowpass(x, cutoff_hz, fs=20e6, taps=31):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff_hz / fs * n) * np.hamming(taps)
    return np.convolve(x, h / h.sum(), mode="same")


def score(est, truth, ire):
    err = est - truth
    active = (ire > 0)                          # picture/porch, not sync tip
    luma = fir_lowpass(err, 3.0e6)
    band = err - fir_lowpass(err, 3.8e6)        # >3.8 MHz: chroma band
    band = fir_lowpass(band, 5.0e6)
    clicks = np.mean(np.abs(err) > 40.0) * 1e3
    sync = ire[: err.size] < -20
    sync_ok = np.mean(est[sync] < -15) * 100 if sync.any() else float("nan")
    return (math.sqrt(np.mean(luma[active] ** 2)),
            math.sqrt(np.mean(band[active] ** 2)), clicks, sync_ok)


def capture_set(seeds, picture, radii, cfos, lines):
    out = []
    for seed in seeds:
        for r in radii:
            for cfo in cfos:
                raw, truth, ire, phase_end = make_capture(lines, picture, r,
                                                          cfo, seed)
                out.append((raw, truth, ire, cfo, phase_end))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--lines", type=int, default=60)
    args = ap.parse_args(argv)
    radii = [1.5, 2.0, 2.5, 3.0, 4.0, 5.5]
    train = capture_set([11, 12], 0, radii, [-3e5, 0.0, 4e5], args.lines)
    hc = HCEstimator(mmse=True)
    hc.train(train)
    hn = HCEstimator(mmse=False)
    hn.train(train)
    print("C/N over 40 MHz from radius r at sigma %.2f step: "
          "10log10(r^2 / (2 sigma^2))" % NOISE_STEP)
    print("%5s %6s | %-23s | %-23s | %-23s | %-23s" %
          ("r", "C/N", "Phase8 luma/chr/clk/syn", "Golden", "HC MMSE pair",
           "HC nominal pair"))
    for r in radii:
        cn = 10 * math.log10(r * r / (2 * NOISE_STEP ** 2))
        row = []
        for demod in (demod_phase8, demod_golden, hc.demod, hn.demod):
            acc = []
            for cfo in (-2e5, 2.5e5):
                raw, truth, ire, _ = make_capture(args.lines, 1, r, cfo, 99)
                acc.append(score(demod(raw, cfo), truth, ire))
            row.append(np.mean(np.array(acc), axis=0))
        print("%5.1f %5.1fdB | " % (r, cn) + " | ".join(
            "%5.1f %5.1f %5.1f %5.1f" % tuple(v) for v in row))


if __name__ == "__main__":
    main()
