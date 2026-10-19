"""Measured kp: pick the U85Q loop gain from the received IQ itself.
(C5VRX by Twotoz and the C5VRX contributors; extends kp_morph.py and
best_combo.py.)

The kp ladder (main/kp_ladder.h) looks kp up from a C/N estimate and a
deviation class. This study asks whether the receiver can measure the best
kp directly instead:
- the control task already copies one completed 4092-byte IQ window every
  50 ms (s_window);
- the CPU can run U85Q in software on that window for every ladder kp. It
  is a bit-exact replica: the same LUTs, a few thousand lookups each;
- from the decoded codes it measures, per kp, the noise on the sync tips
  (flat, the same in every picture) and the colour-burst amplitude (the
  response at the subcarrier, i.e. sharpness);
- all kp see the same IQ samples, so the comparison is paired and needs
  few windows.

Selection rules compared with the oracle (the kp with the best true luma
SINAD, needing the reference picture) and with the table ladder:
- noise:   lowest sync-tip noise;
- chroma:  highest burst^2 / noise (colour SNR);
- blend:   highest burst^w / noise (w fitted on separate seeds).
Usage: python adaptive_kp.py out.json   (CNRS / DEVS / SEEDS env override)
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
CNRS = [float(x) for x in os.environ.get('CNRS', '1,2,3,4,6,8,12,20').split(',')]
DEVS = [float(x) for x in os.environ.get('DEVS', '0.69,1.0,1.4').split(',')]
SEEDS = [int(x) for x in os.environ.get('SEEDS', '51,52,53,54,55,56').split(',')]
WINDOW = 4092           # bytes, CONTROL_BYTES
WINDOWS = 20            # one second of control windows (20 per field here)


def window_stats(codes, fsc):
    """Sync-tip noise and burst amplitude from decoded codes of one window
    (40 MS/s bytes, [D,D] pairs). Returns (n_tips, sum_var, sum_burst)."""
    y = codes[1::2].astype(float)            # 20 MS/s unique codes
    if len(y) < 400:
        return 0, 0.0, 0.0
    ys = np.convolve(y, np.ones(9) / 9, mode='same')     # find syncs on a smoothed copy
    lo, hi = np.percentile(ys, 3), np.percentile(ys, 50)
    thr = lo + 0.5 * (hi - lo)
    below = ys < thr
    tips, var, burst = 0, 0.0, 0.0
    k = 0
    n = len(y)
    while k < n:
        if not below[k]:
            k += 1
            continue
        s = k
        while k < n and below[k]:
            k += 1
        e = k
        if 70 <= e - s <= 110 and e + 70 < n:     # H-sync, 3.5-5.5 us at 20 MS/s
            core = y[s + 10:e - 10]
            var += float(np.var(core))
            # Burst: 0.6..3.0 us after the sync end, at the subcarrier.
            b = y[e + 12:e + 60]
            t = np.arange(len(b)) / 20e6
            burst += float(2 * abs(np.mean((b - b.mean()) * np.exp(-2j * np.pi * fsc * t))))
            tips += 1
    return tips, var, burst


def job(spec):
    sys.path.insert(0, str(HERE))
    import sim
    from kp_morph import build_lut
    dev, cnr, seed = spec
    std = ('PAL', 'NTSC')[seed % 2]
    fsc = 4433618.75 if std == 'PAL' else 3579545.45
    c = sim.make_case(20000 * seed + int(10 * cnr) + int(100 * dev), cnr, std, dev, filter_hz=12e6, fields=1)
    raw = c['raw']
    starts = np.linspace(4000, len(raw) - WINDOW - 4000, WINDOWS).astype(int) & ~1
    row = dict(dev=dev, cnr=cnr, seed=seed, kp={})
    for kp in KPS:
        lut = build_lut(kp)
        m = sim.measure(c, sim.u85q_codes(raw, lut), sim.u85q_codes(c['clean'], lut))
        tips, var, burst = 0, 0.0, 0.0
        for s in starts:
            w = sim.u85q_codes(raw[s:s + WINDOW], lut)[64:]     # replica, fresh state
            t, v, b = window_stats(w, fsc)
            tips, var, burst = tips + t, var + v, burst + b
        row['kp'][kp] = dict(sinad=m['sinad'], lock=m['picture'], tips=tips,
                             noise=var / tips if tips else float('nan'),
                             burst=burst / tips if tips else float('nan'))
    return row


def choose(row, rule, w=1.0):
    best, score = None, -1e9
    for kp in KPS:
        r = row['kp'][kp]
        if not r['tips'] or not np.isfinite(r['noise']):
            continue
        noise = max(r['noise'], 1e-3)
        s = {'noise': -noise, 'chroma': r['burst'] ** 2 / noise, 'blend': r['burst'] ** w / noise}[rule]
        if s > score:
            best, score = kp, s
    return best if best is not None else 0.85


def table_kp(dev, cnr):
    sys.path.insert(0, str(HERE))
    pts = [0, 2, 4, 8, 12, 20]
    rows = {'low': [70, 55, 50, 50, 50, 75], 'normal': [75, 60, 55, 55, 65, 75],
            'mid': [80, 70, 65, 65, 70, 80], 'wide': [85, 85, 80, 80, 80, 80]}
    cls = 'low' if dev <= 0.80 else 'normal' if dev <= 1.05 else 'mid' if dev <= 1.15 else 'wide'
    return round(np.interp(cnr, pts, rows[cls]) / 5) * 5 / 100


def report(rows, fit_seeds=None):
    fit = [r for r in rows if fit_seeds and r['seed'] in fit_seeds]
    test = [r for r in rows if not fit_seeds or r['seed'] not in fit_seeds]
    # Fit the blend weight on the fit seeds (true SINAD of the chosen kp).
    ws = np.arange(0.0, 4.01, 0.25)
    w_best = 1.0
    if fit:
        gains = [np.mean([r['kp'][choose(r, 'blend', w)]['sinad'] for r in fit]) for w in ws]
        w_best = float(ws[int(np.argmax(gains))])
    print(f'blend weight (fit on seeds {sorted(fit_seeds) if fit_seeds else "-"}): w = {w_best}')
    print('dev  C/N   oracle  kp0.85   table   noise  chroma   blend   (mean true luma SINAD of the chosen kp, '
          'held-out seeds)')
    agg = {k: [] for k in ('oracle', 'kp085', 'table', 'noise', 'chroma', 'blend')}
    for dev in DEVS:
        for cnr in CNRS:
            sub = [r for r in test if r['dev'] == dev and r['cnr'] == cnr]
            if not sub:
                continue
            v = {
                'oracle': np.mean([max(r['kp'][k]['sinad'] for k in KPS) for r in sub]),
                'kp085': np.mean([r['kp'][0.85]['sinad'] for r in sub]),
                'table': np.mean([r['kp'][table_kp(dev, cnr)]['sinad'] for r in sub]),
                'noise': np.mean([r['kp'][choose(r, 'noise')]['sinad'] for r in sub]),
                'chroma': np.mean([r['kp'][choose(r, 'chroma')]['sinad'] for r in sub]),
                'blend': np.mean([r['kp'][choose(r, 'blend', w_best)]['sinad'] for r in sub]),
            }
            for k in agg:
                agg[k].append(v[k])
            print(f'{dev:4.2f} {cnr:4.1f}  ' + '  '.join(f'{v[k]:6.2f}' for k in agg))
    print('mean           ' + '  '.join(f'{np.mean(agg[k]):6.2f}' for k in agg))
    # Lock check: the chosen kp must not lose goggle lock against kp 0.85.
    worst = min(r['kp'][choose(r, 'blend', w_best)]['lock'] - r['kp'][0.85]['lock'] for r in test)
    print(f'worst lock change of blend vs kp 0.85 (held-out): {worst:+.3f}')
    return w_best


def main(out_path):
    specs = [(d, c, s) for d in DEVS for c in CNRS for s in SEEDS]
    with mp.Pool(max(1, os.cpu_count() or 1)) as pool:
        rows = list(pool.imap_unordered(job, specs))
    for r in rows:      # json keys
        r['kp'] = {str(k): v for k, v in r['kp'].items()}
    Path(out_path).write_text(json.dumps(rows))
    load_and_report(out_path)


def load_and_report(path):
    rows = json.loads(Path(path).read_text())
    for r in rows:
        r['kp'] = {float(k): v for k, v in r['kp'].items()}
    seeds = sorted({r['seed'] for r in rows})
    report(rows, fit_seeds=set(seeds[:len(seeds) // 2]))


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == 'report':
        load_and_report(sys.argv[2])
    elif len(sys.argv) > 1:
        main(sys.argv[1])
    else:
        print(__doc__)
        sys.exit(2)
