"""C5VRX-5 range simulator (C5VRX by Twotoz and the C5VRX contributors).

Self-contained model of the C5VRX-5 receive chain, for range research:

  CVBS raster (PAL/NTSC, burst, textured picture)
  -> FM at 80 MS/s (blanking at the AFC porch target, VTX deviation scale)
  -> C5 analog channel (5th-order Butterworth, 10 MHz, ~20 MHz complex)
  -> thermal noise at a C/N measured in that 20 MHz channel
  -> optional narrower analog filter (C5VRX-5 narrow-filter calibration)
  -> PARLIO keeps every second sample (40 MS/s, unfiltered decimation)
  -> gain to the Direct Gain V5 radius, 4-bit ultrafine quantization
  -> U85Q, bit-exact: the two LUT16 lookups per 50 ns of main/u85q.bsasm
  -> 6-bit resistor DAC -> 5 MHz goggle input filter
  -> goggle sync/H-PLL model (ideal and peak-clamp AGC) and luma SINAD.

Adapted from the C5VRX-4 study tools of PR #190 (waveforms.py,
goggle_lock.py, detector_study/designs.py). Synthetic engineering numbers,
not goggle acceptance or measured range. C/N is per 20 MHz channel, so a
narrower filter shows its noise reduction as better picture at equal C/N.
"""
from pathlib import Path
import re
import numpy as np
from scipy import signal as sg
from numba import njit

FS, F80 = 40e6, 80e6
HERE = Path(__file__).resolve().parent
DEV_PP = 6.7e6                        # sync tip to peak white at deviation 1.0
PORCH_HZ = -1.0e6                     # C5VRX-5 AFC porch target
DAC_VOLTS = np.array([sum(1 / r for bit, r in enumerate((8200, 3900, 2000, 1000, 470, 240)) if code & (1 << bit))
                      for code in range(64)])
CHAN = sg.butter(5, 10e6, fs=F80, output='sos')
GOGGLE = sg.butter(4, 5e6, fs=FS, output='sos')
SYNC_LP = sg.butter(2, 1e6, fs=FS, output='sos')


def u85q_lut(path=HERE.parent / 'main' / 'u85q.bsasm'):
    text = Path(path).read_text()
    words = re.search(r'^lut ([0-9 ]+)$', text, re.M).group(1).split()
    return np.array([int(w) for w in words], np.uint16)


# PAIR4411 shared word: kept bits (I_A, Q_A, I_B sign, Q_B sign) of the two
# raw bytes in a 50 ns span (compile_overlay.pair_bits('4411')).
PAIR_BITS = np.array([4, 5, 6, 7, 0, 1, 2, 3, 15, 11], np.int64)


