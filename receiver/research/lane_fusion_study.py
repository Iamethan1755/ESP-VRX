"""Executable search/replay/export, C5VRX by Twotoz and contributors.

No training. Probe context is known synthetically; firmware cannot infer it
from arbitrary folded IQ without further evidence. No range claim.
"""
import argparse
import json
from pathlib import Path
import re
import numpy as np
from scipy import signal as sg
import iq_lane_fusion as F
import sim


def describe(row):
    p = row['plan']
    return dict(name=p.name, i=p.i, q=p.q, gain=row['gain'],
                risk_hz2=row['risk_hz2'], screen=row['screen'], risks=row['risks'])


def calibration(y, truth):
    best = None
    for lag in range(-8, 9):
        x = y[256 + lag:len(y) - 256 + lag]
        target = truth[256:len(y) - 256]
        matrix = np.column_stack((x, np.ones(len(x))))
        coeff = np.linalg.lstsq(matrix, target, rcond=None)[0]
        error = np.mean((matrix @ coeff - target) ** 2)
        if best is None or error < best[0]:
            best = error, lag, coeff
    return best[1:]


def sinad(y, truth, cal):
    lag, (gain, offset) = cal
    target = truth[256:len(y) - 256]
    error = y[256 + lag:len(y) - 256 + lag] * gain + offset - target
    return float(10 * np.log10(np.var(target) / max(np.mean(error ** 2), 1e-30)))


