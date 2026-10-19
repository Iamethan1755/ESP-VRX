"""C5VRX by Twotoz and contributors: OUTPUT amplitude feasibility study.

Uses the existing bit-exact U85Q/U85Q-LP arithmetic models and resistor DAC
model. The sync-referenced candidate receives IDEAL source frequency levels;
it is an upper bound, not a tested live estimator or HDZero model. In
particular sim.measure() normalises amplitude: its picture score alone cannot
establish analogue-input compatibility. This study separately reports loaded
sync depth, swing and clipping without fitting away the output gain.
"""
import json
import re
from pathlib import Path

import numpy as np
from scipy import signal as sg

import sim
from u85q_lp import lp_codes

HERE = Path(__file__).resolve().parent
CONDUCTANCE = np.array([1 / r for r in (8200, 3900, 2000, 1000, 470, 240)])
# Nominal 3.3 V GPIO, 200 ohm series, 75 ohm termination. GPIO impedance,
# tolerance, cable and actual goggle loading are NOT measured by this model.
VOLTS = sim.DAC_VOLTS * 3.3 / (CONDUCTANCE.sum() + 1 / 275) * 75 / 275


def transform(lut, gain=1., pivot=21., source_pivot=21.):
    """Only DAC bits 0..5 change, before LP; retain token/phase bits 6..15."""
    x = lut.astype(np.int32) & 63
    wanted = np.floor(pivot + gain * (x - source_pivot) + .5).astype(int)
    mapped = (lut & np.uint16(0xffc0)) | np.clip(wanted, 0, 63).astype(np.uint16)
    return mapped, (wanted < 0) | (wanted > 63)


def referenced(sync_hz, porch_hz, white_hz):
    """Ideal reference policy: 3-code tip, ~18-code sync, 3-code white margin.

    Sync depth and white headroom both bound the gain. The white level must
    be observed/validated in a future estimator; sync depth alone is unsafe.
    """
    sync, porch, white = (63 * (v + 6e6) / 15e6 for v in (sync_hz, porch_hz, white_hz))
    if not sync < porch < white:
        return 1., 21., 21.
    gain = min(2.5, 18 / (porch - sync), 57 / (white - sync))
    return max(.5, gain), 3., sync


def levels(c, codes):
    y = sg.sosfilt(sim.GOGGLE, VOLTS[codes])
    syncs, porches, whites = [], [], []
    # Truth labels are an evaluation oracle, never inputs to the live policy.
    for h in range(40, 250):
        start = sim.half_sample(c['standard'], 2 * h)
        if start + 2300 >= len(y):
            break
        syncs.append(np.median(y[start + 40:start + 140]))
        porches.append(np.median(y[start + 330:start + 370]))
        whites.extend(y[start + 450:start + 2300:8])
    tip, porch, white = float(np.median(syncs)), float(np.median(porches)), float(np.percentile(whites, 99))
    return {'sync_depth_v': porch - tip, 'swing_v': white - tip,
            'tip_v': tip, 'porch_v': porch, 'white_p99_v': white}


class SlowGuard:
    """Research-only time policy; no gain writes with invalid evidence.

    Requires 2 s uninterrupted agreement, 5 s between applications; context
    changes discard the candidate. Failed application must not publish state.
    """
    def __init__(self):
        self.active = 0
        self.pending = None
        self.last = -100.
        self.changes = []

    def observe(self, t, target, usable=True, context=0, apply_ok=True):
        key = (target, context)
        if not usable or target == self.active:
            self.pending = None
        elif self.pending is None or self.pending[0] != key:
            self.pending = (key, t)
        elif t - self.pending[1] >= 2 and t - self.last >= 5:
            self.last = t
            self.pending = None
            if apply_ok:
                self.active = target
                self.changes.append(t)
        return self.active


