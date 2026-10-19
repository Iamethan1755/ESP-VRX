"""C5VRX by Twotoz/contributors: 64-state/16-token CVT tradeoff.

Extends U85Q's two-bundle LUT, not the rejected four-bundle Phase6 detector.
Six state bits and four token bits still address 1024 LUT16 entries. This
plain tracker has no claimed post-LP; the current fixed-LP packing needs
five phase bits and cannot simply be copied to this six-bit-state layout.
"""
import json,math,multiprocessing as mp,os,sys
from pathlib import Path
import numpy as np
import sim
from u85q_lp import assemble,emulate
HERE=Path(__file__).resolve().parent;MAIN=HERE.parent/'main'
VARIANTS=((16,'fixed85'),(16,'fixed60'),(16,'soft'),(16,'wrap'),(14,'origin'))

def lut(angles,law):
    ids=np.arange(1024);word=np.zeros(1024,np.int64)
    for bit,source in enumerate(sim.PAIR_BITS):word|=((ids>>bit)&1)<<source
    i=(((word>>4)&15)^8)-8+.5;q=((word&15)^8)-8+.5
    angle=np.arctan2(q,i);low=16-angles
    token=np.floor((angle+np.pi)*angles/(2*np.pi)).astype(int)%angles
    if low:
        uncertain=np.hypot(i,q)<1.6
        token[uncertain]=angles+(np.floor((angle[uncertain]+np.pi)*low/(2*np.pi)).astype(int)%low)
    phi=(np.arange(64)+.5)*2*np.pi/64-np.pi
    out=np.zeros(1024,np.uint16)
    for state in range(64):
        for t in range(16):
            confidence_low=t>=angles
            observation=((t-angles+.5)*2*np.pi/low-np.pi) if confidence_low else ((t+.5)*2*np.pi/angles-np.pi)
            error=(observation-phi[state]+np.pi)%(2*np.pi)-np.pi
            gain=.3 if confidence_low else .6 if law=='fixed60' else .6+.25*min(1.,abs(error)/2.6) if law=='soft' else .5 if law=='wrap' and abs(error)>2.6 else .85
            nxt=int(np.floor((phi[state]+gain*error+np.pi)*64/(2*np.pi)))%64
            step=(phi[nxt]-phi[state]+np.pi)%(2*np.pi)-np.pi
            lo,hi=-6e6*2*np.pi*50e-9,9e6*2*np.pi*50e-9
            dac=int(np.rint(np.clip((step-lo)/(hi-lo)*63,0,63)))
            out[state*16+t]=dac|(nxt<<6)
    return out|(token.astype(np.uint16)<<12)

def codes(raw,table):return sim._pair_codes(raw,table,sim.PAIR_BITS,4)

def source(variant):
    import re
    text=(MAIN/'u85q.bsasm').read_text()
    text=re.sub(r'^lut .*$', 'lut '+' '.join(str(int(v)) for v in lut(*variant)),text,flags=re.M)
    text=text.replace('set 16..20 L11..L15','set 16..19 L12..L15').replace('set 21..25 O26..O30','set 20..25 O26..O31')
    return '# C5VRX by Twotoz/contributors: EXPERIMENTAL CVT64 '+str(variant)+'\n'+text

def check():
    raw=np.random.default_rng(19664).integers(0,256,16384,np.uint8)
    path=HERE/'cvt64_check.bsasm'
    try:
        for v in VARIANTS:
            path.write_text(source(v));inst,meta,words=assemble(path)
            assert len(inst)==8 and meta['lut_width_bits']==16 and len(words)==1024
            assert np.array_equal(np.array(words,np.uint16),lut(*v))
            got=np.frombuffer(emulate(path,raw.tobytes(),len(raw)-64),np.uint8)&63
            ref=codes(raw,lut(*v))
            assert np.array_equal(got,ref[:len(got)]),v
    finally:
        if path.exists():path.unlink()
    print('PASS: five CVT64 candidates, legal two-bundle packing and complete bit-exact output',flush=True)

def job(spec):
    dev,cn,seed=spec;standard='PAL' if seed%2 else 'NTSC'
    c=sim.make_case(seed,cn,standard,dev,filter_hz=10e6 if dev<=1.05 else 12e6,fields=1,physical_adc=True)
    rows=[]
    for v in VARIANTS:
        table=lut(*v);m=sim.measure(c,codes(c['raw'],table),codes(c['clean'],table))
        truth=c['truth'][3000:-3000];burst=c['region'][3000:-3000]==3
        error=m['video'][3000:-3000]-truth
        rows.append(dict(variant=v,dev=dev,cnr_db=cn,seed=seed,sinad_db=m['sinad'],line_lock=m['picture'],
            burst_sinad_db=float(10*np.log10(max(np.var(truth[burst]),1e-12)/max(np.mean(error[burst]**2),1e-12)))))
    return rows

def search():
    specs=[(dev,cn,seed) for dev in (.69,1.,1.4) for cn in (1.,4.,30.) for seed in (201,202,203,204)]
    rows=[]
    with mp.Pool(2) as pool:
        for n,part in enumerate(pool.imap(job,specs),1):rows.extend(part);print('CVT64',n,'/',len(specs),flush=True)
    old=json.loads((HERE/'data/rescue_study.json').read_text())['rows'];summary=[]
    for v in VARIANTS:
        delta=[];violations=[]
        for r in (r for r in rows if tuple(r['variant'])==v):
            b=next(b for b in old if b['baseline'] and b['scenario']=='clean' and b['dev']==r['dev'] and b['cnr_db']==r['cnr_db'] and b['seed']==r['seed'])
            delta.append(r['sinad_db']-b['sinad_db'])
            if r['line_lock']<b['line_lock']-.02 or (r['cnr_db']>=20 and (r['sinad_db']<b['sinad_db']-1 or r['burst_sinad_db']<b['burst_sinad_db']-1)):
                violations.append([r['dev'],r['cnr_db'],r['seed']])
        summary.append(dict(variant=v,mean_delta_db=float(np.mean(delta)),worst_delta_db=float(min(delta)),violations=violations))
    (HERE/'data/cvt_precision.json').write_text(json.dumps(dict(evidence='synthetic search; no hardware LP or picture qualification',rows=rows,summary=summary),indent=2)+'\n')
    for s in summary:print(s,flush=True)
if __name__=='__main__':
    os.environ.setdefault('OPENBLAS_NUM_THREADS','1')
    check() if len(sys.argv)>1 and sys.argv[1]=='check' else search()
