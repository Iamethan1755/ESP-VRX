"""Open-field link model: antenna polarisation, gain and height (C5VRX by
Twotoz and the C5VRX contributors). Textbook physics, not C5VRX measurements.

Two-ray model over average ground (eps_r 15, sigma 0.05 S/m) with the full
Fresnel reflection coefficients. At FPV grazing angles (0.4-4 degrees) the
ground reflection of a circularly polarised wave comes back almost entirely
in the OPPOSITE hand (-0.2..-2 dB) and only -16..-34 dB in the same hand, so
an RHCP receive antenna rejects the ground bounce (limited by its axial
ratio), while a linear antenna sees it in full and gets two-ray nulls.

Threshold -93 dBm: ESP32-C5 802.11a 6 Mbps sensitivity -92.5 dBm (module
datasheet) implies a ~5 dB noise figure; 3 dB C/N per 20 MHz is the
simulated U85Q usable threshold (README sections 1-2). VTX 25 mW, 2 dBi RHCP.
Range = first 50 m stretch where fewer than 95 % of positions are above it.

    python field_antenna.py
"""
import cmath, math, numpy as np
f=5.8e9; lam=3e8/f
epsc = 15 - 1j*0.05/(2*math.pi*f*8.854e-12)   # average ground
def gammas(th):
    s=math.sin(th); c=math.cos(th); root=cmath.sqrt(epsc-c*c)
    return (s-root)/(s+root), (epsc*s-root)/(epsc*s+root)
def prx(d, ht, hr, ptx_dbm, gt, gr, rx):
    d1=math.hypot(d,ht-hr); d2=math.hypot(d,ht+hr)
    th=math.atan((ht+hr)/d); Gh,Gv=gammas(th)
    ph=cmath.exp(-2j*math.pi*(d2-d1)/lam)*(d1/d2)
    if rx=='linear_v':      # vertical linear RX, RHCP TX: direct -3 dB
        a = (1 + Gv*ph)/math.sqrt(2)
    elif rx.startswith('cp'):  # RHCP RX, cross-pol rejection xr dB
        xr = float(rx.split('_')[1]); leak = 10**(-xr/20)
        same=(Gh-Gv)/2; opp=(Gh+Gv)/2
        a = 1 + same*ph + leak*opp*ph
    fspl = 20*math.log10(4*math.pi*d1/lam)
    return ptx_dbm + gt + gr - fspl + 20*math.log10(abs(a)+1e-9)
THR = -101 + 5 + 3          # -93 dBm: C5 NF ~5 dB (datasheet -92.5 dBm @11a), 3 dB C/N per 20 MHz (U85Q sim)
def rng(ht, hr, ptx, gt, gr, rx, ok=0.95):
    ds=np.arange(50, 6000, 1.0)
    p=np.array([prx(d,ht,hr,ptx,gt,gr,rx) for d in ds])
    good = p >= THR
    # range: last 50 m bin where >= ok of points are good, scanning outward until the first failing bin
    for k in range(0, len(ds)-50, 50):
        if good[k:k+50].mean() < ok: return ds[k]
    return ds[-1]
print(f"threshold {THR} dBm; 25 mW VTX with 2 dBi RHCP; first 50 m stretch with <95% of positions above threshold")
cases=[("stock-style linear, 0 dBi (matched dual-band dipole)", 0,'linear_v'),
       ("linear, -6 dB (2.4 GHz FPC on 5.8, guess)", -6,'linear_v'),
       ("RHCP omni 3 dBi, AR ~3.8 dB (13 dB X-pol)", 3,'cp_13'),
       ("RHCP patch 8 dBi (20 dB X-pol)", 8,'cp_20'),
       ("RHCP patch 13 dBi helical-ish (20 dB X-pol)", 13,'cp_20')]
for ht,hr in ((1,1),(30,1.0),(30,3.0),(60,3.0)):
    print(f"\nVTX height {ht} m, receiver {hr} m")
    for name,g,rx in cases:
        print(f"  {name:50s} {rng(ht,hr,14,2,g,rx):6.0f} m")