def checks():
    tables_text = (HERE.parent / 'main/kp_tables.h').read_text()
    words = np.array([int(x, 16) for x in re.findall(r'0x([0-9a-f]{4})', tables_text)], np.uint16)
    assert len(words) == 8 * 1024
    for gain, pivot in ((1, 21), (1.5, 13), (2, 16), (2.5, 19), (.8, 3)):
        mapped, _ = transform(words, gain, pivot)
        assert np.array_equal(mapped & 0xffc0, words & 0xffc0)
        if gain == 1:
            assert np.array_equal(mapped, words)
    # Decode transformed LUTs, rather than scaling the emitted LP samples.
    raw = np.random.default_rng(196).integers(0, 256, 8192, np.uint8)
    for lut in words.reshape(8, 1024):
        baseline = sim.u85q_codes(raw, lut)
        for gain, pivot in ((1, 21), (1.5, 13), (2, 16), (2.5, 19)):
            mapped, _ = transform(lut, gain, pivot)
            got = sim.u85q_codes(raw, mapped)
            wanted = np.clip(np.floor(pivot + gain * (baseline.astype(float) - 21) + .5), 0, 63)
            assert np.array_equal(got[2:], wanted[2:])
            if gain == 1:
                assert np.array_equal(lp_codes(raw, lut), lp_codes(raw, mapped))
    g = SlowGuard()
    for t in np.arange(0, 20, .05):
        g.observe(float(t), int(t * 10) % 2)
    assert not g.changes
    for t in np.arange(20, 25, .05):
        g.observe(float(t), 1, usable=False)
    assert not g.changes
    for t in np.arange(25, 35, .05):
        g.observe(float(t), 1, apply_ok=False)
    assert g.active == 0 and not g.changes
    for t in np.arange(35, 70, .05):
        g.observe(float(t), int((t - 35) // 6) % 2)
    assert g.changes and all(b - a >= 5 for a, b in zip(g.changes, g.changes[1:]))
    print('PASS: all 8 kp tables, DAC-only transform, 1x identity, jitter/invalid/failure/cooldown guards', flush=True)


def study():
    checks()
    rows = []
    lut = sim.u85q_lut()
    # Include issue #200's measured source levels plus narrow and wide VTXs.
    for standard in ('PAL', 'NTSC'):
        for case, dev, porch, white_scale in (
                ('narrow', .69, -1.5e6, 1.), ('normal', 1., -1e6, 1.), ('wide', 1.7, -1e6, 1.),
                ('issue200_levels', .69, -1.5e6, 5.2e6 / (.69 * sim.DEV_PP * 100 / 140))):
            sync = porch - dev * sim.DEV_PP * 40 / 140
            white = porch + dev * sim.DEV_PP * 100 / 140 * white_scale
            gain, pivot, source = referenced(sync, porch, white)
            for cnr in (6., 30.):
                c = sim.make_case(196, cnr, standard, dev, filter_hz=16e6 if dev >= 1.6 else 12e6,
                                  porch_hz=porch, fields=1, physical_adc=True, white_scale=white_scale)
                for name, fn in (('U85Q', sim.u85q_codes), ('U85Q-LP', lp_codes)):
                    baseline = fn(c['raw'], lut)
                    # Use the unscaled decoder to count samples affected by
                    # the requested affine map, not the fraction of LUT cells.
                    for mode, g, p, s in (('1x', 1, 21, 21), ('1.5x', 1.5, 13, 21),
                                          ('2x', 2, 16, 21), ('2.5x', 2.5, 19, 21),
                                          ('sync_reference_ideal', gain, pivot, source)):
                        mapped, _ = transform(lut, g, p, s)
                        noisy, clean = fn(c['raw'], mapped), fn(c['clean'], mapped)
                        m = sim.measure(c, noisy, clean)
                        wanted = p + g * (baseline[3000:-3000].astype(float) - s)
                        r = dict(case=case, standard=standard, deviation=dev, cnr_db=cnr, program=name, mode=mode,
                                 gain=g, sinad_db=m['sinad'], normalised_picture=m['picture'],
                                 pre_lp_clip_fraction=float(np.mean((wanted < -.5) | (wanted >= 63.5)))
                                 if name == 'U85Q' else None,
                                 **levels(c, clean))
                        rows.append(r)
                    print(f'{standard} {case} dev={dev} C/N={cnr} {name}: done', flush=True)
    out = HERE / 'data/output_level.json'
    out.write_text(json.dumps({'evidence': 'synthetic, ideal level reference; not HDZero/hardware qualification',
                               'loaded_dac_model': '3.3 V GPIO, nominal resistors, series 200 ohm + 75 ohm load',
                               'rows': rows}, indent=2) + '\n')
    print(f'wrote {out}', flush=True)


if __name__ == '__main__':
    import sys
    checks() if '--check' in sys.argv else study()
