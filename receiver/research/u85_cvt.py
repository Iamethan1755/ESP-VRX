"""C5VRX by Twotoz/contributors: immutable, nonlinear U85-CVT research.

Extends the U85Q generator of PR #190 / kp_morph.py. Exactly 32 states and
32 tokens in LUT16, two lookups per 50 ns; no CPU sample loop, live LUT
writes or variable post-LP pole. Confidence is a quantised radius cue,
not RF C/N. Analytical laws are model-selected, not a range guarantee.
"""
import json
import math
import multiprocessing as mp
import os
import re
import sys
from pathlib import Path
import numpy as np

import sim
from kp_morph import build_lut as fixed_lut
from u85q_lp import lp_codes, emulate, assemble

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent/'main'
VARIANTS = [(a, law) for a in (28,24) for law in
            ('soft_lock','bounded','origin_hold','soft_lock_low','soft_lock_med','origin_adapt')]
VARIANTS += [(32, law) for law in ('origin_adapt','soft_lock_med','bounded','sync_bias','soft_lock_high')]
VARIANTS += [(28, law) for law in ('reject_wrap','gentle_wrap','late_wrap')]
DEFAULT = (32,'sync_bias')


def build_lut(angles=28, law='soft_lock'):
    assert (angles,law) in VARIANTS
    low = 32-angles
    ids = np.arange(1024)
    # Low byte of PAIR4411 is exact signed I4/Q4 sample A; upper two bits
    # contain sample B signs. No unavailable confidence metadata is assumed.
    word = np.zeros(1024,dtype=np.int64)
    for bit,source in enumerate(sim.PAIR_BITS): word |= ((ids>>bit)&1)<<source
    i = (((word>>4)&15)^8)-8+.5
    q = ((word&15)^8)-8+.5
    rad = np.hypot(i,q)
    ang = np.arctan2(q,i)
    token = np.floor((ang+np.pi)*angles/(2*np.pi)).astype(int)%angles
    uncertain = rad < (.8 if angles==28 else 2.3)
    if low:
        token[uncertain] = angles + (np.floor((ang[uncertain]+np.pi)*low/(2*np.pi)).astype(int)%low)
    phi = (np.arange(32)+.5)*2*np.pi/32-np.pi
    lut = np.zeros(1024,np.uint16)
    for state in range(32):
        for t in range(32):
            confidence_low = t>=angles
            observation = ((t-angles+.5)*2*np.pi/low-np.pi) if confidence_low else ((t+.5)*2*np.pi/angles-np.pi)
            error = (observation-phi[state]+np.pi)%(2*np.pi)-np.pi
            if confidence_low:
                gain = (0 if law=='origin_hold' else .3+.4*abs(error)/np.pi if law=='origin_adapt' else
                        .5 if law in ('soft_lock_med','reject_wrap','gentle_wrap','late_wrap') else .25)
            elif law=='soft_lock':
                gain = .55 + .30*min(1.,(abs(error)/1.6)**2)
            elif law=='bounded':
                gain = .65 + .20*min(1.,abs(error)/1.8)
            elif law=='soft_lock_low':
                gain = .4+.45*min(1.,(abs(error)/2.2)**2)
            elif law=='soft_lock_med':
                gain = .5+.35*min(1.,(abs(error)/2.5)**2)
            elif law=='origin_adapt':
                gain = .85
            elif law=='sync_bias':
                gain = .85 if error < 0 else .65 + .20*min(1.,abs(error)/2.)
            elif law=='soft_lock_high':
                gain = .70 + .15*min(1.,(abs(error)/2.)**2)
            elif law in ('reject_wrap','gentle_wrap','late_wrap'):
                # Preserve baseline tracking on plausible modulation. Test
                # suppression of near-wrap excursions separately: noise can
                # cause those, but genuine wide FM/CFO can also require them.
                threshold = 3.0 if law=='late_wrap' else 2.6
                gain = .85 if abs(error)<=threshold else .65 if law=='gentle_wrap' else .5
            else:
                gain = .75
            correction = gain*error
            if law=='bounded': correction = np.clip(correction,-1.8,2.6)
            nxt = int(np.floor((phi[state]+correction+np.pi)*32/(2*np.pi)))%32
            step = (phi[nxt]-phi[state]+np.pi)%(2*np.pi)-np.pi
            lo,hi = -6e6*2*np.pi*50e-9,9e6*2*np.pi*50e-9
            dac = int(np.rint(np.clip((step-lo)/(hi-lo)*63,0,63)))
            lut[state*32+t] = dac | (nxt<<6)
    return lut | (token.astype(np.uint16)<<11)


def source(variant=DEFAULT,lp=False):
    lut = build_lut(*variant)
    template = (MAIN/('u85q_lp.bsasm' if lp else 'u85q.bsasm')).read_text()
    template = re.sub(r'^lut .*$', 'lut '+' '.join(str(int(v)) for v in lut), template,flags=re.M)
    return '# C5VRX by Twotoz/contributors: EXPERIMENTAL U85-CVT '+str(variant)+'\n'+template


def generate(variant=DEFAULT):
    (HERE/'u85q_cvt.bsasm').write_text(source(variant))
    (HERE/'u85q_cvt_lp.bsasm').write_text(source(variant,lp=True))
    (HERE/'data/cvt_selected.json').write_text(json.dumps({'variant':variant,'experimental':True,
                                                        'law':'immutable observation-dependent gain; fixed two-bundle schedule'},indent=2)+'\n')
    print('generated',variant,flush=True)


