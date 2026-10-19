"""C5VRX by Twotoz and contributors: Multi-Rescue proposal same-Q4 study.

All 8 U85Q kp LUTs, LP and a floating SAME-Q4 reference see identical raw
bytes, behind the best existing matched filter. Ranking is offline/oracle,
not counterfactual online measurements or a demonstrated moving-image range
threshold. Two-bundle hardware still executes only one candidate at a time.
"""
import json
import multiprocessing as mp
import os
from pathlib import Path

import numpy as np

import sim
from kp_morph import build_lut
from u85q_lp import lp_codes
from output_level import levels
from situations import make_case as stress_case

HERE = Path(__file__).resolve().parent
KPS = (.85, .80, .75, .70, .65, .60, .55, .50)


def floating_q4(raw):
    """Floating adjacent FM from every captured Q4 sample, then 2:1 average.
    Includes origin samples (zero-midrise centres never exactly zero); it is
    a host reference, not a feasible replacement hardware architecture.
    """
    i = ((raw.astype(np.int16) >> 4) ^ 8) - 8 + .5
    q = ((raw.astype(np.int16) & 15) ^ 8) - 8 + .5
    z = i + 1j * q
    delta = np.angle(z[1:] * np.conj(z[:-1])) * sim.FS / (2 * np.pi)
    n = len(delta) // 2
    hz = delta[:2*n].reshape(-1, 2).mean(axis=1)
    code = np.clip(np.floor((hz + 6e6) * 63 / 15e6 + .5), 0, 63).astype(np.uint8)
    out = np.zeros(len(raw), np.uint8)
    out[2:2 + 2*n] = np.repeat(code, 2)[:len(out)-2]
    return out


def job(spec):
    dev, cnr, seed, scenario = spec
    standard = 'PAL' if seed % 2 else 'NTSC'
    filt = 10e6 if dev <= 1.05 else 16e6 if dev >= 1.6 else 12e6
    baseline_kp = .5 if dev <= .8 else .6 if dev <= 1.05 else .85
    kwargs = {'cfo+0.75': {'offset_hz': .75e6}, 'cfo-0.75': {'offset_hz': -.75e6},
              'dc1': {'dc': 1.}, 'fade3ms': {'fade': (3e-3, 10e-3, -30)}}.get(scenario, {})
    if kwargs:
        c = stress_case(seed, cnr, standard, dev, filt, fields=1, **kwargs)
        gain_model = 'legacy fixed-through-fade model; quantised synthetic Q4'
    else:
        c = sim.make_case(seed, cnr, standard, dev, filter_hz=filt, fields=1, physical_adc=True)
        gain_model = 'fixed physical ADC gain across clean/noisy pair'
    rows = []
    for kp in (*KPS, None):
        lut = build_lut(kp) if kp is not None else None
        for program in (('U85Q', 'LP') if kp is not None else ('FLOAT_Q4',)):
            fn = floating_q4 if kp is None else (lambda raw: sim.u85q_codes(raw, lut)) if program == 'U85Q' else (lambda raw: lp_codes(raw, lut))
            noisy, clean = fn(c['raw']), fn(c['clean'])
            m = sim.measure(c, noisy, clean)
            region = c['region'][3000:-3000]
            truth = c['truth'][3000:-3000]
            error = m['video'][3000:-3000] - truth
            burst = region == 3
            luma = region == 1
            burst_sinad = 10 * np.log10(max(np.var(truth[burst]), 1e-12) /
                                      max(np.mean(error[burst] ** 2), 1e-12))
            rows.append(dict(dev=dev, cnr_db=cnr, seed=seed, standard=standard, scenario=scenario,
                             gain_model=gain_model, filter_mhz=int(filt/1e6), kp=kp, program=program,
                             baseline=program == 'U85Q' and kp == baseline_kp,
                             sinad_db=m['sinad'], line_lock=m['picture'], burst_sinad_db=float(burst_sinad),
                             large_luma_error_fraction=float(np.mean(abs(error[luma]) > 80)),
                             clean_rail_fraction=float(np.mean((clean[3000:-3000] == 0) | (clean[3000:-3000] == 63))),
                             **levels(c, clean)))
    return rows


def rank(rows):
    groups = {}
    for r in rows:
        groups.setdefault((r['dev'], r['cnr_db'], r['scenario']), []).append(r)
    reports = []
    for key, group in sorted(groups.items()):
        candidates = {}
        for r in group:
            candidates.setdefault((r['program'], r['kp']), []).append(r)
        baseline = [r for r in group if r['baseline']]
        base_lock = min(r['line_lock'] for r in baseline)
        base_sinad = min(r['sinad_db'] for r in baseline)
        base_burst = min(r['burst_sinad_db'] for r in baseline)
        base_rails = max(r['clean_rail_fraction'] for r in baseline)
        scores = []
        for (program, kp), values in candidates.items():
            if program == 'FLOAT_Q4': continue  # reference is not a hardware candidate
            lock = min(v['line_lock'] for v in values)
            sinad = min(v['sinad_db'] for v in values)
            burst = min(v['burst_sinad_db'] for v in values)
            rails = max(v['clean_rail_fraction'] for v in values)
            # Hard absolute lock gate, colour/headroom no worse than matched,
            # plus <=1 dB strong-signal penalty. Report NONE if none qualifies.
            eligible = lock >= .9 and lock >= base_lock - .02 and burst >= base_burst - 1 and rails <= base_rails + .005
            if key[1] >= 20: eligible &= sinad >= base_sinad - 1
            scores.append(dict(program=program, kp=kp, eligible=eligible, worst_lock=lock,
                               worst_sinad_db=sinad, worst_burst_sinad_db=burst))
        allowed = [s for s in scores if s['eligible']]
        pick = max(allowed, key=lambda s: s['worst_sinad_db']) if allowed else None
        reports.append(dict(dev=key[0], cnr_db=key[1], scenario=key[2], seeds=len(baseline),
                            baseline_worst_lock=base_lock, baseline_worst_sinad_db=base_sinad,
                            selected=pick, delta_vs_matched_db=pick['worst_sinad_db'] - base_sinad if pick else None,
                            candidates=scores))
    return reports


def main():
    specs = [(dev, cnr, seed, 'clean') for dev in (.69, 1., 1.4, 1.7)
             for cnr in (1., 4., 30.) for seed in (201, 202, 203, 204)]
    specs += [(dev, 4., seed, scenario) for dev in (1., 1.7) for seed in (201, 202)
              for scenario in ('cfo+0.75', 'cfo-0.75', 'dc1', 'fade3ms')]
    rows = []
    with mp.Pool(2) as pool:
        for n, part in enumerate(pool.imap(job, specs), 1):
            rows.extend(part)
            print(f'case {n}/{len(specs)}: {specs[n-1]} done', flush=True)
    report = rank(rows)
    out = HERE / 'data/rescue_study.json'
    out.write_text(json.dumps({'evidence': 'synthetic same-Q4 offline ranking; no moving-picture/HDZero/range proof',
                               'rows': rows, 'report': report}, indent=2) + '\n')
    for r in report:
        p = r['selected']
        print(f"{r['dev']} C/N={r['cnr_db']} {r['scenario']}: " +
              (f"{p['program']} kp={p['kp']} worst_lock={p['worst_lock']:.3f} delta_vs_MATCHED={r['delta_vs_matched_db']:+.2f}dB"
               if p else 'NONE meets lock/colour/headroom gates'), flush=True)
    print('wrote', out, flush=True)


if __name__ == '__main__':
    os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
    main()
