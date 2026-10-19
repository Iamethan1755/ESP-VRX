"""C5VRX by Twotoz/contributors: held-out CVT report and exportable plots."""
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
HERE=Path(__file__).resolve().parent
d=json.loads((HERE/'data/cvt_validation.json').read_text())
rows=d['rows']; names=sorted({r['program'] for r in rows if r['program'].startswith('CVT')})
groups={}
for r in rows:groups.setdefault((r['dev'],r['cnr_db'],r['seed'],r['scenario']),[]).append(r)
summary=[]
for name in names:
    delta=[];weak=[];strong=[];violations=[];oracle_delta=[]
    for key,group in groups.items():
        b=next(r for r in group if r['baseline']);c=next(r for r in group if r['program']==name)
        diff=c['sinad_db']-b['sinad_db'];delta.append(diff)
        (strong if key[1]>=20 else weak).append(diff)
        eligible=lambda r: r['line_lock']>=.9 and r['line_lock']>=b['line_lock']-.02 and r['burst_sinad_db']>=b['burst_sinad_db']-1 and r['clean_rail_fraction']<=b['clean_rail_fraction']+.005 and (key[1]<20 or r['sinad_db']>=b['sinad_db']-1)
        fixed=[r for r in group if r['program'] in ('U85Q','LP') and eligible(r)]
        if fixed:oracle_delta.append(c['sinad_db']-max(r['sinad_db'] for r in fixed))
        if not eligible(c):violations.append(list(key))
    summary.append(dict(program=name,cases=len(delta),gate_failures=len(violations),
        mean_delta_db=float(np.mean(delta)),worst_delta_db=float(min(delta)),
        weak_mean_delta_db=float(np.mean(weak)),strong_mean_delta_db=float(np.mean(strong)),
        mean_delta_vs_eligible_fixed_oracle_db=float(np.mean(oracle_delta)),violations=violations))
(HERE/'data/cvt_validation_summary.json').write_text(json.dumps(dict(
    evidence='synthetic held-out model; no recognisable moving-video/colour/range qualification',summary=summary),indent=2)+'\n')
fig,axes=plt.subplots(4,2,figsize=(12,13),sharex=True)
for j,dev in enumerate((.69,1.,1.4,1.7)):
    clean=[r for r in rows if r['dev']==dev and r['scenario']=='clean']
    cnrs=sorted({r['cnr_db'] for r in clean})
    series={'MATCHED':[r for r in clean if r['baseline']],**{name:[r for r in clean if r['program']==name] for name in names}}
    for name,values in series.items():
        for ax,metric in zip(axes[j],('sinad_db','line_lock')):
            y=[min(r[metric] for r in values if r['cnr_db']==cn) for cn in cnrs]
            ax.plot(cnrs,y,label=name,linewidth=1.5)
    axes[j,0].set_ylabel(f'deviation {dev}\nworst-seed SINAD (dB)')
    axes[j,1].set_ylabel('worst-seed line lock')
    axes[j,1].axhline(.9,color='grey',linestyle=':',linewidth=1)
    for ax in axes[j]:ax.grid(alpha=.25)
axes[0,0].legend(fontsize=7)
for ax in axes[-1]:ax.set_xlabel('model input C/N (dB)')
fig.suptitle('C5VRX by Twotoz/contributors — CVT held-out same-Q4\nTwo synthetic fields, PAL/NTSC; waveform metrics, not moving-video/range proof',fontsize=11)
fig.tight_layout(rect=(0,0,1,.96))
for ext in ('png','svg'):fig.savefig(HERE/f'data/cvt_validation.{ext}',dpi=160)
for s in summary:print({k:v for k,v in s.items() if k!='violations'})
