"""AFC porch-estimate bias at weak signal (C5VRX by Twotoz and the C5VRX
contributors). Host model, not a hardware measurement.

Question (open-field range, PR #196): after a fade, AFC re-acquisition may
retune (up to 4 x 250 kHz). Is the porch estimate it uses unbiased when the
carrier is weak?

The porch estimate in main/afc.h is the mean of wrapped adjacent phase
steps (Phase8 LUT, 156.25 kHz per code) over 1 us of back porch, skipping
samples in the four origin cells. Below the FM threshold the noise phasor
adds clicks; with the carrier off the receiver centre the clicks are not
symmetric, which pulls the mean frequency toward the receiver centre (Rice's
click model). The porch target is -1.0 MHz, so the true error is 0 but a
weak carrier reads as "above target" and the AFC would step the offset the
wrong way.

Model: constant porch tone at -1.0 MHz + true error, C5 channel (5th-order
Butterworth, ~20 MHz complex, as sim.py), optional narrower analog filter,
white noise at a C/N per 20 MHz channel, 80 -> 40 MS/s, Direct Gain V5
radius (rms 4.2 cells), 4-bit ultrafine quantisation, the firmware Phase8
LUT and origin gate. An AFC window holds up to 8 lines x 40 porch pairs; a
decision takes the median of 16 windows (afc_ctrl.h).

    python afc_bias.py              porch-estimate bias and MAD gate
    python afc_bias.py --coherence  observer coherence vs C/N per filter
                                    (tables of main/cn_estimate.h)
"""
from pathlib import Path
import re
import numpy as np
from scipy import signal as sg

HERE = Path(__file__).resolve().parent
F80, FS = 80e6, 40e6
CHAN = sg.butter(5, 10e6, fs=F80, output='sos')
TARGET_KHZ = -1000
KHZ_PER_CODE = 156.25


def phase8_lut():
    text = (HERE.parent / 'main' / 'phase8_gain_lut.h').read_text()
    body = text[text.index('{') + 1:text.index('}')]
    return np.array([int(v) for v in re.findall(r'\d+', body)], dtype=np.int32)


LUT = phase8_lut()
_i = ((np.arange(256) >> 4) ^ 8) - 8
_q = ((np.arange(256) & 15) ^ 8) - 8
POWER = ((2 * _i + 1) ** 2 + (2 * _q + 1) ** 2 + 2) // 4


def porch_windows(rng, cnr_db, error_khz, filter_hz, windows, pairs=320, rms=4.2):
    """Per-window porch estimates (kHz relative to the target)."""
    n40 = windows * (pairs + 1) + 64
    f = (TARGET_KHZ + error_khz) * 1e3
    tone = np.exp(2j * np.pi * f * np.arange(2 * n40) / F80)
    z = sg.sosfilt(CHAN, tone)[::2]
    z /= np.sqrt(np.mean(abs(z[64:]) ** 2))
    w = sg.sosfilt(CHAN, (rng.normal(size=2 * n40) + 1j * rng.normal(size=2 * n40)) / np.sqrt(2))[::2]
    w /= np.sqrt(np.mean(abs(w[64:]) ** 2))
    ratio = 10 ** (cnr_db / 10)
    y = (z * np.sqrt(ratio) + w) / np.sqrt(1 + ratio)
    if filter_hz:
        y = sg.sosfilt(sg.butter(4, filter_hz / 2, fs=FS, output='sos'), y)
    y = y[64:]
    y *= rms / np.sqrt(np.mean(abs(y) ** 2))
    i = np.clip(np.floor(y.real), -8, 7).astype(int)
    q = np.clip(np.floor(y.imag), -8, 7).astype(int)
    raw = ((i & 15) << 4) | (q & 15)
    out = []
    for k in range(windows):
        s = raw[k * (pairs + 1):(k + 1) * (pairs + 1)]
        a, b = s[:-1], s[1:]
        ok = (POWER[a] >= 2) & (POWER[b] >= 2)
        d = ((LUT[b] - LUT[a] + 128) & 255) - 128
        if ok.sum() * 4 < pairs * 3:      # firmware drops lines with < 3/4 valid pairs
            continue
        out.append(d[ok].mean() * KHZ_PER_CODE - TARGET_KHZ)
    return np.array(out)


def coherence_table():
    """Observer coherence (gain.c dg_measure) vs C/N on FM PAL video."""
    import sys
    sys.path.insert(0, str(HERE))
    import sim
    for filt in (None, 16e6, 12e6, 10e6):
        row = []
        for c in (-40, 0, 2, 4, 6, 8, 10, 15):
            vals = []
            for seed in range(3):
                raw = sim.make_case(seed, c, 'PAL', 1.0, filter_hz=filt, fields=1)['raw'].astype(int)
                for k in range(0, len(raw) - 4092, 40000):
                    a, b = raw[k:k + 4091], raw[k + 1:k + 4092]
                    d = ((LUT[b] - LUT[a] + 128) & 255) - 128
                    vals.append(100 * ((POWER[b] >= 8) & (abs(d) <= 32)).mean())
            row.append(f'{c:>3} dB {np.median(vals):5.1f}')
        print(f'{filt and filt / 1e6 or 20:4.0f} MHz: ' + ' | '.join(row) + '   (-40 dB = noise only)')


def main():
    import sys
    if '--coherence' in sys.argv:
        coherence_table()
        return
    rng = np.random.default_rng(196)
    cnrs = [0, 2, 4, 6, 8, 10, 15, 20]
    print('C/N per 20 MHz | filter | true error | median of 16 windows (kHz, mean over 40 decisions) | '
          'AFC would step')
    for filt in (None, 12e6, 10e6):
        for err in (0, -300):
            for c in cnrs:
                meds, passed = [], 0
                for _ in range(40):
                    w = porch_windows(rng, c, err, filt, 16)
                    if len(w) == 16:
                        med = np.median(w)
                        meds.append(med)
                        passed += np.median(abs(w - med)) <= 80   # AFC2_CTRL_MAX_MAD_KHZ

                if not meds:
                    print(f'{c:5d} dB | {filt and filt / 1e6 or 20:4.0f} MHz | {err:5d} | no complete window')
                    continue
                m = float(np.mean(meds))
                step = 0 if abs(m) <= 50 else max(-250, min(250, m))
                print(f'{c:5d} dB | {filt and filt / 1e6 or 20:4.0f} MHz | {err:5d} kHz | est {m:7.0f} kHz | '
                      f'bias {m - err:7.0f} kHz | step {step:5.0f} | MAD gate passes {100 * passed / len(meds):3.0f} %')
            print()


if __name__ == '__main__':
    main()
