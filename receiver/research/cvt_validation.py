"""C5VRX by Twotoz/contributors: held-out same-Q4 CVT validation.

Training/search seeds are 201..204; validation uses 211..214. Results are
synthetic waveform metrics, not recognisable moving-video or range proof.
All fixed candidates, CVT and floating reference share exactly one capture.
"""
import json
import multiprocessing as mp
import os
from pathlib import Path
import numpy as np
import sim
from situations import make_case as stress_case
from rescue_study import KPS, floating_q4
from kp_morph import build_lut
from u85_cvt import build_lut as cvt_lut
from u85q_lp import lp_codes
from output_level import levels

HERE=Path(__file__).resolve().parent
CVTS=((24,'origin_adapt'),(32,'sync_bias'))

def job(spec):
    dev,cn,seed,scenario=spec
    standard='PAL' if seed%2 else 'NTSC'
    filt=10e6 if dev<=1.05 else 16e6 if dev>=1.6 else 12e6
    baseline=.5 if dev<=.8 else .6 if dev<=1.05 else .85
    kwargs={'cfo+1':{'offset_hz':1e6},'cfo-1':{'offset_hz':-1e6},'dc1':{'dc':1.},
            **{f'fade{ms}ms':{'fade':(ms/1000, .003,-30)} for ms in (1,5,20)}}.get(scenario,{})
    c=stress_case(seed,cn,standard,dev,filt,fields=2,**kwargs) if kwargs else sim.make_case(
        seed,cn,standard,dev,filter_hz=filt,fields=2,physical_adc=True)
    candidates=[('U85Q',kp,build_lut(kp),False) for kp in KPS]
    candidates += [('LP',kp,build_lut(kp),True) for kp in KPS]
    candidates += [(f'CVT-{angles}-{law}'+('-LP' if lp else ''),None,cvt_lut(angles,law),lp)
                   for angles,law in CVTS for lp in (False,True)]
    candidates += [('FLOAT_Q4',None,None,False)]
    rows=[]
    for program,kp,lut,lp in candidates:
        fn=floating_q4 if lut is None else (lambda raw:lp_codes(raw,lut)) if lp else (lambda raw:sim.u85q_codes(raw,lut))
        noisy,clean=fn(c['raw']),fn(c['clean'])
        m=sim.measure(c,noisy,clean)
        truth=c['truth'][3000:-3000];region=c['region'][3000:-3000]
        error=m['video'][3000:-3000]-truth;burst=region==3;luma=region==1
        rows.append(dict(dev=dev,cnr_db=cn,seed=seed,standard=standard,scenario=scenario,
            gain_model='legacy fixed-through-fade' if kwargs else 'fixed physical ADC',
            program=program,kp=kp,baseline=program=='U85Q' and kp==baseline,
            sinad_db=m['sinad'],line_lock=m['picture'],
            burst_sinad_db=float(10*np.log10(max(np.var(truth[burst]),1e-12)/max(np.mean(error[burst]**2),1e-12))),
            large_luma_error_fraction=float(np.mean(abs(error[luma])>80)),
            clean_rail_fraction=float(np.mean((clean[3000:-3000]==0)|(clean[3000:-3000]==63))),
            **levels(c,clean)))
    return rows

def main():
    specs=[(dev,cn,seed,'clean') for dev in (.69,1.,1.4,1.7)
           for cn in (-2.,-1.,0.,.5,1.,1.5,2.,2.5,3.,4.,5.,6.,10.,20.) for seed in (211,212)]
    specs += [(dev,cn,seed,scenario) for dev in (1.,1.7) for cn in (4.,20.)
              for seed in (213,214) for scenario in ('cfo+1','cfo-1','dc1','fade1ms','fade5ms','fade20ms')]
    rows=[]
    with mp.Pool(2) as pool:
        for n,part in enumerate(pool.imap(job,specs),1):
            rows.extend(part);print(f'held-out CVT {n}/{len(specs)} {specs[n-1]}',flush=True)
    from rescue_study import rank
    report=rank(rows)
    (HERE/'data/cvt_validation.json').write_text(json.dumps(dict(
        evidence='held-out synthetic two-field same-Q4; offline oracle is not an online actuator; no moving-video/range qualification',
        candidates=CVTS,rows=rows,report=report),indent=2)+'\n')
    print('completed',len(rows),'same-Q4 results',flush=True)

if __name__=='__main__':
    os.environ.setdefault('OPENBLAS_NUM_THREADS','1')
    main()
