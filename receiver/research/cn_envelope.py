#!/usr/bin/env python3
"""C/N from the envelope instead of the phase coherence (C5VRX by Twotoz and
the C5VRX contributors). Host model, not a hardware measurement.

Board walks 2026-10-13 (docs/FINDINGS.md): the coherence model of
main/cn_estimate.h read 9-13 dB next to the VTX. Its 45-degree phase-step
test also fails on fast FM swings (white at +9 MHz is 81 degrees per sample
at 40 MS/s), so it depends on picture and deviation, not only on noise.

An FM carrier has a constant envelope; noise makes it fluctuate. With the
window DC removed, p = |z|^2 has mean^2/var R = (c+1)^2/(2c+1) for carrier
to noise c (complex Gaussian noise: R = 1). This script measures R per
observer probe (4 x 64 bytes of a 4092-byte descriptor, as transport.c
copy_probe_window) on sim.py FM video through the 4-bit quantiser, per
filter class and deviation, and prints the medians the firmware table uses.

    python research/cn_envelope.py
"""
from pathlib import Path
import sys
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sim  # noqa: E402

OFFSETS = (512, 1536, 2560, 4028)
CNS = (-40, 0, 2, 4, 6, 8, 10, 12, 15, 20, 25)


def ratio_x100(probe):
    x = 2 * ((probe.astype(np.int16) >> 4) ^ 8) - 15   # 2*i+1 with i = signed high nibble
    y = 2 * ((probe.astype(np.int16) & 15) ^ 8) - 15
    x = x - x.mean()
    y = y - y.mean()
    p = x * x + y * y
    v = p.var()
    if p.mean() <= 0:
        return 0
    return 65535 if v <= 0 else min(65535, int(100 * p.mean() ** 2 / v + .5))


def probes(raw):
    for k in range(0, len(raw) - 4092, 4092):
        blk = raw[k:k + 4092]
        yield np.concatenate([blk[o:o + 64] for o in OFFSETS])


def main():
    for filt in (None, 16e6, 12e6, 10e6):
        for dev in (0.69, 1.0, 1.4):
            row = []
            for c in CNS:
                vals = []
                for seed in range(2):
                    raw = sim.make_case(seed, c, 'PAL', dev, filter_hz=filt, fields=1)['raw']
                    vals += [ratio_x100(p) for p in probes(raw)]
                row.append(f'{c:>3}:{int(np.median(vals)):5d}')
            print(f'{(filt or 20e6) / 1e6:4.0f} MHz dev {dev:4.2f} | ' + ' '.join(row), flush=True)


if __name__ == '__main__':
    main()
