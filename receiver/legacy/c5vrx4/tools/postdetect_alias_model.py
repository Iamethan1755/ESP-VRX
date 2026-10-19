#!/usr/bin/env python3
"""C5VRX by Twotoz and contributors: post-detection aliasing model (C5VRX-4).

Host model, not a hardware measurement. Question: how much video SNR does the
span75 detector lose, and what would recover it?

Chain: complex FM (Gaussian video 0..4.5 MHz, 1.6 MHz rms deviation, +1.3 MHz
mean) at 80 MS/s, analog channel filter (5th-order Butterworth, one-sided
cutoff), unfiltered 2:1 decimation to 40 MS/s (PARLIO), Q4/I4 floor
quantization with the noise held at the fine-lane sigma of 1.04 steps
(hardware VTX-off at G81: ultrafine P50 7 = 0.52 coarse steps), then:

  FIRMWARE  generated Unwrap75 LUTs (Phase8, trajectory class, STD150 DAC
            codes, resistor-DAC voltage), [D,D,D]
  span75    exact unwrapped 75 ns endpoint delta, 4-bin DAC midpoint
  avg3      mean phase of the three span samples minus the previous span's
            mean (still one code per 75 ns), 4-bin DAC midpoint
  adj40     adjacent 25 ns deltas at 40 MS/s (reference; not feasible on the
            TX BitScrambler)

The goggle input is a 4th-order 5 MHz low-pass. SNR = clean output variance /
(noisy - clean) variance, so linear response differences are not counted as
noise; per-band SNR (1 MHz bands) is independent of picture sharpness.

Usage: python3 tools/postdetect_alias_model.py   (numpy + scipy, ~1 min)
"""
import sys
from pathlib import Path
import numpy as np
from scipy import signal as sg

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import generate_phase8 as gen  # noqa: E402

F80 = 80e6
N = 1 << 18
SIG_CELL = 1.04
rng = np.random.default_rng(1)

b = sg.firwin(801, 4.5e6, fs=F80)
video = sg.lfilter(b, 1, rng.standard_normal(N))
video /= video.std()
dev = 1.6e6 * video + 1.3e6
s80 = np.exp(2j * np.pi * np.cumsum(dev) / F80)
white = (rng.standard_normal(N) + 1j * rng.standard_normal(N)) / np.sqrt(2)


def chan(x, bw):
    bb, aa = sg.butter(5, bw, fs=F80)
    return sg.lfilter(bb, aa, x)


def goggle(x):
    bb, aa = sg.butter(4, 5e6, fs=40e6)
    return sg.lfilter(bb, aa, x)


BANDS = [(0.05e6, 1e6), (1e6, 2e6), (2e6, 3e6), (3e6, 4e6), (4e6, 5e6)]
BAND_FIR = [sg.firwin(511, band, pass_zero=False, fs=40e6) for band in BANDS]

PHASE = np.array(gen.decoder(False)[0])
WORDS = np.array(gen.words_for(False))
VOLT = np.array([sum(1 / r for bit, r in enumerate((8200, 3900, 2000, 1000, 470, 240))
                     if code & (1 << bit)) for code in range(64)])


def raw_bytes(z):
    i = np.clip(np.floor(z.real), -8, 7).astype(int)
    q = np.clip(np.floor(z.imag), -8, 7).astype(int)
    return ((i & 15) << 4) | (q & 15)


def hold(e, span, n):
    y = np.repeat(e, span)
    return goggle(np.pad(y, (0, n - len(y)))[:n])


def detect(z, kind):
    raw = raw_bytes(z)
    n = len(raw)
    if kind == 'FIRMWARE':
        k = (n - 1) // 3
        p, m1, m2, c = (raw[j:3 * k + j:3] for j in range(4))
        sign = lambda x: ((x >> 7) & 1) | (((x >> 3) & 1) << 1)  # noqa: E731
        cls = (WORDS[sign(p) | sign(m1) << 2 | sign(m2) << 4 | sign(c) << 6] >> 6) & 3
        e = (((128 + PHASE[c]) & 254) + ((-PHASE[p]) & 254)) & 255
        return hold(VOLT[WORDS[(e >> 2) | (cls << 6)] & 63], 3, n)
    ph = PHASE[raw].astype(float)
    d1 = ((np.diff(ph, prepend=ph[0]) + 128) % 256) - 128
    if kind == 'adj40':
        return goggle(d1)
    cum = np.cumsum(d1)
    if kind == 'span75':
        e = cum[3::3] - cum[0:-3:3]
    else:  # avg3
        mean = (cum[3::3] + cum[2:-1:3] + cum[1:-2:3]) / 3
        e = np.diff(mean, prepend=mean[0])
    return hold(np.floor(e / 4) * 4 + 2, 3, n)


def snr(clean, noisy):
    c, e = clean[4000:-4000], (noisy - clean)[4000:-4000]
    total = 10 * np.log10(np.var(c) / np.var(e))
    bands = [10 * np.log10(np.var(sg.lfilter(f, 1, clean)[4000:-4000]) /
                           np.var(sg.lfilter(f, 1, noisy - clean)[4000:-4000]))
             for f in BAND_FIR]
    return total, bands


def row(label, total, bands):
    print(f'  {label:26s} {total:5.1f}   ' + ' '.join(f'{x:5.1f}' for x in bands))


def main():
    kinds = ('FIRMWARE', 'span75', 'avg3', 'adj40')
    print('A. Fixed C/N in the 40 MS/s stream, channel +/-20 MHz')
    print('  detector                   total   0-1   1-2   2-3   3-4   4-5 MHz')
    noise = chan(white, 20e6)[::2]
    noise *= SIG_CELL * np.sqrt(2) / np.sqrt(np.mean(np.abs(noise) ** 2))
    for cnr in (6, 8, 10):
        sig = chan(s80, 20e6)[::2] * np.sqrt(2 * SIG_CELL ** 2 * 10 ** (cnr / 10))
        for kind in kinds:
            row(f'CNR {cnr:2d} dB {kind}', *snr(detect(sig, kind), detect(sig + noise, kind)))
    print('B. Fixed C/N0 82 dB-Hz, analog channel width varied (AGC rescales noise)')
    sig2 = F80 / 10 ** (82 / 10)
    for bw in (20e6, 14e6, 11e6, 9e6):
        n80, s = chan(white * np.sqrt(sig2), bw), chan(s80, bw)
        g = SIG_CELL * np.sqrt(2) / np.sqrt(np.mean(np.abs(n80[::2]) ** 2))
        for kind in ('span75', 'avg3'):
            row(f'+/-{bw / 1e6:4.1f} MHz {kind}',
                *snr(detect(g * s[::2], kind), detect(g * (s + n80)[::2], kind)))


if __name__ == '__main__':
    main()
