"""C5VRX-5 stress matrix: where is the receiver good, where is it bad?
(C5VRX by Twotoz and the C5VRX contributors.)

The bit-exact U85Q chain of sim.py, with the receive profile the firmware
picks for each VTX deviation, run through field situations:

  clean        reference
  offset       residual carrier error after AFC (+-0.5, +-1.5 MHz)
  echo         multipath copy: -6 dB at 0.3 us, -3 dB at 1.0 us
  fade         motion nulls: -30 dB for 3 ms every 40 ms
  dc           receiver DC of 0.5 / 1.0 cell (no DC correction held)
  iq           I/Q imbalance 1.5 dB / 10 degrees
  spur         CW interferer -10 dBc at +3 MHz (Wi-Fi / other VTX edge)
  strong       25 dB C/N at the top of the gain band (radius 5.6)

Metrics per C/N: picture (share of lines the goggle model shows correctly,
worse of ideal and peak-clamp AGC) and luma SINAD. Host model, synthetic
numbers, not measured range.

    python situations.py [quick]
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

PROFILES = {
    'DEFAULT': dict(filter_hz=12e6, lut=None),
    'WIDE': dict(filter_hz=16e6, lut=None),
    'KP060': dict(filter_hz=10e6, lut='kp0.60'),
    'KP050': dict(filter_hz=10e6, lut='kp0.50'),
}


def firmware_profile(dev):
    """profile_logic.h: profile_class() with every filter class calibrated."""
    return 'WIDE' if dev >= 1.60 else 'KP050' if dev <= 0.80 else 'KP060' if dev <= 1.05 else 'DEFAULT'


SITUATIONS = {
    'clean': {},
    'offset+0.5': dict(offset_hz=0.5e6),
    'offset-0.5': dict(offset_hz=-0.5e6),
    'offset+1.5': dict(offset_hz=1.5e6),
    'offset-1.5': dict(offset_hz=-1.5e6),
    'echo0.3us': dict(echo=(0.5, 0.3e-6)),
    'echo1.0us': dict(echo=(0.7, 1.0e-6)),
    'fade3ms': dict(fade=(3e-3, 40e-3, -30)),
    'dc0.5': dict(dc=0.5),
    'dc1.0': dict(dc=1.0),
    'iq': dict(iq=(1.5, 10.0)),
    'spur+3MHz': dict(spur=(3e6, -10)),
}


def make_case(seed, cnr_db, standard, deviation, filter_hz, offset_hz=0.0, echo=None, fade=None, dc=0.0,
              iq=None, spur=None, rms=4.2, fields=2):
    rng = np.random.default_rng(seed)
    ire, region = sim.raster(standard, seed + 7, fields)
    source = sg.lfilter(sg.firwin(81, 6e6, fs=sim.F80), 1, np.repeat(ire, 2))
    freq = sim.PORCH_HZ + offset_hz + deviation * source / 140 * sim.DEV_PP
    z80 = np.exp(2j * np.pi * np.cumsum(freq) / sim.F80)
    if echo:
        a, tau = echo
        k = int(round(tau * sim.F80))
        z80 = z80 + a * np.exp(1j * 1.3) * np.r_[np.zeros(k), z80[:-k]]
    if spur:
        f, dbc = spur
        z80 = z80 + 10 ** (dbc / 20) * np.exp(2j * np.pi * f * np.arange(len(z80)) / sim.F80)
    if fade:
        dur, period, depth = fade
        t = np.arange(len(z80)) / sim.F80
        env = np.where((t % period) < dur, 10 ** (depth / 20), 1.0)
        z80 = z80 * sg.lfilter(np.ones(400) / 400, 1, env)   # 5 us edges
    z = sg.sosfilt(sim.CHAN, z80)[::2]
    z /= np.sqrt(np.mean(abs(z) ** 2))
    n = sg.sosfilt(sim.CHAN, (rng.normal(size=2 * len(z)) + 1j * rng.normal(size=2 * len(z))) / np.sqrt(2))[::2]
    n /= np.sqrt(np.mean(abs(n) ** 2))
    pre = sg.butter(4, filter_hz / 2, fs=sim.FS, output='sos') if filter_hz else None
    ratio = 10 ** (cnr_db / 10)
    fade_gain = None
    if fade:
        # Direct Gain holds the gain through a null (fade hold): normalise on
        # the unfaded level, so the null really is a null at the quantiser.
        fade_gain = 1.0

    def quantize(noise):
        y = (z * np.sqrt(ratio) + noise) / np.sqrt(1 + ratio)
        if pre is not None:
            y = sg.sosfilt(pre, y)
        if iq:
            g_db, ph = iq
            g = 10 ** (g_db / 20)
            y = y.real * g + 1j * (y.imag * np.cos(np.radians(ph)) + y.real * np.sin(np.radians(ph)))
        ref = y if fade_gain is None else y[np.abs(z) > 0.5]
        y = y * rms / np.sqrt(np.mean(abs(ref) ** 2))
        y = y + dc * (1 + 1j)
        i = np.clip(np.floor(y.real), -8, 7).astype(int)
        q = np.clip(np.floor(y.imag), -8, 7).astype(int)
        return (((i & 15) << 4) | (q & 15)).astype(np.uint8)

    truth = sg.sosfilt(sim.GOGGLE, ire)
    return dict(raw=quantize(n), clean=quantize(np.zeros_like(n)), truth=truth, region=region, standard=standard)


_LUTS = {}


def lut(name):
    if name is None:
        return None
    if name not in _LUTS:
        _LUTS[name] = np.array(json.loads((HERE / 'data' / 'tracker_luts.json').read_text())[name]['lut'], np.uint16)
    return _LUTS[name]


def job(spec):
    label, dev, prof, sit, cnr, seed, rms = spec
    p = PROFILES[prof]
    std = ('PAL', 'NTSC')[seed % 2]
    c = make_case(1000 * seed + int(10 * cnr) + 7, cnr, std, dev, p['filter_hz'], rms=rms, **SITUATIONS[sit])
    L = lut(p['lut'])
    r = sim.measure(c, sim.u85q_codes(c['raw'], L), sim.u85q_codes(c['clean'], L))
    return dict(label=label, dev=dev, profile=prof, situation=sit, cnr=cnr, seed=seed, picture=r['picture'],
                sinad=r['sinad'])


def main():
    quick = 'quick' in sys.argv
    devs = (1.0, 1.7) if quick else (0.69, 1.0, 1.4, 1.7)
    cnrs = (4.0, 8.0, 20.0)
    seeds = (11, 12)
    specs = []
    for dev in devs:
        prof = firmware_profile(dev)
        for sit in SITUATIONS:
            for cnr in cnrs:
                for s in seeds:
                    specs.append((f'dev{dev}-{prof}-{sit}', dev, prof, sit, cnr, s, 4.2))
        for cnr in (25.0,):
            for s in seeds:
                specs.append((f'dev{dev}-{prof}-strong', dev, prof, 'clean', cnr, s, 5.6))
    with mp.get_context('spawn').Pool(int(os.environ.get('JOBS', 4))) as pool:
        rows = pool.map(job, specs, chunksize=2)
    out = HERE / 'data' / ('situations_quick.json' if quick else 'situations.json')
    out.write_text(json.dumps(rows))
    labels = list(dict.fromkeys(r['label'] for r in rows))
    print(f'{"situation":34s} ' + ' '.join(f'pic@{c:g}  sinad@{c:g}' for c in cnrs))
    for lab in labels:
        rr = [r for r in rows if r['label'] == lab]
        cells = []
        for c in sorted({r['cnr'] for r in rr}):
            x = [r for r in rr if r['cnr'] == c]
            cells.append(f'{np.mean([r["picture"] for r in x]):6.2f} {np.mean([r["sinad"] for r in x]):8.2f}')
        print(f'{lab:34s} ' + '  '.join(cells))


if __name__ == '__main__':
    main()
