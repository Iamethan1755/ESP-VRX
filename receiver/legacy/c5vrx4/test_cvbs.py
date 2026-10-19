#!/usr/bin/env python3
"""Source/LUT and compiled snapshot-estimator regressions; no RF range claim."""
import ctypes as ct
import json
import math
from pathlib import Path
import random
import subprocess
import tempfile
import generate_phase8 as gen

HERE = Path(__file__).resolve().parent
FIELDS = [('pairs',ct.c_uint),('ambiguous_pm',ct.c_uint),('origin_pm',ct.c_uint),
          ('clip_pm',ct.c_uint),('mean_i_mcell',ct.c_int),('mean_q_mcell',ct.c_int),
          ('pulses',ct.c_uint),('repeated',ct.c_uint),('period_raw',ct.c_uint)]
FIELDS += [(x,ct.c_int) for x in ('sync_bins','blank_bins','span_bins','sync_mad_bins',
             'blank_mad_bins','sync_mv','blank_mv','sync_depth_mv')]
FIELDS += [('suggested_scale_q10',ct.c_uint),('levels_valid',ct.c_bool)]
class Stats(ct.Structure):
    _fields_ = FIELDS


def make_raw(period=2542, radius=4, sigma=0, offset=0, depth=1, dc=0, seed=0, n=4092, shift=45):
    rng=random.Random(seed);angle=0;raw=[]
    for k in range(n):
        in_sync=(k-shift)%period<188
        # Full sync separation 2 MHz = 38.4 Phase8 bins over 75 ns.
        frequency=-2e6*depth if in_sync else 0
        angle+=2*math.pi*frequency/40e6
        angle+=2*math.pi*offset/40e6
        i=max(-8,min(7,math.floor(radius*math.cos(angle)+dc+rng.gauss(0,sigma))))
        q=max(-8,min(7,math.floor(radius*math.sin(angle)+rng.gauss(0,sigma))))
        raw.append(((i&15)<<4)|(q&15))
    return bytes(raw)


