"""C5VRX by Twotoz and contributors: non-oracle passive Q4 level probe.
Requires Docker gcc:14. No live LUT writes. Optional --capture path replays
a real IQ snapshot, without inventing a ground truth for its picture/levels.
"""
import argparse
import json
import subprocess
import tempfile
from collections import Counter
from pathlib import Path
import numpy as np
import sim

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture', type=Path)
    args = parser.parse_args()
    # One mounted temporary directory, no path interpolation into shell code.
    with tempfile.TemporaryDirectory(prefix='c5vrx-output-probe-') as folder:
        work = Path(folder)
        specs = []
        for dev in (.69, 1.0, 1.7):
            for cnr in (-10., 1., 6., 30.):
                for standard in ('PAL', 'NTSC'):
                    c = sim.make_case(200, cnr, standard, dev, filter_hz=12e6, fields=1, physical_adc=True)
                    name = f'case{len(specs)}'
                    (work / (name + '.bin')).write_bytes(c['raw'].tobytes())
                    specs.append(dict(name=name, deviation=dev, cnr_db=cnr, standard=standard, synthetic=True))
        if args.capture:
            (work / 'capture.bin').write_bytes(args.capture.read_bytes())
            specs.append(dict(name='capture', synthetic=False, source='local board VTX-off snapshot'))
        (work / 'names.txt').write_bytes(('\n'.join(s['name'] for s in specs) + '\n').encode('ascii'))
        command = ('set -e; gcc -O2 -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I/project/main '
                   '/project/research/output_probe.c /project/main/gain.c '
                   '-Wl,--gc-sections -lm -o /tmp/probe; while IFS= read -r name; do '
                   '/tmp/probe /data/"$name".bin 12 > /data/"$name".jsonl; done < /data/names.txt')
        subprocess.run(['docker','run','--rm','-v',str(ROOT.as_posix())+':/project',
                        '-v',str(work.as_posix())+':/data','gcc:14','bash','-c',command],check=True)
        result = []
        for spec in specs:
            rows = [json.loads(s) for s in (work / (spec['name']+'.jsonl')).read_text().splitlines()]
            usable = [r for r in rows if r['usable']]
            summary = dict(**spec, windows=len(rows), accepted=len(usable), rejected=len(rows)-len(usable))
            if usable:
                sync = np.array([r['sync_khz'] for r in usable])
                gain = np.array([r['candidate_gain'] for r in usable])
                summary.update(sync_median_khz=float(np.median(sync)), sync_spread_p95_p5_khz=float(np.percentile(sync,95)-np.percentile(sync,5)),
                               gain_median=float(np.median(gain)), gain_spread_p95_p5=float(np.percentile(gain,95)-np.percentile(gain,5)))
                if spec.get('synthetic'):
                    true_sync = sim.PORCH_HZ - spec['deviation']*sim.DEV_PP*40/140
                    summary['sync_bias_khz'] = float(np.median(sync) - true_sync/1000)
            summary['rejection_reasons'] = dict(Counter(r['reason'] for r in rows if not r['usable']))
            summary['example_windows'] = rows[:4]
            result.append(summary)
            print({k:v for k,v in summary.items() if k!='example_windows'},flush=True)
        # Very weak synthetic carrier and real board noise must not be used
        # to pick a new output setting. This is estimator feasibility only.
        assert all(s['accepted']==0 for s in result if not s['synthetic'] or s.get('cnr_db')==-10)
        target = HERE / 'data/output_probe.json'
        target.write_text(json.dumps({'evidence':'passive research; white reserve is unqualified, no actuator',
                                     'cases':result},indent=2)+'\n')
        print('wrote',target,flush=True)


if __name__ == '__main__': main()
