"""Where can C5VRX-5 firmware still gain range? (C5VRX by Twotoz and the
C5VRX contributors.)

For every configuration a C/N sweep is simulated (sim.py, bit-exact U85Q)
and the picture threshold is taken as the C/N at which the goggle still
shows 90 % of the lines correctly. A threshold that drops by X dB is X dB
more link budget: free-space range scales with 10^(X/20).

Experiments:
  filter    narrower analog filter before the ADC, per VTX deviation
  sync      upper bound of perfect sync regeneration (ideal flywheel)
  tracker   lower U85Q loop gain (threshold extension), fixed program
  wide      wide VTXs (the board VTX swung sync -5.0 to white +6.25 MHz,
            deviation ~1.7): filter width per deviation, kp 0.85 / 0.70
Usage: python range_study.py <experiment> <out.json>
"""
import json
import os
import multiprocessing as mp
import sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
CNRS = np.array([float(x) for x in os.environ["CNRS"].split(",")]) if "CNRS" in os.environ else np.arange(float(os.environ.get("CN_MIN", -3)), 9.0, 1.0)
SEEDS = (11, 12, 13, 14)          # two PAL, two NTSC
THRESHOLD = 0.9


def job(spec):
    sys.path.insert(0, str(HERE))
    import sim
    exp, label, cfg, seed, cnr = spec
    std = ('PAL', 'NTSC')[seed % 2]
    c = sim.make_case(1000 * seed + int(10 * cnr) + 7, cnr, std, cfg.get('deviation', 1.0),
                      filter_hz=cfg.get('filter_hz'), offset_hz=cfg.get('offset_hz', 0.0))
    lut = None
    if 'lut' in cfg:
        lut = np.array(json.loads((HERE / 'data' / 'tracker_luts.json').read_text())[cfg['lut']]['lut'], np.uint16)
    codes, clean = sim.u85q_codes(c['raw'], lut), sim.u85q_codes(c['clean'], lut)
    if cfg.get('sync_regen') is True:
        # Ideal regenerator: sync pulses replaced by clean ones (codes of the
        # noise-free decode inside the true sync intervals), picture untouched.
        m = np.repeat(c['region'] == 2, 1)[:len(codes)]
        codes = np.where(m, clean, codes).astype(np.uint8)
    if cfg.get('sync_regen') == 'pll':
        codes = sim.regenerate_codes(codes, clean, c)
    r = sim.measure(c, codes, clean)
    return dict(exp=exp, label=label, seed=seed, cnr=float(cnr), picture=r['picture'], sinad=r['sinad'])


def configs(exp):
    if exp == 'filter':
        out = {}
        for dev in (0.69, 1.0, 1.4):
            for f in (None, 16e6, 14e6, 12e6, 11e6, 10e6):
                out[f'dev{dev}-{"chan20" if f is None else "%gMHz" % (f / 1e6)}'] = dict(deviation=dev, filter_hz=f)
        return out
    if exp == 'sync':
        return {f'{"regen" if regen else "plain"}-{"chan20" if f is None else "14MHz"}':
                dict(deviation=1.0, filter_hz=f, sync_regen=regen)
                for regen in (False, True) for f in (None, 14e6)}
    if exp == 'regen':
        return {f'{k}-{"chan20" if f is None else "14MHz"}': dict(deviation=1.0, filter_hz=f, sync_regen=v)
                for k, v in (('plain', False), ('pll', 'pll'), ('ideal', True)) for f in (None, 14e6)}
    if exp == 'matched':
        out = {}
        for dev in (0.69, 1.0, 1.4):
            for f in (14e6, 12e6, 10e6):
                for k in ('kp0.50', 'kp0.60', 'kp0.70', 'kp0.85'):
                    out[f'dev{dev}-{f / 1e6:g}MHz-{k}'] = dict(deviation=dev, filter_hz=f, lut=k)
        return out
    if exp == 'strong':
        # Matched settings against v5 on strong signals and with residual
        # carrier error after AFC.
        sets = {'v5': (14e6, 'kp0.85'), 'm069': (10e6, 'kp0.50'), 'm100': (10e6, 'kp0.60'), 'm140': (12e6, 'kp0.85')}
        out = {}
        for dev in (0.69, 1.0, 1.4):
            for name, (f, k) in sets.items():
                for off in (0.0, 0.5e6, -0.5e6):
                    out[f'dev{dev}-{name}-off{off / 1e6:+.1f}'] = dict(deviation=dev, filter_hz=f, lut=k, offset_hz=off)
        return out
    if exp == 'wide':
        out = {}
        for dev in (1.4, 1.7, 2.0):
            for f in (12e6, 14e6, 16e6, None):
                for k in ('kp0.85', 'kp0.70'):
                    out[f'dev{dev}-{"chan20" if f is None else "%gMHz" % (f / 1e6)}-{k}'] = dict(
                        deviation=dev, filter_hz=f, **({} if k == 'kp0.85' else {'lut': k}))
        return out
    if exp == 'tracker':
        return {f'{k}-dev{dev}': dict(deviation=dev, filter_hz=14e6, lut=k)
                for k in ('kp0.50', 'kp0.60', 'kp0.70', 'kp0.85') for dev in (0.69, 1.0, 1.4)}
    raise SystemExit('unknown experiment')


def threshold(rows):
    cn = sorted({r['cnr'] for r in rows})
    pic = [np.mean([r['picture'] for r in rows if r['cnr'] == c]) for c in cn]
    for k in range(len(cn)):
        if pic[k] >= THRESHOLD and all(p >= THRESHOLD for p in pic[k:]):
            if k == 0:
                return cn[0]
            a, b = pic[k - 1], pic[k]
            return cn[k - 1] + (THRESHOLD - a) / (b - a) * (cn[k] - cn[k - 1])
    return float('nan')


def usable(rows, level=0.0):
    """C/N where the luma SINAD reaches 0 dB: below it the picture is noise,
    even when the goggle still locks."""
    cn = sorted({r['cnr'] for r in rows})
    v = [np.mean([r['sinad'] for r in rows if r['cnr'] == c]) for c in cn]
    for k in range(1, len(cn)):
        if v[k - 1] < level <= v[k]:
            return cn[k - 1] + (level - v[k - 1]) / (v[k] - v[k - 1]) * (cn[k] - cn[k - 1])
    return float('nan')


def main():
    exp, out = sys.argv[1], Path(sys.argv[2])
    cfgs = configs(exp)
    specs = [(exp, label, cfg, s, float(c)) for label, cfg in cfgs.items() for s in SEEDS for c in CNRS]
    with mp.get_context('spawn').Pool(int(os.environ.get('JOBS', 7))) as pool:
        rows = pool.map(job, specs, chunksize=4)
    out.write_text(json.dumps(rows))
    print(f'{"configuration":26s} lock_dB  usable_dB  SINAD@4dB  SINAD@8dB')
    for label in cfgs:
        r = [x for x in rows if x['label'] == label]
        s4 = np.mean([x['sinad'] for x in r if x['cnr'] == 4.0])
        s8 = np.mean([x['sinad'] for x in r if x['cnr'] == 8.0])
        print(f'{label:26s} {threshold(r):7.2f} {usable(r):10.2f} {s4:10.2f} {s8:10.2f}')


if __name__ == '__main__':
    main()
