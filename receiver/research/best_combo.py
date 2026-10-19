"""Which demod setting is best where? (C5VRX by Twotoz and the C5VRX
contributors; extends range_study.py, u85q_lp.py and kp_morph.py.)

Candidates, all in the existing two-bundle TX BitScrambler:
- U85Q with kp 0.85 .. 0.50 (kp_tables.h): one program, the live LUT can
  move between them seamlessly (README section 13);
- U85Q-LP with the same kp tables (u85q_lp.bsasm): a different program, so
  switching between U85Q and U85Q-LP needs a program change.

Per VTX deviation and C/N: luma SINAD and goggle lock (min of ideal and
peak-clamp AGC) for every candidate, 8 seeds (4 PAL, 4 NTSC), 12 MHz filter
(the DEFAULT class; FILTER_MHZ=10 runs the MATCHED narrow class). Then:
- the best fixed kp for U85Q (one setting everywhere);
- the seamless ladder envelope (best U85Q kp per C/N, lock >= 0.9 of the
  best lock at that point);
- the same with U85Q-LP allowed (needs a program switch).
Usage: python best_combo.py out.json      (CNRS / DEVS / SEEDS env override)
Synthetic numbers (sim.py); not goggle acceptance or measured range.
"""
import json
import multiprocessing as mp
import os
import sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
KPS = (0.85, 0.80, 0.75, 0.70, 0.65, 0.60, 0.55, 0.50)
CNRS = [float(x) for x in os.environ.get('CNRS', '0,1,2,3,4,6,8,12,20,30').split(',')]
DEVS = [float(x) for x in os.environ.get('DEVS', '0.69,1.0,1.4').split(',')]
SEEDS = [int(x) for x in os.environ.get('SEEDS', '31,32,33,34,35,36,37,38').split(',')]
FILTER_MHZ = float(os.environ.get('FILTER_MHZ', '12'))


def job(spec):
    sys.path.insert(0, str(HERE))
    import sim
    from kp_morph import build_lut
    from u85q_lp import lp_codes
    dev, cnr, seed = spec
    c = sim.make_case(10000 * seed + int(10 * cnr) + int(100 * dev), cnr, ('PAL', 'NTSC')[seed % 2], dev,
                      filter_hz=FILTER_MHZ * 1e6, fields=1)
    out = []
    for kp in KPS:
        lut = build_lut(kp)
        for prog in ('U85Q', 'LP'):
            f = (lambda w: sim.u85q_codes(w, lut)) if prog == 'U85Q' else (lambda w: lp_codes(w, lut))
            m = sim.measure(c, f(c['raw']), f(c['clean']))
            out.append(dict(dev=dev, cnr=cnr, seed=seed, prog=prog, kp=kp, sinad=m['sinad'], lock=m['picture']))
    return out


def crossing(xs, ys, level):
    """First C/N where ys reaches level (linear interpolation), or nan."""
    for k in range(1, len(xs)):
        if ys[k - 1] < level <= ys[k]:
            return xs[k - 1] + (level - ys[k - 1]) * (xs[k] - xs[k - 1]) / (ys[k] - ys[k - 1])
    return xs[0] if ys[0] >= level else float('nan')


def limit(cnrs, sinad, lock):
    """Range limit: the worse of usable (SINAD 0 dB) and lock (0.9 of lines)."""
    return max(crossing(cnrs, sinad, 0.0), crossing(cnrs, lock, 0.9))


def main(out_path):
    specs = [(d, c, s) for d in DEVS for c in CNRS for s in SEEDS]
    with mp.Pool(max(1, os.cpu_count() or 1)) as pool:
        rows = [r for part in pool.imap_unordered(job, specs) for r in part]
    Path(out_path).write_text(json.dumps(rows))
    report(rows)


def report(rows):
    def mean(dev, prog, kp, cnr, key):
        v = [r[key] for r in rows if r['dev'] == dev and r['prog'] == prog and r['kp'] == kp and r['cnr'] == cnr]
        return float(np.mean(v))
    for dev in DEVS:
        print(f'\n=== deviation {dev} ({FILTER_MHZ:g} MHz) ===')
        print('prog  kp    limit_dB  ' + '  '.join(f'C/N{c:g}' for c in CNRS))
        lims = {}
        for prog in ('U85Q', 'LP'):
            for kp in KPS:
                s = [mean(dev, prog, kp, c, 'sinad') for c in CNRS]
                l = [mean(dev, prog, kp, c, 'lock') for c in CNRS]
                lims[(prog, kp)] = limit(CNRS, s, l)
                print(f'{prog:5s} {kp:.2f}  {lims[(prog, kp)]:7.2f}   ' + '  '.join(f'{x:5.2f}' for x in s))
        best_fixed = min(KPS, key=lambda kp: (lims[('U85Q', kp)], -kp))
        print(f'best fixed U85Q kp: {best_fixed:.2f} (limit {lims[("U85Q", best_fixed)]:.2f} dB, '
              f'kp 0.85: {lims[("U85Q", 0.85)]:.2f} dB)')
        for allow_lp in (False, True):
            env_s, env_l, pick = [], [], []
            for c in CNRS:
                cands = [('U85Q', kp) for kp in KPS] + ([('LP', kp) for kp in KPS] if allow_lp else [])
                best_lock = max(mean(dev, p, k, c, 'lock') for p, k in cands)
                ok = [(p, k) for p, k in cands if mean(dev, p, k, c, 'lock') >= best_lock - 0.02]
                p, k = max(ok, key=lambda pk: mean(dev, pk[0], pk[1], c, 'sinad'))
                env_s.append(mean(dev, p, k, c, 'sinad'))
                env_l.append(mean(dev, p, k, c, 'lock'))
                pick.append(f'{p}{k:.2f}')
            name = 'U85Q + LP (program switch)' if allow_lp else 'seamless U85Q ladder'
            print(f'{name}: limit {limit(CNRS, env_s, env_l):.2f} dB; per C/N: ' +
                  ', '.join(f'{c:g}:{p}' for c, p in zip(CNRS, pick)))


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == 'report':
        report(json.loads(Path(sys.argv[2]).read_text()))
    elif len(sys.argv) > 1:
        main(sys.argv[1])
    else:
        print(__doc__)
        sys.exit(2)