def probe(samples, n):
    rng = np.random.default_rng(196)
    rows = []
    filt = sg.butter(4, 5e6, fs=40e6, output='sos')
    for radius, sigma, stress in ((32, 8, 'nominal'), (64, 8, 'nominal'),
                                 (160, 8, 'fold'), (64, 24, 'weak'),
                                 (64, 8, 'fade'), (64, 8, 'echo')):
        t = np.arange(samples) / 40e6
        hz = -1e6 + 2e6 * np.sin(2 * np.pi * .7e6 * t) + .6e6 * np.sin(2 * np.pi * 2.1e6 * t)
        clean = radius * np.exp(2j * np.pi * np.cumsum(hz) / 40e6)
        if stress == 'fade':
            clean[samples // 3:samples // 3 + samples // 12] *= .03
        if stress == 'echo':
            clean += .7 * np.r_[np.zeros(8), clean[:-8]]
        noisy = clean + sigma * (rng.normal(size=samples) + 1j * rng.normal(size=samples))
        ctx = F.Context((float(radius),), sigma=float(sigma))
        choice = F.select(F.SHADOW_PLANS, (ctx,), n=n)
        selected = choice['best']['plan']
        truth = sg.sosfilt(filt, hz)
        baseline = sim.video_out(sim.u85q_codes(F.capture(noisy)))
        baseline_clean = sim.video_out(sim.u85q_codes(F.capture(clean)))
        decoded = F.decode(F.capture(noisy, selected), selected, ctx, n)
        decoded_clean = F.decode(F.capture(clean, selected), selected, ctx, n)
        y = sg.sosfilt(filt, decoded.hz)
        yc = sg.sosfilt(filt, decoded_clean.hz)
        row = dict(radius=radius, sigma=sigma, stress=stress,
                   proposed=describe(choice['best']),
                   u85q_sinad=sinad(baseline, truth, calibration(baseline_clean, truth)),
                   reference_sinad=sinad(y, truth, calibration(yc, truth)),
                   ambiguity=float(np.mean(decoded.ambiguity[256:])),
                   model_context='known synthetic radius/noise; mismatch during fade/echo')
        rows.append(row)
        print(json.dumps(row), flush=True)
    return rows


def compressed_lut(plan, ctx, n):
    """Fixed-context compressed experiment, NOT the full posterior reference.

    Preserve two-bundle U85Q transport/transition plane; analytic first decoder
    plane maps the selected physical bitset to phase tokens. Ambiguous cells
    use the original four low-gain quadrant tokens. No live LUT rewrite.
    """
    if plan.i[-1] != 9 or plan.q[-1] != 9 or ctx.dc_i or ctx.dc_q or ctx.skew_rad or ctx.iq_ratio != 1:
        raise ValueError('export requires sign-on-bit3 and centred isotropic context')
    base = sim.u85q_lut().astype(np.int64)
    L = F.likelihood(plan, ctx, n)
    angles = F.phases(n)
    tokens = []
    for raw in range(256):
        weights = L[raw] / max(L[raw].sum(), 1e-300)
        moment = weights @ np.exp(1j * angles)
        if abs(moment) < .85:
            # These are actual ADC signs, not invented phase measurements.
            i, q = (15 if raw & 128 else 0), (15 if raw & 8 else 0)
        else:
            theta = angles[int(np.argmax(weights))]
            i = int(np.clip(np.floor(4.2 * np.cos(theta)), -8, 7)) & 15
            q = int(np.clip(np.floor(4.2 * np.sin(theta)), -8, 7)) & 15
        tokens.append((i, q))
    lut = base & 2047
    for address in range(1024):
        raw = ((address & 15) << 4) | ((address >> 4) & 15)
        i, q = tokens[raw]
        canonical = i | (q << 4) | (address & 768)
        lut[address] |= (base[canonical] >> 11) << 11
    return lut.astype(np.uint16)


def export_program(plan, ctx, path, n):
    template = (Path(__file__).resolve().parents[1] / 'main' / 'u85q.bsasm').read_text()
    lut = compressed_lut(plan, ctx, n)
    text = re.sub(r'^lut [0-9 ]+$', 'lut ' + ' '.join(map(str, lut)), template, flags=re.M)
    preamble = '# IQLaneFusion fixed-context COMPRESSED EXPERIMENT, not live adaptive firmware.\n'
    preamble += '# Route I=' + str(plan.i) + ' Q=' + str(plan.q) + ' before starting TX.\n'
    preamble += '# Context=' + repr(ctx) + '; full posterior does not fit this phase-only state.\n'
    Path(path).write_text(preamble + text)
    return dict(words=len(lut), lut_bytes=lut.nbytes, bundles_per_50ns=2,
                state_bits=5, observation_bits=5, source='Q4/I4@40M',
                unique_dac_hz=20e6, physical_tx_hz=40e6,
                status='encoding candidate; throughput and picture unproved')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=('search', 'probe', 'export', 'video'))
    p.add_argument('--all-bits', action='store_true')
    p.add_argument('--symmetric', action='store_true')
    p.add_argument('--radius', type=float, default=64.)
    p.add_argument('--sigma', type=float, default=8.)
    p.add_argument('--phases', type=int, default=64)
    p.add_argument('--samples', type=int, default=4096)
    p.add_argument('--output')
    p.add_argument('--plan', choices=tuple(x.name for x in F.SHADOW_PLANS), default='mixed468')
    args = p.parse_args()
    ctx = F.Context((args.radius,), sigma=args.sigma)
    if args.action == 'search':
        result = F.select(F.all_plans(not args.all_bits, args.symmetric), (ctx,), n=args.phases)
        report = dict(candidates=result['candidates'], ranked=[describe(r) for r in result['ranked']],
                      guarantee=result['guarantee'], trained=False)
        print(json.dumps(report, indent=2))
    elif args.action == 'probe':
        report = dict(evidence='synthetic FM probe, not displayed PAL/NTSC or measured range',
                      trained=False, phases=args.phases, rows=probe(args.samples, args.phases))
    elif args.action == 'export':
        if not args.output:
            p.error('export requires --output path.bsasm')
        report = export_program(next(x for x in F.SHADOW_PLANS if x.name == args.plan), ctx, args.output, args.phases)
        print(json.dumps(report))
        return
    else:
        # Full moving PAL field; actual ADC folding and one fixed gain. This
        # validates the reference only, NOT the compressed BS experiment.
        c = sim.make_case(196, 6., fields=1, filter_hz=12e6, physical_adc=True)
        radius = float(np.sqrt(np.mean(abs(c['clean_adc']) ** 2)))
        sigma = float(np.std((c['adc'] - c['clean_adc']).real))
        context = F.Context((radius,), sigma=sigma)
        baseline = sim.video_out(sim.u85q_codes(c['raw']))
        basecal = sim.calibrate(sim.video_out(sim.u85q_codes(c['clean'])), c['truth'], c['region'])
        print('decoding moving PAL field', len(c['raw']), 'samples', flush=True)
        result = F.decode(c['raw'], F.ULTRA, context, args.phases)
        clean = F.decode(c['clean'], F.ULTRA, context, args.phases)
        filt = sg.butter(4, 5e6, fs=40e6, output='sos')
        y, yc = sg.sosfilt(filt, result.hz), sg.sosfilt(filt, clean.hz)
        cal = sim.calibrate(yc, c['truth'], c['region'])
        a, b = sim.apply_cal(baseline, basecal), sim.apply_cal(y, cal)
        mask = (c['region'] == 1) & (np.arange(len(a)) > 3000)
        def score(x):
            return float(10 * np.log10(np.var(c['truth'][mask]) / np.mean((x[mask] - c['truth'][mask]) ** 2)))
        report = dict(evidence='synthetic PAL field; oracle amplitude/noise context; not measured range',
                      physical_adc=True, gain=c['adc_gain'], samples=len(a),
                      u85q=dict(sinad=score(a), sync=sim.goggle(a, c['truth'], 'PAL')),
                      reference=dict(sinad=score(b), sync=sim.goggle(b, c['truth'], 'PAL'),
                                     ambiguity=float(np.mean(result.ambiguity))))
        print(json.dumps(report), flush=True)
    if args.output:
        Path(args.output).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
