"""What a mid-transition MODEM_DIAG read costs at the range edge (C5VRX by
Twotoz and the C5VRX contributors; README section 19, main/sphase_logic.h).

PARLIO RX samples the ~80 MS/s MODEM_DIAG bus at 40 MS/s. On a bad
sampling phase some bits of a read come from the next bus word. This runs
sim.py's chain at 80 MS/s, quantises both bus words with one gain and mixes
their bits per read:
- clean:    every bit from the sampled word (today's model);
- IQskew:   the Q nibble from the next word (12.5 ns I/Q skew);
- lsbI:     only the I LSB from the next word;
- lsbIQ:    the I and Q LSBs;
- msbI:     only the I sign bit;
- lsb2:     the two low bits of I and Q;
- half:     half of the bits of both nibbles;
- random50: every bit at random (zerowidth/C5VRX PR #3 model).
Range limit = worse of goggle lock (0.9 of lines) and usable (luma SINAD
0 dB), bit-exact U85Q kp 0.85, 12 MHz filter, 4 seeds (2 PAL, 2 NTSC).
It also prints the probe excess (dsp.h) per mode on the carrier.
Usage: python sphase_study.py out.json
Synthetic numbers; which bits mix on the board is not known.
"""
import json
import multiprocessing as mp
import os
import sys
from pathlib import Path
import numpy as np
from scipy import signal as sg

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sim  # noqa: E402
from best_combo import limit  # noqa: E402

CNRS = [0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 20]
DEVS = (0.69, 1.0, 1.4)
SEEDS = ((1, 'PAL'), (2, 'NTSC'), (3, 'PAL'), (4, 'NTSC'))
MODES = {'clean': None, 'IQskew': 0x0F, 'lsbI': 0x10, 'lsbIQ': 0x11, 'msbI': 0x80, 'lsb2': 0x33,
         'half': 0x5A, 'random50': 'random'}


def q4(y):
    i = np.clip(np.floor(y.real), -8, 7).astype(int)
    q = np.clip(np.floor(y.imag), -8, 7).astype(int)
    return (((i & 15) << 4) | (q & 15)).astype(np.uint8)


def bus_words(seed, cnr, standard, dev, filter_hz=12e6, rms=4.2):
    """Both 80 MS/s bus words of every 40 MS/s read, noisy and clean."""
    rng = np.random.default_rng(seed)
    ire, region = sim.raster(standard, seed + 7, 2)
    src = sg.lfilter(sg.firwin(81, 6e6, fs=sim.F80), 1, np.repeat(ire, 2))
    freq = sim.PORCH_HZ + dev * src / 140 * sim.DEV_PP
    z = sg.sosfilt(sim.CHAN, np.exp(2j * np.pi * np.cumsum(freq) / sim.F80))
    z /= np.sqrt(np.mean(abs(z) ** 2))
    n = sg.sosfilt(sim.CHAN, (rng.normal(size=len(z)) + 1j * rng.normal(size=len(z))) / np.sqrt(2))
    n /= np.sqrt(np.mean(abs(n) ** 2))
    pre = sg.butter(4, filter_hz / 2, fs=sim.F80, output='sos')
    ratio = 10 ** (cnr / 10)
    out, scale = {}, None
    for name, noise in (('raw', n), ('clean', 0 * n)):
        y = sg.sosfilt(pre, (z * np.sqrt(ratio) + noise) / np.sqrt(1 + ratio))
        if scale is None:
            scale = rms / np.sqrt(np.mean(abs(y[::2]) ** 2))   # one gain, as Direct Gain holds it
        a, b = q4(y[0::2] * scale), q4(y[1::2] * scale)
        m = min(len(a), len(b))
        out[name] = (a[:m], b[:m])
    return out, sg.sosfilt(sim.GOGGLE, ire), region


def mix(a, b, mode, rng):
    if mode is None:
        return a
    mask = rng.integers(0, 256, len(a)).astype(np.uint8) if mode == 'random' else np.uint8(mode)
    return (a & ~mask) | (b & mask)


def excess(b):
    """dsp_hf4_excess_milli() over a byte stream."""
    i = (b.astype(np.int16) >> 4) & 15
    q = b.astype(np.int16) & 15
    i, q = 2 * np.where(i > 7, i - 16, i) + 1, 2 * np.where(q > 7, q - 16, q) + 1
    w = np.array([1, -4, 6, -4, 1])
    di, dq = np.convolve(i, w, 'valid').astype(float), np.convolve(q, w, 'valid').astype(float)
    return 1000 * (np.sum(di ** 2 + dq ** 2) / len(di) / 70 / 4 - 2 / 12)


def job(spec):
    seed, standard, cnr, dev = spec
    words, truth, region = bus_words(seed, cnr, standard, dev)
    rows = []
    for name, mode in MODES.items():
        raw = mix(*words['raw'], mode, np.random.default_rng(seed + 99))
        n = min(len(raw), len(truth))
        c = dict(raw=raw[:n], clean=words['clean'][0][:n], truth=truth[:n], region=region[:n], standard=standard)
        m = sim.measure(c)
        rows.append(dict(mode=name, dev=dev, cnr=cnr, seed=seed, sinad=m['sinad'], lock=m['picture'],
                         excess=float(excess(raw[:200000]))))
    return rows


def report(rows):
    for dev in DEVS:
        print(f'\n=== deviation {dev} ===')
        print(f'{"mode":9s} limit_dB  SINAD@4  SINAD@20  excess@20')
        for name in MODES:
            def mean(c, key):
                return float(np.mean([r[key] for r in rows if r['mode'] == name and r['dev'] == dev and r['cnr'] == c]))
            s = [mean(c, 'sinad') for c in CNRS]
            lk = [mean(c, 'lock') for c in CNRS]
            print(f'{name:9s} {limit(CNRS, s, lk):7.2f}  {s[CNRS.index(4)]:7.2f}  {s[-1]:8.2f}  {mean(20, "excess"):9.0f}')


def main(out_path):
    specs = [(s, std, c, d) for d in DEVS for c in CNRS for s, std in SEEDS]
    with mp.Pool(max(1, os.cpu_count() or 1)) as pool:
        rows = [r for part in pool.imap_unordered(job, specs) for r in part]
    Path(out_path).write_text(json.dumps(rows))
    report(rows)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == 'report':
        report(json.loads(Path(sys.argv[2]).read_text()))
    elif len(sys.argv) > 1:
        main(sys.argv[1])
    else:
        print(__doc__)
        sys.exit(2)
