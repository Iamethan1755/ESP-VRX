#!/usr/bin/env python3
"""Decode full RF dump words (probe AGC_WORDS passes) into native AGC events.

Word layout (Q/I proven bit-exact on C5VRX; gain/state per ESPARGOS esp-sdr):
  bits 0..9   Q, signed 10-bit
  bits 10..19 I, signed 10-bit
  bits 20..27 RX gain index
  bits 28..31 AGC state machine

Reports per window and overall:
  * acquisitions: gain changes closer than --merge-us belong to one event;
    event interval, duration, gain path and time share;
  * which state values occur during acquisitions versus trapped (stable-gain)
    stretches, and which state bits separate the two -- the candidate hold
    flag for an AGC-gated demodulator;
  * 10-bit IQ amplitude in trapped stretches and during acquisitions,
    including the share outside |x| < 256 (the issue #123 fine-lane window).

Usage:
  python tools/analyze_agc_words.py probe_boot.log
  python tools/analyze_agc_words.py --self-test
"""

import argparse
import re
import sys

import numpy as np

FS_HZ = 80e6
BEGIN_RE = re.compile(r"AGC_WORDS BEGIN window=(\d+) words=(\d+) stop_ptr=(\d+)")
HEX_RE = re.compile(r"AGC_WORDS_HEX window=(\d+) chunk=(\d+) hex=([0-9a-fA-F]+)")


def parse(text):
    windows = {}
    for m in BEGIN_RE.finditer(text):
        windows[int(m.group(1))] = {"words": int(m.group(2)), "stop": int(m.group(3)), "chunks": {}}
    for m in HEX_RE.finditer(text):
        w = windows.get(int(m.group(1)))
        if w is not None:
            w["chunks"][int(m.group(2))] = m.group(3)
    out = []
    for n in sorted(windows):
        w = windows[n]
        raw = "".join(w["chunks"][c] for c in sorted(w["chunks"]))
        words = np.array([int(raw[i:i + 8], 16) for i in range(0, len(raw) - 7, 8)], dtype=np.uint32)
        if len(words) != w["words"]:
            continue
        out.append(np.roll(words, -w["stop"]))  # oldest sample first
    return out


def fields(words):
    def signed10(x):
        x = x.astype(np.int32)
        return np.where(x >= 512, x - 1024, x)
    q = signed10(words & 0x3FF)
    i = signed10((words >> 10) & 0x3FF)
    gain = ((words >> 20) & 0xFF).astype(np.int32)
    state = ((words >> 28) & 0xF).astype(np.int32)
    return i, q, gain, state


def events(gain, merge):
    change = np.flatnonzero(np.diff(gain) != 0) + 1
    ev = []
    for c in change:
        if ev and c - ev[-1][1] <= merge:
            ev[-1][1] = c
        else:
            ev.append([c, c])
    return ev


def analyze(windows, merge_us=4.0):
    merge = int(merge_us * FS_HZ / 1e6)
    acq_state, trap_state = [], []
    acq_mask_all, amp_trap, amp_acq = [], [], []
    starts, durations, paths = [], [], []
    segments = []   # (gain, samples, mean I, mean Q, radius) per trapped stretch
    total = 0
    for words in windows:
        i, q, gain, state = fields(words)
        n = len(words)
        total += n
        acq = np.zeros(n, dtype=bool)
        for a, b in events(gain, merge):
            lo = max(0, a - 8)
            acq[lo:b + 1] = True
            starts.append(a + total - n)
            durations.append((b - a) / FS_HZ * 1e6)
            paths.append(gain[max(0, a - 1):b + 1][np.r_[True, np.diff(gain[max(0, a - 1):b + 1]) != 0]].tolist())
        acq_state.append(state[acq])
        trap_state.append(state[~acq])
        # Trapped stretches: DC offset and radius per gain state. A PHY that
        # calibrates DC per gain index shows a different IQ centre per stretch
        # when the tap sits before that correction.
        edges = np.flatnonzero(np.diff(acq.astype(np.int8)) != 0) + 1
        bounds = np.r_[0, edges, n]
        for a, b in zip(bounds[:-1], bounds[1:]):
            if not acq[a] and b - a >= 400:
                segments.append((int(np.bincount(gain[a:b]).argmax()), b - a,
                                 float(i[a:b].mean()), float(q[a:b].mean()),
                                 float(np.median(np.hypot(i[a:b], q[a:b])))))
        amp = np.maximum(np.abs(i), np.abs(q))
        amp_trap.append(amp[~acq])
        amp_acq.append(amp[acq])
        acq_mask_all.append(acq)
    acq_state = np.concatenate(acq_state) if acq_state else np.array([], int)
    trap_state = np.concatenate(trap_state) if trap_state else np.array([], int)
    amp_trap = np.concatenate(amp_trap) if amp_trap else np.array([], int)
    amp_acq = np.concatenate(amp_acq) if amp_acq else np.array([], int)
    acq_share = float(np.concatenate(acq_mask_all).mean()) if acq_mask_all else 0.0

    def hist(x):
        v, c = np.unique(x, return_counts=True)
        return {int(a): int(b) for a, b in zip(v, c)}

    # A state bit separates the two if it is (almost) always one value in
    # acquisitions and the other when trapped.
    separating = []
    for bit in range(4):
        if len(acq_state) and len(trap_state):
            a = float(((acq_state >> bit) & 1).mean())
            t = float(((trap_state >> bit) & 1).mean())
            separating.append((bit, round(a, 3), round(t, 3)))
    intervals = np.diff(starts) / FS_HZ * 1e6 if len(starts) > 1 else np.array([])
    return {
        "windows": len(windows), "samples": total, "events": len(starts),
        "acq_share": acq_share,
        "interval_us": intervals, "duration_us": np.array(durations), "paths": paths,
        "state_acq": hist(acq_state), "state_trap": hist(trap_state),
        "state_bits": separating,
        "segments": segments,
        "trap_amp_p50": float(np.median(amp_trap)) if len(amp_trap) else None,
        "trap_amp_p99": float(np.percentile(amp_trap, 99)) if len(amp_trap) else None,
        "trap_out256": float((amp_trap >= 256).mean()) if len(amp_trap) else None,
        "acq_out256": float((amp_acq >= 256).mean()) if len(amp_acq) else None,
    }


