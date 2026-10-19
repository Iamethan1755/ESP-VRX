"""C5VRX by Twotoz/contributors: standalone figures for rescue_study.py."""
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
data = json.loads((HERE/'data/rescue_study.json').read_text())
fig, axes = plt.subplots(2,4,figsize=(16,11),layout='constrained')
names = [(p,k) for k in (.85,.80,.75,.70,.65,.60,.55,.50) for p in ('U85Q','LP')]
for col,dev in enumerate((.69,1.,1.4,1.7)):
    matched = .50 if dev<=.8 else .60 if dev<=1.05 else .85
    reports = [next(r for r in data['report'] if r['dev']==dev and r['cnr_db']==cn and r['scenario']=='clean')
               for cn in (1.,4.,30.)]
    for row,key,label,lo,hi in ((0,'worst_sinad_db','Luma SINAD (dB)',-6,16),(1,'worst_lock','Model line lock',0,1)):
        a = axes[row,col]
        values = np.array([[next(c for c in r['candidates'] if c['program']==p and c['kp']==k)[key]
                            for r in reports] for p,k in names])
        im=a.imshow(values,aspect='auto',vmin=lo,vmax=hi,cmap='viridis')
        a.set_xticks(range(3),['1','4','30']);a.set_xlabel('Input C/N model (dB)')
        a.set_yticks(range(len(names)),[f'{p} {k:.2f}'+(' *' if p=='U85Q' and k==matched else '') for p,k in names],fontsize=8)
        a.set_title(f'Deviation {dev:g}: {label}')
        for x,r in enumerate(reports):
            pick=r['selected']
            if pick:
                y=names.index((pick['program'],pick['kp']))
                a.plot(x,y,marker='s',markersize=16,markerfacecolor='none',markeredgecolor='red',markeredgewidth=2)
        fig.colorbar(im,ax=a,shrink=.75)
fig.suptitle('C5VRX Multi-Rescue: worst of four seeds on identical synthetic Q4\n'
             '* existing MATCHED baseline; red box passes offline lock/colour/headroom gates',fontsize=14)
fig.supxlabel('C5VRX by Twotoz and contributors — synthetic evidence, not HDZero acceptance or measured moving-picture/range gain.',fontsize=10)
fig.savefig(HERE/'data/rescue_study.png',dpi=160)
fig.savefig(HERE/'data/rescue_study.svg')
print('wrote rescue_study.png and .svg')
