"""Does the kp/LP ladder reach the range edge? (C5VRX by Twotoz and the
C5VRX contributors; extends best_combo.py and kp_ladder.h, README section 18.)

control.c gated the ladder and its two-second target confirmation with the
AFC window gate, which includes afc2_envelope_stationary(): every 32-sample
block power within 2.5x of its neighbour. That test was written for the AFC.
Near the FM threshold receiver noise alone breaks it, so many windows read
"unusable" exactly where the ladder is meant to work. The ladder needs 1 s of
uninterrupted usable windows to enter U85Q-LP, and the switch guard wanted
the exact (level, program) target unchanged for 2 s more, while the ladder
steps one kp level every 0.2-1 s.

    python ladder_gate.py rates data/ladder_gate_rates.json
        pass rate of the stationarity gate per C/N, filter class, deviation
        (sim.py IQ, firmware power table, 4092-byte control windows; ~2 min)
    python ladder_gate.py study data/ladder_gate_rates.json data/ladder_gate.json
        compiles ladder_gate_trace.c (the real kp_ladder.h / kp_switch_guard.h
        in control.c's order), 16 seeds, and scores the configuration that
        actually runs with the best_combo.json / best_combo_10mhz.json means:
        - fly-out at 0.25 dB/s from 14 to -1 dB C/N: range limit (worse of
          luma SINAD 0 dB and goggle lock 0.9, as best_combo.py);
        - arrive at the edge (14 dB to X in 3 s) and stay, without and with a
          +-2 dB / 2 s or +-4 dB / 4 s fade: share of windows with a picture
          (lock >= 0.9 and SINAD >= 0 dB), and the mean C/N where it is 50 %.
        Modes: before (stationarity gate, exact target), no gate, toward
        (gate kept, directional confirm), after (control.c now), oracle (the
        curve's program and kp at the true C/N, no guards).
    python ladder_gate.py check data/ladder_gate_rates.json
        CI: 8 seeds, the 50 % point of "after" against "before" and oracle.

Synthetic numbers (sim.py and the best_combo model); not measured range.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
RATE_CNRS = (-3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 8, 10)
CASES = {   # name: (filter MHz, deviation, best_combo data, narrow profile)
    '12MHz dev0.69': (12, 0.69, 'best_combo.json', False),
    '12MHz dev1.0': (12, 1.0, 'best_combo.json', False),
    '12MHz dev1.4': (12, 1.4, 'best_combo.json', False),
    '10MHz dev0.69 (MATCHED)': (10, 0.69, 'best_combo_10mhz.json', True),
    '10MHz dev1.0 (MATCHED)': (10, 1.0, 'best_combo_10mhz.json', True),
}
SEEDS = range(int(os.environ.get('SEEDS', '16')))
RAMP = (140, -10, 60.0)          # C/N x10 from, to, seconds: 0.25 dB/s fly-out
NOISE_X10 = 30                   # +-3 dB per-window estimate jitter (test_kp_ladder.c)


def _power():
    i = ((np.arange(256) >> 4) ^ 8) - 8
    q = ((np.arange(256) & 15) ^ 8) - 8
    return ((2 * i + 1) ** 2 + (2 * q + 1) ** 2 + 2) // 4


POWER = _power()


def stationary(window):
    """afc2_envelope_stationary (main/afc.h), 32-sample blocks."""
    s = POWER[window[:len(window) // 32 * 32]].reshape(-1, 32).sum(1)
    if (s < 64).any():
        return False
    a, b = s[:-1], s[1:]
    return not ((b * 2 > a * 5) | (a * 2 > b * 5)).any()


def rates(out):
    sys.path.insert(0, str(HERE))
    import sim
    table = {}
    for name, (fmhz, dev, _, _) in CASES.items():
        row = []
        for cn in RATE_CNRS:
            ok = n = 0
            for seed in range(4):
                c = sim.make_case(1000 * seed + 17, cn, ('PAL', 'NTSC')[seed % 2], dev, filter_hz=fmhz * 1e6,
                                  fields=1)
                raw = c['raw']
                for k in range(0, len(raw) - 4092, 4092):
                    n += 1
                    ok += stationary(raw[k:k + 4092])
            row.append(ok / n)
        table[name] = row
        print(f'{name:24s} ' + ' '.join(f'{cn:+d}dB:{p:.2f}' for cn, p in zip(RATE_CNRS, row)))
    Path(out).write_text(json.dumps(dict(cnrs=RATE_CNRS, rates=table), indent=1))


def model(path, dev):
    rows = [r for r in json.loads((HERE / 'data' / path).read_text()) if abs(r['dev'] - dev) < 1e-6]
    cnrs = sorted({r['cnr'] for r in rows})
    out = {}
    for prog in ('U85Q', 'LP'):
        for kp in sorted({r['kp'] for r in rows}):
            s = [np.mean([r['sinad'] for r in rows if r['prog'] == prog and r['kp'] == kp and r['cnr'] == c])
                 for c in cnrs]
            l = [np.mean([r['lock'] for r in rows if r['prog'] == prog and r['kp'] == kp and r['cnr'] == c])
                 for c in cnrs]
            out[(prog, round(kp, 2))] = (np.array(s), np.array(l))
    return np.array(cnrs), out


def crossing(xs, ys, level):
    for k in range(1, len(xs)):
        if ys[k - 1] < level <= ys[k]:
            return xs[k - 1] + (level - ys[k - 1]) * (xs[k] - xs[k - 1]) / (ys[k] - ys[k - 1])
    return xs[0] if ys[0] >= level else float('nan')


def score(trace_rows, cnrs, table):
    """Binned (0.5 dB) mean SINAD / lock of the running configuration."""
    bins = {}
    for _, cn_x10, level, lp in trace_rows:
        cn = cn_x10 / 10
        s, l = table[('LP' if lp else 'U85Q', round(0.85 - 0.05 * level, 2))]
        b = round(cn * 2) / 2
        acc = bins.setdefault(b, [0.0, 0.0, 0, 0])
        acc[0] += np.interp(cn, cnrs, s)
        acc[1] += np.interp(cn, cnrs, l)
        acc[2] += 1
        acc[3] += lp
    xs = sorted(bins)
    sinad = [bins[x][0] / bins[x][2] for x in xs]
    lock = [bins[x][1] / bins[x][2] for x in xs]
    lp_share = {x: bins[x][3] / bins[x][2] for x in xs}
    return max(crossing(xs, sinad, 0.0), crossing(xs, lock, 0.9)), xs, sinad, lp_share


MODES = ((0, 'before'), (1, 'no gate'), (3, 'toward'), (2, 'after'), (9, 'oracle'))
HOVER_DB = (0, 1, 2, 3, 4, 5, 6, 8)
FADES = ((0, 0.0), (20, 2.0), (40, 4.0))   # sine amplitude x10, period s


def run_trace(exe, mode, dev, narrow, traj, fade, table_str):
    rows = []
    for seed in SEEDS:
        p = subprocess.run([str(exe), str(mode), str(int(round(dev * 100))), str(int(narrow)), str(seed + 1), traj,
                            str(fade[0]), str(fade[1]), str(NOISE_X10), table_str],
                           check=True, capture_output=True, text=True)
        rows += [tuple(int(v) for v in ln.split()) for ln in p.stdout.splitlines()]
    return rows


def usable_share(rows, cnrs, table, after_ms=15000):
    """Share of windows whose running configuration shows a picture (model
    lock >= 0.9 and luma SINAD >= 0 dB at the true C/N), and mean SINAD."""
    ok = n = 0
    sinad = 0.0
    for t, cn_x10, level, lp in rows:
        if t < after_ms:
            continue
        s, l = table[('LP' if lp else 'U85Q', round(0.85 - 0.05 * level, 2))]
        cn = cn_x10 / 10
        si, lo = np.interp(cn, cnrs, s), np.interp(cn, cnrs, l)
        ok += si >= 0 and lo >= 0.9
        sinad += si
        n += 1
    return ok / n, sinad / n


def study(rates_path, out):
    r = json.loads(Path(rates_path).read_text())
    exe = Path(tempfile.mkdtemp()) / 'ladder_gate_trace'
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{HERE.parent / "main"}',
                    str(HERE / 'ladder_gate_trace.c'), '-lm', '-o', str(exe)], check=True)
    result = {}
    for name, (fmhz, dev, data, narrow) in CASES.items():
        table_str = ','.join(f'{cn * 10}:{p:.4f}' for cn, p in zip(r['cnrs'], r['rates'][name]))
        cnrs, table = model(data, dev)
        res = result[name] = {}
        print(f'\n=== {name} ===')
        # Fly-out: range limit of the running configuration along the ramp.
        traj = f'0:{RAMP[0]},{RAMP[2]}:{RAMP[1]}'
        line = []
        for mode, label in MODES:
            lim = score(run_trace(exe, mode, dev, narrow, traj, (0, 0.0), table_str), cnrs, table)[0]
            res[f'flyout {label}'] = lim
            line.append(f'{label} {lim:5.2f}')
        fixed = table[('U85Q', 0.85)]
        base = max(crossing(list(cnrs), list(fixed[0]), 0.0), crossing(list(cnrs), list(fixed[1]), 0.9))
        res['U85Q kp0.85 fixed'] = base
        print(f'fly-out 0.25 dB/s, range limit dB: fixed kp0.85 {base:5.2f} | ' + ' | '.join(line))
        # Hover at the edge with fading: picture share per mean C/N, and the
        # mean C/N where half of the windows still show a picture.
        for fade in FADES:
            print(f'hover, fade +-{fade[0] / 10:g} dB / {fade[1]:g} s: picture share (mean SINAD) per mean C/N')
            for mode, label in MODES:
                shares, sinads = [], []
                for x in HOVER_DB:
                    sh, si = usable_share(run_trace(exe, mode, dev, narrow, f'0:140,10:140,13:{x * 10},60:{x * 10}',
                                                    fade, table_str), cnrs, table)
                    shares.append(sh)
                    sinads.append(si)
                half = crossing(list(HOVER_DB), shares, 0.5)
                res[f'hover fade{fade[0]} {label}'] = dict(share=shares, sinad=sinads, half=half)
                print(f'  {label:8s} 50%-picture at {half:5.2f} dB | ' +
                      ' '.join(f'{x}:{sh:.2f}({si:+.1f})' for x, sh, si in zip(HOVER_DB, shares, sinads)))
    Path(out).write_text(json.dumps(result, indent=1))


def check(rates_path):
    """CI: arriving at the edge, the fixed control loop reaches the curve."""
    global SEEDS
    SEEDS = range(8)
    r = json.loads(Path(rates_path).read_text())
    exe = Path(tempfile.mkdtemp()) / 'ladder_gate_trace'
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{HERE.parent / "main"}',
                    str(HERE / 'ladder_gate_trace.c'), '-lm', '-o', str(exe)], check=True)
    failures = 0
    for name in ('12MHz dev1.0', '10MHz dev0.69 (MATCHED)'):
        fmhz, dev, data, narrow = CASES[name]
        table_str = ','.join(f'{cn * 10}:{p:.4f}' for cn, p in zip(r['cnrs'], r['rates'][name]))
        cnrs, table = model(data, dev)
        for fade in FADES[:2]:
            half = {}
            for mode, label in ((0, 'before'), (2, 'after'), (9, 'oracle')):
                shares = [usable_share(run_trace(exe, mode, dev, narrow, f'0:140,10:140,13:{x * 10},60:{x * 10}',
                                                 fade, table_str), cnrs, table)[0] for x in HOVER_DB]
                half[label] = crossing(list(HOVER_DB), shares, 0.5)
            ok = half['after'] <= half['oracle'] + 0.3 and half['after'] <= half['before'] - 0.3
            failures += not ok
            print(f'{"ok" if ok else "FAIL"} {name} fade +-{fade[0] / 10:g} dB: 50 %-picture before '
                  f'{half["before"]:.2f}, after {half["after"]:.2f}, oracle {half["oracle"]:.2f} dB')
    if failures:
        sys.exit(1)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'rates':
        rates(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'study':
        study(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == 'check':
        check(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