def check():
    # MATCHED trackers have the same instruction geometry; their known LUT
    # levels can use the existing verified ladder and LP without losing filter.
    base=assemble(MAIN/'u85q.bsasm')[0]
    assert base==assemble(MAIN/'u85q_kp050.bsasm')[0]
    assert base==assemble(MAIN/'u85q_kp060.bsasm')[0]
    rng=np.random.default_rng(196201)
    raw=rng.integers(0,256,16384,np.uint8)
    for variant in VARIANTS:
        lut=build_lut(*variant)
        assert lut.shape==(1024,) and lut.dtype==np.uint16
        assert ((lut>>6)&31).max()<32 and (lut&63).max()<=63
        assert (lut>>11).max()<32
        # State/token address semantics remain legal, including random raw
        # quadrant transitions and long origin runs followed by recovery.
        codes=sim.u85q_codes(raw,lut)
        assert np.array_equal(codes[2::2],codes[3::2])
        for lp in (False,True):
            temp=HERE/('cvt_check_lp.bsasm' if lp else 'cvt_check.bsasm')
            try:
                temp.write_text(source(variant,lp))
                insts,meta,words=assemble(temp)
                assert len(insts)==8 and meta['lut_width_bits']==16 and len(words)==1024
                assert np.array_equal(np.array(words,np.uint16),lut)
                got=np.frombuffer(emulate(temp,raw.tobytes(),len(raw)-64),np.uint8)&63
                ref=lp_codes(raw,lut) if lp else sim.u85q_codes(raw,lut)
                offsets=[s for s in range(8) if np.array_equal(got[s:s+4000],ref[:4000])]
                assert offsets,variant
                offset=offsets[0]
                assert np.array_equal(got[offset:],ref[:len(got)-offset]),(variant,lp)
            finally:
                if temp.exists():temp.unlink()
    # The archived research programs are the selected candidate, regenerated.
    selected=tuple(json.loads((HERE/'data/cvt_selected.json').read_text())['variant'])
    assert (HERE/'u85q_cvt.bsasm').read_text()==source(selected),'regenerate: python u85_cvt.py generate'
    assert (HERE/'u85q_cvt_lp.bsasm').read_text()==source(selected,lp=True),'regenerate: python u85_cvt.py generate'
    print('PASS:',len(VARIANTS),'immutable CVT LUTs, U85Q/fixed-LP assembly and bit-exact emulator equality;',
          'research/u85q_cvt*.bsasm current for',selected,flush=True)


def job(spec):
    dev,cn,seed=spec
    standard='PAL' if seed%2 else 'NTSC'
    filt=10e6 if dev<=1.05 else 12e6
    c=sim.make_case(seed,cn,standard,dev,filter_hz=filt,fields=1,physical_adc=True)
    rows=[]
    cache_path=HERE/'data/cvt_search.json'
    cache=json.loads(cache_path.read_text())['rows'] if cache_path.exists() else []
    for variant in VARIANTS:
        prior=next((r for r in cache if tuple(r['variant'])==variant and r['dev']==dev and
                    r['cnr_db']==cn and r['seed']==seed),None)
        if prior:
            rows.append(prior)
            continue
        lut=build_lut(*variant)
        m=sim.measure(c,sim.u85q_codes(c['raw'],lut),sim.u85q_codes(c['clean'],lut))
        truth=c['truth'][3000:-3000];region=c['region'][3000:-3000]
        error=m['video'][3000:-3000]-truth;burst=region==3
        burst_sinad=10*np.log10(max(np.var(truth[burst]),1e-12)/max(np.mean(error[burst]**2),1e-12))
        rows.append(dict(variant=list(variant),dev=dev,cnr_db=cn,seed=seed,standard=standard,
                         sinad_db=m['sinad'],line_lock=m['picture'],burst_sinad_db=float(burst_sinad)))
    return rows


def search():
    specs=[(dev,cn,seed) for dev in (.69,1.,1.4) for cn in (1.,4.,30.) for seed in (201,202,203,204)]
    rows=[]
    with mp.Pool(2) as pool:
        for n,part in enumerate(pool.imap(job,specs),1):
            rows.extend(part);print('CVT search case',n,'/',len(specs),flush=True)
    old=json.loads((HERE/'data/rescue_study.json').read_text())['rows']
    summaries=[]
    for variant in VARIANTS:
        delta=[];violations=[]
        for dev,cn,seed in specs:
            r=next(r for r in rows if tuple(r['variant'])==variant and r['dev']==dev and r['cnr_db']==cn and r['seed']==seed)
            b=next(b for b in old if b['baseline'] and b['scenario']=='clean' and b['dev']==dev and b['cnr_db']==cn and b['seed']==seed)
            delta.append(r['sinad_db']-b['sinad_db'])
            if r['line_lock']<b['line_lock']-.02 or (cn>=20 and
                    (r['sinad_db']<b['sinad_db']-1 or r['burst_sinad_db']<b['burst_sinad_db']-1)):
                violations.append([dev,cn,seed])
        summaries.append(dict(variant=list(variant),violations=violations,
                              mean_delta_db=float(np.mean(delta)),worst_delta_db=float(min(delta))))
    best=min(summaries,key=lambda s:(len(s['violations']),-s['worst_delta_db'],-s['mean_delta_db']))
    result={'evidence':'analytical candidate search on synthetic same-Q4, against best existing matched filter/profile; no hardware picture proof',
            'rows':rows,'summary':summaries,'selected':best}
    (HERE/'data/cvt_search.json').write_text(json.dumps(result,indent=2)+'\n')
    generate(tuple(best['variant']))
    for s in summaries:print(s,flush=True)
    print('selected experimental candidate:',best,flush=True)


if __name__=='__main__':
    os.environ.setdefault('OPENBLAS_NUM_THREADS','1')
    cmd=sys.argv[1] if len(sys.argv)>1 else 'search'
    if cmd=='check':check()
    elif cmd=='generate':generate()
    else:search()