@njit(cache=True)
def _pair_codes(raw, lut, bits, b):
    out = np.zeros(len(raw), np.uint8)
    state = 0
    mask = (1 << (10 - b)) - 1
    for k in range(len(raw) // 2 - 1):
        word = np.int64(raw[2 * k]) | (np.int64(raw[2 * k + 1]) << 8)
        address = 0
        for j in range(10):
            address |= ((word >> bits[j]) & 1) << j
        token = np.int64(lut[address]) >> (16 - b)
        value = np.int64(lut[(state << b) + token])
        state = (value >> 6) & mask
        out[2 * k + 2] = value & 63
        out[2 * k + 3] = value & 63
    return out


def u85q_codes(raw, lut=None):
    return _pair_codes(raw, u85q_lut() if lut is None else lut, PAIR_BITS, 5)


def half_sample(standard, h):
    return h * 1280 if standard == 'PAL' else ((h * 2860 + 4) // 9) * 4


def raster(standard, seed, fields=2):
    """BT.470-timed raster: syncs, equalizing/broad pulses, burst, and a
    textured picture (random luma knots every 0.2 us plus chroma)."""
    pal = standard == 'PAL'
    fh, eq = (625, 5) if pal else (525, 6)
    total = fh * fields
    ire = np.zeros(half_sample(standard, total))
    region = np.zeros(len(ire), np.uint8)   # 1 picture, 2 sync, 3 burst
    fsc = 4433618.75 if pal else 3579545.454545
    rng = np.random.default_rng(seed)
    h = 0
    while h < total:
        pos = (h + (eq if pal else 0)) % fh
        start, end = half_sample(standard, h), half_sample(standard, h + 1)
        if pos < 3 * eq:
            width = (end - start) - 188 if eq <= pos < 2 * eq else (94 if pal else 92)
            ire[start:start + width] = -40
            region[start:start + width] = 2
            h += 1
            continue
        if h & 1:
            h += 1
            continue
        end = half_sample(standard, min(h + 2, total))
        ire[start:start + 188] = -40
        region[start:start + 188] = 2
        bs = start + (224 if pal else 212)
        be = bs + (90 if pal else 100)
        t = np.arange(bs, be) / FS
        ire[bs:be] = 20 * np.sin(2 * np.pi * fsc * t + ((.75 if (h // 2) & 1 else -.75) * np.pi if pal else np.pi))
        region[bs:be] = 3
        if pos > 40:
            a, b = start + 420, end - 80
            x = np.arange(b - a)
            knots = np.arange(0, len(x) + 8, 8)
            image = np.interp(x, knots, rng.uniform(5, 95, len(knots)))
            image += 8 * np.sin(2 * np.pi * fsc * (x + a) / FS + ((-1) ** (h // 2) if pal else 1) * .6)
            ire[a:b] = image
            region[a:b] = 1
        h += 2
    return ire, region


def make_case(seed, cnr_db, standard='PAL', deviation=1.0, rms=4.2, filter_hz=None,
              porch_hz=PORCH_HZ, offset_hz=0.0, fields=2, physical_adc=False, adc_gain=None, white_scale=1.0):
    """Raw IQ bytes (noisy and noise-free) plus the goggle-filtered truth.
    filter_hz: -3 dB full width of an extra 4th-order analog filter (None =
    the C5's normal ~20 MHz channel only). offset_hz: residual AFC error."""
    rng = np.random.default_rng(seed)
    ire, region = raster(standard, seed + 7, fields)
    # Optional research stress: unequal sync/picture deviation (issue #200
    # reported levels). Default 1.0 preserves all existing studies exactly.
    if white_scale != 1.0:
        ire = np.where(ire > 0, ire * white_scale, ire)
    source = sg.lfilter(sg.firwin(81, 6e6, fs=F80), 1, np.repeat(ire, 2))
    # Blanking (0 IRE) sits at the porch target; sync -40, white +100 IRE.
    freq = porch_hz + offset_hz + deviation * source / 140 * DEV_PP
    z = sg.sosfilt(CHAN, np.exp(2j * np.pi * np.cumsum(freq) / F80))[::2]
    z /= np.sqrt(np.mean(abs(z) ** 2))
    n = sg.sosfilt(CHAN, (rng.normal(size=2 * len(z)) + 1j * rng.normal(size=2 * len(z))) / np.sqrt(2))[::2]
    n /= np.sqrt(np.mean(abs(n) ** 2))
    pre = sg.butter(4, filter_hz / 2, fs=FS, output='sos') if filter_hz else None
    ratio = 10 ** (cnr_db / 10)

    analog = []
    # Physical study uses ONE gain for noisy and clean inputs. Default legacy
    # studies retain their old clamp/instantaneous-normalization behavior;
    # those results are not folding or gain-transient proofs.
    reference = z * np.sqrt(ratio / (1 + ratio))
    if pre is not None:
        reference = sg.sosfilt(pre, reference)
    fixed_scale = rms / np.sqrt(np.mean(abs(reference) ** 2)) if adc_gain is None else adc_gain

    def quantize(noise):
        y = (z * np.sqrt(ratio) + noise) / np.sqrt(1 + ratio)
        if pre is not None:
            # The narrower filter sits before the ADC on the chip; modelled at
            # 40 MS/s after the channel filter that already limits aliasing.
            y = sg.sosfilt(pre, y)
        if physical_adc:
            import iq_lane_fusion as F
            y *= fixed_scale
            analog.append(y * 16.)
            return F.capture(y * 16.)
        y *= rms / np.sqrt(np.mean(abs(y) ** 2))   # Legacy ideal gain normalization
        i = np.clip(np.floor(y.real), -8, 7).astype(int)
        q = np.clip(np.floor(y.imag), -8, 7).astype(int)
        return (((i & 15) << 4) | (q & 15)).astype(np.uint8)

    truth = sg.sosfilt(GOGGLE, ire)
    result = dict(raw=quantize(n), clean=quantize(np.zeros_like(n)), truth=truth, region=region,
                  standard=standard, cnr=cnr_db, deviation=deviation, filter_hz=filter_hz,
                  physical_adc=physical_adc)
    if physical_adc:
        result.update(adc=analog[0], clean_adc=analog[1], adc_gain=fixed_scale)
    return result


def video_out(codes):
    return sg.sosfilt(GOGGLE, DAC_VOLTS[codes])


# ---- Goggle model (C5VRX-4 goggle_lock.py) ---------------------------------

def calibrate(y, truth, region, burn=3000):
    def fit(lag, step):
        a = y[burn + lag:len(y) - burn + lag:step]
        t = truth[burn:len(truth) - burn:step]
        m = region[burn:len(region) - burn:step] != 3
        A = np.c_[a[m], np.ones(m.sum())]
        coef = np.linalg.lstsq(A, t[m], rcond=None)[0]
        return float(np.mean((A @ coef - t[m]) ** 2)), float(coef[0]), float(coef[1])
    lag = min(range(-40, 41), key=lambda l: fit(l, 16)[0])
    _, g, off = fit(lag, 1)
    return lag, g, off


def apply_cal(y, cal):
    lag, g, off = cal
    out = np.empty_like(y)
    if lag >= 0:
        out[:len(y) - lag] = y[lag:]
        out[len(y) - lag:] = y[-1]
    else:
        out[-lag:] = y[:lag]
        out[:-lag] = y[0]
    return g * out + off


def goggle(y_ire, truth, standard, agc='ideal', window_us=1.5, gain=.3, min_width_us=1.5):
    """Sync low-pass, slicer, H-PLL flywheel; returns the share of lines the
    goggle shows correctly (locked and within 0.5 us)."""
    period = 2560. if standard == 'PAL' else 40e6 / 15734.264
    ys = sg.sosfilt(SYNC_LP, y_ire)
    below_t = truth < -20
    d = np.diff(np.r_[False, below_t, False].astype(np.int8))
    s, e = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    hs = s[((e - s) > 140) & ((e - s) < 320)]
    W, n = int(min_width_us * 40), len(ys)
    if agc == 'ideal':
        thr = np.full(n, -20.)
    else:
        thr = np.empty(n)
        tipv, blk, rel = -40., 0., 1 / (2e-3 * FS)
        lp = sg.sosfilt(sg.butter(1, 2e5, fs=FS, output='sos'), ys)
        for k0 in range(0, n, 40):
            m = float(lp[k0:k0 + 40].min())
            tipv = m if m < tipv else tipv + (blk - 40. - tipv) * 40 * rel
            thr[k0:k0 + 40] = (tipv + blk) / 2
    below = ys < thr
    cs = np.r_[0, np.cumsum(below)]
    fall = np.flatnonzero(below[1:] & ~below[:-1]) + 1
    cands, last = [], -10 ** 9
    for f in fall:
        if f + W >= n or f - last < W:
            continue
        if cs[f + W] - cs[f] >= .8 * W:
            cands.append(f)
            last = f
    cands = np.array(cands, float)
    win = window_us * 40
    locked, hits, history, starts = False, 0, [], []
    tp = cands[0] if len(cands) else 0.
    while tp < n - period:
        lo, hi = np.searchsorted(cands, tp - win), np.searchsorted(cands, tp + win, 'right')
        if hi > lo:
            c = cands[lo + int(np.argmin(abs(cands[lo:hi] - tp)))]
            tp += gain * (c - tp) if locked else (c - tp)
            hits += 1
            history.append(1)
        else:
            history.append(0)
            hits = 0
            if not locked:
                nxt = np.searchsorted(cands, tp + win, 'right')
                if nxt < len(cands) and cands[nxt] < tp + period - win:
                    tp = cands[nxt] - period
        history = history[-32:]
        if not locked and hits >= 8:
            locked = True
        elif locked and len(history) == 32 and sum(history) < 16:
            locked = False
        starts.append((tp, locked))
        tp += period
    if not len(hs) or not starts:
        return 0.
    ls = np.array([p for p, _ in starts])
    lk = np.array([l for _, l in starts], bool)
    dd = (ls - hs[0] % period + period / 2) % period - period / 2
    if lk.any():
        ang = np.angle(np.mean(np.exp(2j * np.pi * dd[lk] / period))) * period / (2 * np.pi)
        dd = (dd - ang + period / 2) % period - period / 2
    ok = lk & (np.abs(dd) < 20)
    return float(np.mean(ok[40:])) if len(ok) > 40 else 0.


def measure(c, codes=None, clean_codes=None):
    """Picture (worse of ideal and peak-clamp goggle) and luma SINAD."""
    codes = u85q_codes(c['raw']) if codes is None else codes
    clean_codes = u85q_codes(c['clean']) if clean_codes is None else clean_codes
    y, yc = video_out(codes), video_out(clean_codes)
    yi = apply_cal(y, calibrate(yc, c['truth'], c['region']))
    t, r = c['truth'][3000:-3000], c['region'][3000:-3000]
    m = r == 1
    e = yi[3000:-3000][m] - t[m]
    sinad = float(10 * np.log10(np.var(t[m]) / max(np.mean(e ** 2), 1e-12)))
    pic = min(goggle(yi, c['truth'], c['standard'], 'ideal'), goggle(yi, c['truth'], c['standard'], 'peak'))
    return dict(picture=pic, sinad=sinad, video=yi)


def regenerate_syncs(yi, standard, gain=.05, window_us=1.0, hold_lines=600):
    """Receiver-side sync regeneration, as firmware could do it: a slow line
    PLL on the demodulated video (slicer at -20 IRE, >= 1.5 us wide) coasts
    through missing syncs on the VTX's own line rate and writes a clean
    4.7 us sync tip at its predicted positions while it is locked. No
    knowledge of the true timing. Picture content is untouched."""
    period = 2560. if standard == 'PAL' else 40e6 / 15734.264
    ys = sg.sosfilt(SYNC_LP, yi)
    below = ys < -20
    cs = np.r_[0, np.cumsum(below)]
    fall = np.flatnonzero(below[1:] & ~below[:-1]) + 1
    W = 60
    cands = np.array([f for f in fall if f + W < len(ys) and cs[f + W] - cs[f] >= .8 * W], float)
    out = yi.copy()
    if not len(cands):
        return out
    win = window_us * 40
    tp, locked, hits, misses = cands[0], False, 0, 0
    delay = 60    # sync-LP and slicer delay to the sync leading edge (~1.5 us)
    while tp < len(ys) - period:
        lo, hi = np.searchsorted(cands, tp - win), np.searchsorted(cands, tp + win, 'right')
        if hi > lo:
            c = cands[lo + int(np.argmin(abs(cands[lo:hi] - tp)))]
            tp += gain * (c - tp) if locked else (c - tp)
            hits += 1
            misses = 0
        else:
            hits = 0
            misses += 1
            if not locked:
                nxt = np.searchsorted(cands, tp + win, 'right')
                if nxt < len(cands) and cands[nxt] < tp + period - win:
                    tp = cands[nxt] - period
        if not locked and hits >= 16:
            locked = True
        elif locked and misses > hold_lines:
            locked = False
        if locked:
            s = int(round(tp)) - delay
            if 0 <= s and s + 188 < len(out):
                out[s:s + 188] = -40.
        tp += period
    return out


def regenerate_codes(codes, clean, c, gain=.05, window_us=1.0, hold_lines=600, detect_delay=12):
    """Firmware-faithful variant: the PLL runs on the demodulated video and
    the clean sync is written into the DAC code stream (the firmware would
    write the ring ahead of the TX read). Sync code and decode latency come
    from the noise-free calibration, never from the noisy truth timing."""
    yc = video_out(clean)
    cal = calibrate(yc, c['truth'], c['region'])
    yi = apply_cal(video_out(codes), cal)
    lag = cal[0]
    standard = c['standard']
    period = 2560. if standard == 'PAL' else 40e6 / 15734.264
    sync_code = int(np.median(clean[np.clip(np.flatnonzero(c['region'] == 2) + lag, 0, len(clean) - 1)]))
    ys = sg.sosfilt(SYNC_LP, yi)
    below = ys < -20
    cs = np.r_[0, np.cumsum(below)]
    fall = np.flatnonzero(below[1:] & ~below[:-1]) + 1
    W = 60
    cands = np.array([f for f in fall if f + W < len(ys) and cs[f + W] - cs[f] >= .8 * W], float)
    out = codes.copy()
    if not len(cands):
        return out
    win = window_us * 40
    tp, locked, hits, misses = cands[0], False, 0, 0
    while tp < len(ys) - period:
        lo, hi = np.searchsorted(cands, tp - win), np.searchsorted(cands, tp + win, 'right')
        if hi > lo:
            cnd = cands[lo + int(np.argmin(abs(cands[lo:hi] - tp)))]
            tp += gain * (cnd - tp) if locked else (cnd - tp)
            hits += 1
            misses = 0
        else:
            hits = 0
            misses += 1
            if not locked:
                nxt = np.searchsorted(cands, tp + win, 'right')
                if nxt < len(cands) and cands[nxt] < tp + period - win:
                    tp = cands[nxt] - period
        if not locked and hits >= 16:
            locked = True
        elif locked and misses > hold_lines:
            locked = False
        if locked:
            s = int(round(tp)) - detect_delay + lag
            if 0 <= s and s + 188 < len(out):
                out[s:s + 188] = sync_code
        tp += period
    return out