def report(r):
    print(f"windows={r['windows']} samples={r['samples']} ({r['samples'] / FS_HZ * 1e6:.0f} us)")
    print(f"acquisitions: {r['events']}, time share {100 * r['acq_share']:.1f}%")
    if len(r["interval_us"]):
        iv = r["interval_us"]
        print(f"  interval us: median {np.median(iv):.1f}  min {iv.min():.1f}  max {iv.max():.1f}")
    if len(r["duration_us"]):
        print(f"  duration us: median {np.median(r['duration_us']):.2f}  max {r['duration_us'].max():.2f}")
    for p in r["paths"][:8]:
        print(f"  gain path: {p}")
    print(f"state during acquisition: {r['state_acq']}")
    print(f"state when trapped:       {r['state_trap']}")
    for bit, a, t in r["state_bits"]:
        tag = "  <- separates" if abs(a - t) >= 0.8 else ""
        print(f"  state bit {bit}: P(1|acq)={a:.3f} P(1|trapped)={t:.3f}{tag}")
    if r["segments"]:
        print("trapped stretches (gain, us, DC I, DC Q, radius):")
        for g, n, di, dq, rad in r["segments"][:12]:
            print(f"  gain {g:3d}  {n / FS_HZ * 1e6:6.1f} us  DC ({di:+6.1f},{dq:+6.1f})  radius {rad:6.1f}")
        dcs = np.array([(di, dq) for _, _, di, dq, _ in r["segments"]])
        rads = np.array([rad for *_, rad in r["segments"]])
        print(f"  DC spread between stretches: {np.ptp(dcs[:, 0]):.1f} / {np.ptp(dcs[:, 1]):.1f} codes; "
              f"radius spread {np.ptp(rads):.1f} codes")
    if r["trap_amp_p50"] is not None:
        print(f"trapped max(|I|,|Q|): p50 {r['trap_amp_p50']:.0f} p99 {r['trap_amp_p99']:.0f} "
              f"codes; outside +-256: {1000 * r['trap_out256']:.1f} pm")
    if r["acq_out256"] is not None:
        print(f"during acquisition outside +-256: {1000 * r['acq_out256']:.1f} pm")


def self_test():
    rng = np.random.default_rng(1)
    words = []
    for _ in range(3):
        n = 8192
        gain = np.full(n, 40)
        state = np.full(n, 11)
        amp = np.full(n, 150.0)
        for start in range(500, n - 400, 3000):
            for k, g in enumerate((82, 60, 58, 44)):
                gain[start + 48 * k:start + 48 * (k + 1)] = g
            gain[start + 192:] = 44 if start < 5000 else 40
            state[start:start + 192] = 3
            amp[start:start + 96] = 480.0
        ph = np.cumsum(rng.normal(0.3, 0.05, n))
        i = np.clip(np.round(amp * np.cos(ph)), -512, 511).astype(int) & 0x3FF
        q = np.clip(np.round(amp * np.sin(ph)), -512, 511).astype(int) & 0x3FF
        w = (q | (i << 10) | (gain.astype(int) << 20) | (state.astype(int) << 28)).astype(np.uint32)
        words.append(np.roll(w, 1234))
    text = ""
    for n, w in enumerate(words):
        text += f"AGC_WORDS BEGIN window={n} words=8192 stop_ptr=1234\n"
        for c in range(32):
            text += f"AGC_WORDS_HEX window={n} chunk={c} hex=" + "".join(f"{x:08x}" for x in w[c * 256:(c + 1) * 256]) + "\n"
    r = analyze(parse(text))
    assert r["windows"] == 3 and r["events"] == 9, r["events"]
    assert abs(np.median(r["duration_us"]) - 1.8) < 0.1
    assert abs(np.median(r["interval_us"]) - 37.5) < 1.0
    bits = {b: (a, t) for b, a, t in r["state_bits"]}
    assert bits[3][0] < 0.2 and bits[3][1] > 0.95      # bit 3: 0 in acq, 1 trapped
    assert r["trap_out256"] == 0.0 and r["acq_out256"] > 0.3
    print("analyze_agc_words self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--merge-us", type=float, default=4.0)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.logs:
        ap.print_help()
        sys.exit(2)
    text = "".join(open(p, encoding="utf-8", errors="replace").read() for p in args.logs)
    windows = parse(text)
    if not windows:
        sys.exit("no complete AGC_WORDS windows found")
    report(analyze(windows, args.merge_us))


if __name__ == "__main__":
    main()