def main():
    volts=gen.voltages()
    assert 0.95<max(volts)<1.15
    transfers=(('std150',''),('legacy','_legacy'),('cvbs150','_cvbs150'))
    for history in (False,True):
        fixed=gen.words_for(history)
        mode='history' if history else 'static'
        for transfer,suffix in transfers:
            other=gen.words_for(history,False,transfer)
            # Phase decode, sign winding and all instruction routes must survive.
            for i in range(1024):
                assert (fixed[i]&~63)==(other[i]&~63)
                if (i//256)&1: assert fixed[i]==other[i]
            body=gen.build(history).split('accumulate:',1)[1]
            assert body==gen.build(history,False,transfer).split('accumulate:',1)[1]
            path=HERE/f'c5vrx4_phase8_{mode}{suffix}.bsasm'
            assert path.read_text()==gen.build(history,False,transfer)
    # One Phase8 bin over 75 ns is 52.083 kHz: 1/128 V at 0.150 V/MHz.
    for transfer,blank,per_bin in (('std150',.31,1/128),('cvbs150',.3,1/128)):
        codes=gen.dac_codes(False,transfer)
        for index in range(256):
            delta=gen.transfer_delta(index)
            target=max(min(volts),min(max(volts),blank+delta*per_bin))
            assert abs(volts[codes[index]]-target)==min(abs(v-target) for v in volts)
            if index>>6==3: assert codes[index]==codes[3<<6]
        pairs=sorted((gen.transfer_delta(i),volts[codes[i]]) for i in range(192))
        assert all(a[1]<=b[1] for a,b in zip(pairs,pairs[1:]))
    # Saturation is final voltage clipping, never modulo wrapping.
    hr,c150=gen.dac_codes(),gen.dac_codes(False,'cvbs150')
    assert all(c150[i]==63 for i in range(256) if gen.transfer_delta(i)>150)
    assert all(c150[i]==0 for i in range(256) if gen.transfer_delta(i)<-60)
    # Standard loaded amplitude: target endpoints round within one DAC step.
    def output_at(hz):
        d=hz*256*gen.SPAN_S
        i=min(range(192),key=lambda i:abs(gen.transfer_delta(i)-d))
        return volts[hr[i]]
    sync,blank,white=map(output_at,(-2e6,0,4.6666666667e6))
    assert .27 <= blank-sync <= .33, (sync,blank,white)
    assert .95 <= white-sync <= 1.05, (sync,blank,white)
    assert .65 <= white-blank <= .75, (sync,blank,white)
    # Full amplitude leaves little CFO margin on the ~1.02 V passive DAC.
    assert output_at(-4e6)==0
    # Invalid scope calibration fails instead of producing arbitrary firmware.
    original=gen.CALIBRATION
    # Windows cannot delete the still-loaded monitor DLL at cleanup.
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        temp=Path(tmp)
        gen.CALIBRATION=temp/'bad.json'
        gen.CALIBRATION.write_text('{"load_ohms":150,"volts_by_code":[]}')
        try:
            gen.voltages()
        except ValueError: pass
        else: raise AssertionError('unqualified load accepted')
        # A ladder gap wider than the servo's 32-mV slew would stall it.
        gapped=[min(1.0,c*0.015+(0.05 if c>=32 else 0)) for c in range(64)]
        gen.CALIBRATION.write_text(json.dumps({"load_ohms":75,"volts_by_code":gapped}))
        try:
            gen.voltages()
        except ValueError as e: assert 'slew bound' in str(e)
        else: raise AssertionError('servo-stalling ladder gap accepted')
        assert max(b-a for a,b in zip(sorted(volts),sorted(volts)[1:]))<=gen.LEVEL_SLEW_VOLTS
        assert f"C5V4_LEVEL_STEP_UV {round(gen.LEVEL_SLEW_VOLTS*1e6)}u" in (HERE/'cvbs_level.h').read_text()
        gen.CALIBRATION=original
        lib=temp/'monitor.so'
        subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        str(HERE/'cvbs_monitor.c'),'-o',str(lib)],check=True)
        analyze=ct.CDLL(str(lib)).c5v4_cvbs_analyze
        analyze.argtypes=[ct.c_void_p,ct.c_size_t,ct.c_bool,ct.c_uint,ct.POINTER(Stats)]
        def inspect(raw,history=False,transfer=0):
            out=Stats();buf=ct.create_string_buffer(raw)
            analyze(buf,len(raw),history,transfer,ct.byref(out));return out
        # Longer observer snapshots contain at least two complete H-syncs at
        # every ring/line alignment, including a reduced phase separation.
        for period in (2542,2560):
            for shift in range(0,period,97):
                for depth in (1,.6):
                    s=inspect(make_raw(period=period,n=8190,shift=shift,depth=depth))
                    assert s.levels_valid and s.pulses>=2 and s.repeated, (period,shift,depth)
                    assert abs(s.period_raw-period)<=6
        for period in (2542,2560):
            for history in (False,True):
                for phase_offset in (0,100e3,-100e3):
                    raw=make_raw(period=period,offset=phase_offset)
                    before=bytes(raw)
                    new=inspect(raw,history);old=inspect(raw,history,1)
                    c150=inspect(raw,history,2)
                    assert raw==before
                    assert new.levels_valid, (period,history,phase_offset,new.pulses,new.repeated)
                    assert abs(new.period_raw-period)<=6
                    assert 240<=new.sync_depth_mv<=400, new.sync_depth_mv
                    assert 240<=c150.sync_depth_mv<=400, c150.sync_depth_mv
                    assert old.sync_depth_mv<new.sync_depth_mv*.7
                    assert 768<=new.suggested_scale_q10<=1536
        # CFO remains a separate problem: do not claim broad headroom at 1 V.
        for history in (False,True):
            shifted=make_raw(offset=-1.5e6)
            std_s=inspect(shifted,history)
            assert std_s.levels_valid and std_s.sync_mv==0
            assert std_s.sync_depth_mv<150, std_s.sync_depth_mv
        # Smaller phase separation must be visible as smaller sync span,
        # rather than silently applied as an unbounded output gain change.
        # Native AGC with the acquisition mask: Q LSB carries the AGC flag.
        # With mask decode the observer sees the same Q3 as the program, and
        # acquisition bursts (~2.5 us every ~37 us, garbage IQ) are outliers
        # the level evidence must survive (servo now also runs under native).
        setmask=ct.CDLL(str(lib)).c5v4_cvbs_set_mask_decode
        setmask.argtypes=[ct.c_bool]
        def masked(raw,seed,burst=100,every=1480):
            rng=random.Random(seed);out=bytearray()
            start=rng.randrange(every)
            for k,b in enumerate(raw):
                acq=(k-start)%every<burst
                if acq: b=rng.randrange(256)
                out.append((b&0xFE)|(1 if acq else 0))
            return bytes(out)
        valid=total=0
        for period in (2542,2560):
            for shift in range(0,period,211):
                raw=masked(make_raw(period=period,n=8190,shift=shift,radius=5),shift)
                setmask(True); m=inspect(raw); setmask(False); u=inspect(raw)
                total+=1
                if m.levels_valid and m.repeated and m.sync_mad_bins<=4 and m.blank_mad_bins<=4:
                    valid+=1
                    assert abs(m.period_raw-period)<=6
                    assert 240<=m.sync_depth_mv<=400, m.sync_depth_mv
                    assert m.sync_mad_bins<=u.sync_mad_bins+1
        print(f"masked native snapshots: {valid}/{total} valid with acquisition bursts")
        assert valid*10>=total*8, (valid,total)
        setmask(False)
        weak=inspect(make_raw(depth=.5));strong=inspect(make_raw())
        assert weak.levels_valid and weak.span_bins<strong.span_bins
        assert weak.sync_depth_mv<strong.sync_depth_mv
        assert weak.suggested_scale_q10==1536
        assert not inspect(bytes([0x22])*4092).levels_valid
        assert not inspect(bytes([0])*4092).levels_valid
        assert not inspect(bytes([0x22])*100).levels_valid
        rng=random.Random(157)
        for _ in range(100):
            assert not inspect(bytes(rng.randrange(256) for _ in range(4092))).levels_valid
        # A periodic video-frequency block with no correct H-sync widths
        # cannot be accepted just from its low/high percentiles.
        chopped=bytes(0x22 if (k//20)%2 else 0xDD for k in range(4092))
        assert not inspect(chopped).levels_valid
        # Check diagnostics expose offset dominance; it is not auto-subtracted.
        shifted=inspect(make_raw(radius=1,dc=3))
        assert shifted.mean_i_mcell>2000 and not shifted.levels_valid
    print('PASS: STD150/CVBS150/legacy transfers, standard amplitude and explicit CFO limits, unchanged '
          'Phase8/winding/routes, PAL/NTSC/CFO snapshots, shrink telemetry and 100 noise refusals')

if __name__=='__main__': main()
