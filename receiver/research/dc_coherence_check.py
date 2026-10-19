#!/usr/bin/env python3
"""C5VRX by Twotoz and the C5VRX contributors.

DC versus coherence on IQSNAP captures (console `I`, 8190 bytes, Ihigh_Qlow).
Prints the window DC, the spread, the coherence as gain.c dg_measure counts
it (power >= 8, phase step <= 45 degrees) and the same after removing the
window DC, each with the 12 MHz cn_estimate.h table.
Usage: python research/dc_coherence_check.py research/data/walk_2026-10-13/off_*.bin
"""
import sys, math, glob
def cn(coh, f=12):
    c20=[572,632,700,772,842,902,986,1000];c12=[705,756,814,868,918,957,997,1000]
    t=c12; noise=577; cnp=[0,20,40,60,80,100,150,200]; c=coh*10
    if c<=noise+20: return -990
    if c<=t[0]: return -1-(t[0]-c)*30//(t[0]-noise)
    for k in range(1,8):
        if c<=t[k]: return cnp[k-1]+(c-t[k-1])*(cnp[k]-cnp[k-1])//(t[k]-t[k-1])
    return 200
def coh(I,Q,di=0.0,dq=0.0):
    n=0; pp=None
    for i,q in zip(I,Q):
        a=i-di;b=q-dq; p=(a*a+b*b)  # in cell^2 (ci/2)
        ph=math.atan2(b,a)
        if pp is not None and p*1>=8:  # firmware: power>=8 on (ci^2+cq^2)/4 scale == a^2+b^2 >= 8
            d=(ph-pp+math.pi)%(2*math.pi)-math.pi
            if abs(d)<=math.pi/4: n+=1
        pp=ph
    return n*100//(len(I)-1)
for fn in sorted(sum([glob.glob(a) for a in sys.argv[1:]], [])):
    raw=open(fn,"rb").read(); meta=open(fn+".txt").read().split()
    gain=[m for m in meta if m.startswith("gain=")][0]
    I=[(((b>>4)^8)-8)+0.5 for b in raw]; Q=[(((b&15)^8)-8)+0.5 for b in raw]
    mi=sum(I)/len(I); mq=sum(Q)/len(Q)
    vi=sum((x-mi)**2 for x in I)/len(I); vq=sum((x-mq)**2 for x in Q)/len(Q)
    ac=sum(abs(x-mi) for x in I)/len(I)
    c0=coh(I,Q); c1=coh(I,Q,mi,mq)
    p=sorted(a*a+b*b for a,b in zip(I,Q))
    print(f"{fn.split('/')[-1]:14s} {gain:8s} dc=({mi:+.2f},{mq:+.2f}) std=({vi**.5:.2f},{vq**.5:.2f}) p50={p[len(p)//2]:.1f} coh_raw={c0:3d} cn={cn(c0)/10:6.1f} | coh_dcfree={c1:3d} cn={cn(c1)/10:6.1f} uniqI={len(set(I))} uniqQ={len(set(Q))}")
