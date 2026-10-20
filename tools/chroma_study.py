# SPDX-License-Identifier: GPL-3.0-only
"""Colour study for VRX Bridge: can the S3 decode NTSC colour from the C5VRX
receiver's video bus? A PC prototype on raw bus recordings
(bridge_tool.py rawlong: 20 MS/s, one uint8 per sample, 6-bit codes).

  python tools/chroma_study.py captures/rawlong_20261018_154739_20MHz.bin
  python tools/chroma_study.py captures/rawlong_5bit_20MHz.bin          (5 bits: found by itself)
  python tools/chroma_study.py REC.bin --bits 5 --raw5                  (bit 0 dropped, codes not restored)

Per line: the sync edge (line PLL as in bridge/main/video.c), burst phase and
amplitude, luma/chroma separation and U/V demodulation against the burst
with a ~1 MHz low-pass. Writes next to the recording (suffix _5bit/_raw5
for the 5-bit runs):
  <name>_colour.png      one field, 320x240, PC reference decoder
  <name>_colour_fw.png   the same field, firmware-style decoder (8-sample chroma blocks)
  <name>_gray.png        the same field, luma only
  <name>_compare.png     the three side by side
  <name>_slips.png       burst phase and bit-mixed samples over the recording
and prints the measured numbers. docs/colour-study.md has the findings.
Needs numpy and matplotlib (and Pillow, which matplotlib installs, for the JPEG sizes).
"""
import argparse
import io
import pathlib

import numpy as np

FS = 20e6
FSC = 315e6 / 88                 # NTSC colour subcarrier, 3.579545 MHz
W0 = 2 * np.pi * FSC / FS        # subcarrier phase per sample
LINE = 63.5556e-6 * FS           # nominal NTSC line, 1271.1 samples
SLIP_DEG = 360 * FSC / FS        # one duplicated or dropped sample: 64.4 degrees of subcarrier
W, ROWS = 320, 240
FIRST_ROW = 14                   # picture row 0 is the 14th line after the broad pulses (video.c)
H_START, H_LEN = 9.6e-6, 52.2e-6
BURST_AT, BURST_N = 5.5e-6, 45   # 45 samples = 8.05 subcarrier cycles: no DC leak
FW_BLOCK = 8                     # firmware-style chroma: 8-sample blocks (0.4 us, 130 per line)
CPU_HZ = 240e6

# Output codes of the C5's FM tracker at OUTPUT 1.0x: every LUT in
# receiver/main/kp_tables.h maps onto this ladder (~2.67 codes a step).
# Any other code is a sample the S3 took while the C5's bits were changing.
LADDER = (0, 2, 4, 7, 9, 12, 15, 17, 20, 23, 25, 28, 30, 33, 36, 38, 41, 44, 46, 49, 51, 54, 57, 59, 62)


def n_us(us: float) -> int:
    return int(round(us * 1e-6 * FS))


def load(path: pathlib.Path, bits: int, raw5: bool):
    """Codes 0..63, 5-bit?, recorded as 5-bit?, and a mask of the codes the
    C5 can send. 5-bit data (bit 0 always 0, or --bits 5) is mapped back onto
    the ladder with a 32-entry table: no two ladder codes differ only in
    bit 0, so nothing is lost except on bit-mixed samples."""
    codes = np.fromfile(path, np.uint8) & 63
    native5 = not (codes & 1).any()
    five = bits == 5 or native5
    valid = np.zeros(64, bool)
    if five:
        codes = codes & 62
        if not raw5:
            lut = np.arange(64, dtype=np.uint8)
            for c in LADDER:
                lut[c & 62] = c
            codes = lut[codes]
    valid[[c & 62 if five and raw5 else c for c in LADDER]] = True
    return codes, five, native5, valid


def lowpass(fc: float, taps: int) -> np.ndarray:
    m = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * fc / FS * m) * np.blackman(taps)
    return h / h.sum()


def median_filter(v: np.ndarray, k: int) -> np.ndarray:
    p = np.pad(v, k // 2, mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(p, k), axis=1)


# ---- Line sync -----------------------------------------------------------

def _track(box: np.ndarray, thr: float):
    low = box < thr
    cs = np.concatenate(([0], np.cumsum(low)))

    def low_frac(i, n):
        return (cs[i + n] - cs[i]) / n

    def cross(i):
        return i - 1 + (box[i - 1] - thr) / (box[i - 1] - box[i])

    def edge(a, b, run):
        # first falling slicer crossing whose low run lasts (80 % low over) `run` samples
        for i in np.flatnonzero(~low[a - 1:b - 1] & low[a:b]) + a:
            if low_frac(i, run) >= 0.8:
                return cross(i)
        return None

    # Lock on the first sync (>= 3.5 us low) followed by five line-spaced ones.
    starts = np.flatnonzero(~low[:-1] & low[1:]) + 1
    starts = starts[(starts > 8) & (starts < len(box) - 2 * LINE)]
    cand = np.array([s for s in starts if low_frac(s, n_us(3.5)) > 0.9])
    d = np.diff(cand)
    ok = (d > n_us(62)) & (d < n_us(65.5))
    first = next(i for i in range(len(ok) - 5) if ok[i:i + 5].all())
    period = float(np.median(d[first:first + 5]))
    t = cross(cand[first])
    # Line PLL with flywheel, as video.c: +-2 us search, edge = pred + err/2, period += err/32.
    T, R = [t], [t]
    while t + 2.5 * period < len(box):
        pred = t + period
        e = edge(int(pred) - n_us(2), int(pred) + n_us(2), n_us(1.5))
        if e is None:
            t = pred
        else:
            t = pred + (e - pred) / 2
            period = min(max(period + (e - pred) / 32, n_us(62)), n_us(65.5))
        T.append(t)
        R.append(np.nan if e is None else e)
    T, R = np.array(T), np.array(R)
    probes = np.array([n_us(6 + 3 * k) for k in range(7)])
    broad = low[T.astype(int)[:, None] + probes].sum(axis=1) >= 5
    return T, R, broad


def track_lines(x: np.ndarray):
    """Sync edge (PLL), raw edge (nan: flywheel) and broad-pulse flag per line,
    on the 0.4 us box average like video.c. The slicer starts 20 % up from the
    sync tip and is then set midway between the measured sync and blanking."""
    box = np.convolve(x, np.ones(8) / 8, mode="same")
    lo, hi = np.percentile(box, [2, 60])
    thr = lo + (hi - lo) / 5
    for _ in range(2):
        T, R, broad = _track(box, thr)
        e = np.round(T[~broad & ~np.isnan(R)]).astype(int)
        sync = x[e[:, None] + np.arange(n_us(1.0), n_us(3.5))].mean()
        blank = x[e[:, None] + np.arange(n_us(8.0), n_us(9.2))].mean()
        thr = (sync + blank) / 2
    return T, R, broad, sync, blank


def fields(broad: np.ndarray) -> list[dict]:
    """Picture rows {row: line} of each complete field, numbered as video.c:
    the first regular line after >= 2 broad ones starts a field."""
    out, cur, run, lif = [], None, 0, 0
    for i, b in enumerate(broad):
        if b:
            run += 1
        else:
            if run >= 2:
                cur, lif = {}, 0
                out.append(cur)
            run = 0
            if cur is not None and 0 <= lif - FIRST_ROW < ROWS:
                cur[lif - FIRST_ROW] = i
        lif += 1
    return [f for f in out if len(f) == ROWS]


# ---- Burst and phase reference -------------------------------------------

def bursts(x: np.ndarray, T: np.ndarray):
    """Per line: the burst as a complex amplitude against the subcarrier at
    the absolute sample index (burst = Re(B e^{j w0 k}), codes peak) and the
    rms of what is left in the burst window after the mean and that tone.
    Measured against the absolute index, the NTSC 180-degree line alternation
    is gone (227.5 cycles a line): the phase only drifts with the clocks and
    steps by -64 degrees at every duplicated sample (+64 at a dropped one)."""
    k = np.round(T).astype(int)[:, None] + n_us(BURST_AT * 1e6) + np.arange(BURST_N)
    seg = x[k] - x[k].mean(axis=1, keepdims=True)
    lo = np.exp(-1j * W0 * k)
    B = 2 * (seg * lo).mean(axis=1)
    res = (seg - np.real(B[:, None] * np.conj(lo))).std(axis=1)
    return B, res


def references(B: np.ndarray, ok: np.ndarray, mode: str, k: float = 1 / 8, jump: float = 35.0):
    """Phase reference (unit phasor) per line, as firmware would keep it:
      line   each line's own burst
      avg    IIR average of the unit bursts (k = 1/8, ~16 lines)
      reset  avg, but a burst more than `jump` degrees off is used for its
             own line only, and a second one in a row (agreeing with the
             first) restarts the average there: the slip detector."""
    u = B / np.maximum(np.abs(B), 1e-9)
    ref = np.zeros(len(B), complex)
    jump = np.radians(jump)
    a = pend = None
    for i in range(len(B)):
        if not ok[i]:
            ref[i] = ref[i - 1] if i else 1
            continue
        if mode == "line":
            ref[i] = u[i]
            continue
        if a is None:
            a = u[i]
        if mode == "reset" and abs(np.angle(u[i] * np.conj(a))) > jump:
            ref[i] = u[i]
            if pend is not None and abs(np.angle(u[i] * np.conj(pend))) < jump:
                a, pend = (u[i] + pend) / 2, None
            else:
                pend = u[i]
            continue
        pend = None
        a = a + k * (u[i] - a)
        ref[i] = a / abs(a)
    return ref


# ---- Slips -----------------------------------------------------------------

def bitmix(codes: np.ndarray, valid: np.ndarray, T: np.ndarray) -> np.ndarray:
    """Fraction of off-ladder (bit-mixed) samples in each line."""
    cs = np.concatenate(([0], np.cumsum(~valid[codes])))
    a = np.round(T).astype(int)
    return (cs[a + int(LINE)] - cs[a]) / int(LINE)


def slip_study(T, R, B, ok, mix):
    """Slip events from steps in the burst phase, the full step per slip from
    line fits over the clean stretches, the S3-C5 clock offset two ways, and
    the hue error of the three phase references."""
    i = np.flatnonzero(ok)
    ph = np.degrees(np.unwrap(np.angle(B[i])))
    truth = median_filter(ph, 9)            # keeps steps, removes most burst noise
    tms = T[i] / FS * 1e3
    # Abrupt steps: mean of the next 8 lines minus the previous 8, local maxima > 30 deg.
    w = 8
    c = np.concatenate(([0], np.cumsum(ph)))
    D = np.zeros(len(ph))
    j = np.arange(w, len(ph) - w + 1)
    D[j] = (c[j + w] - c[j]) / w - (c[j] - c[j - w]) / w
    ev = [j for j in range(w, len(ph) - w)
          if abs(D[j]) > 30 and abs(D[j]) == np.abs(D[j - w:j + w + 1]).max()]
    # Off-ladder sawtooth: the end of each bit-mixing stretch.
    m = mix[i]
    drops = [j for j in range(w, len(m) - w)
             if m[j - w:j].mean() > 0.08 and m[j:j + w].mean() < 0.01 and m[j - 1] > 0.02]
    drops = [j for k, j in enumerate(drops) if k == 0 or j - drops[k - 1] > 50]
    # How long each stretch lasts (> 1 % mixed): the S3's sample point drifts
    # 50 ns per slip interval, so this is the width of the C5's bit transitions.
    mixing = []
    for j in drops:
        k = j - 1
        while k > 0 and m[k - 1] > 0.01:
            k -= 1
        mixing.append(tms[j] - tms[k])
    # Full step per slip: one drift slope, an offset per stretch, clean lines only (< 0.5 % mixed).
    seg = np.searchsorted(ev, np.arange(len(ph)), side="right")
    clean = m < 0.005
    nseg = len(ev) + 1
    A = np.zeros((clean.sum(), nseg + 1))
    A[:, 0] = i[clean]
    A[np.arange(clean.sum()), 1 + seg[clean]] = 1
    sol = np.linalg.lstsq(A, ph[clean], rcond=None)[0]
    slope = sol[0]                           # degrees per line between slips
    counts = np.bincount(seg[clean], minlength=nseg)
    steps = [sol[2 + k] - sol[1 + k] for k in range(len(ev)) if counts[k] > 30 and counts[k + 1] > 30]
    # Clock offset 1: one slip per interval.
    t_ev = tms[ev]
    t_slip = np.diff(t_ev).mean() if len(ev) > 1 else np.nan
    sign = -np.sign(np.mean(steps)) if steps else 1.0   # a phase step down = duplicated sample = S3 faster
    ppm_slips = sign * 1e3 / (t_slip * FS) * 1e6 if len(ev) > 1 else np.nan
    # Clock offset 2: line period in S3 samples, burst frequency in C5 samples
    # (between slips the S3 sees C5 samples one to one) and fsc = 227.5 fH:
    # f_S3 / f_C5 = P_S3 * nu / 227.5.
    r = ~np.isnan(R)
    p_s3 = np.polyfit(np.flatnonzero(r), R[r], 1)[0]
    nu = FSC / FS + slope / 360 / p_s3
    ppm_lines = (p_s3 * nu / 227.5 - 1) * 1e6
    # Hue error of each reference against the step-keeping median of the bursts.
    hue = {}
    for mode in ("line", "avg", "reset"):
        e = np.degrees(np.angle(references(B, ok, mode)[i] * np.exp(-1j * np.radians(truth))))
        hue[mode] = (np.sqrt(np.mean(e ** 2)), np.mean(np.abs(e) > 10), np.mean(np.abs(e) > 20))
    inline = np.mean(np.abs(np.diff(truth)) > 20)    # a slip inside a line's picture
    dph = np.diff(ph)
    noise_clean = np.std(dph[clean[1:] & clean[:-1]]) / np.sqrt(2)
    dirty = (m > 0.05)
    noise_dirty = np.std(dph[dirty[1:] & dirty[:-1] & (np.abs(dph) < 45)]) / np.sqrt(2)
    return dict(i=i, ph=ph, truth=truth, tms=tms, ev=ev, drops=drops, mixing=mixing, steps=steps, slope=slope,
                abrupt=[D[j] for j in ev], t_slip=t_slip, ppm_slips=ppm_slips, p_s3=p_s3,
                ppm_lines=ppm_lines, hue=hue, inline=inline, noise_clean=noise_clean,
                noise_dirty=noise_dirty, clean_frac=clean.mean(), dirty_frac=dirty.mean())


# ---- Decoders --------------------------------------------------------------

H_CHROMA = lowpass(1.0e6, 41)      # -6 dB at 1 MHz
H_FSC = (1 + np.cos(2 * W0)) / 2   # video.c's smooth filter [1 2 1] (taps 0.1 us apart) at fsc: -15 dB


def decode_line_pc(x, t, ref):
    """PC reference: U/V by mixing with the subcarrier and a 1 MHz FIR
    low-pass, luma = composite minus the remodulated chroma, then video.c's
    [1 2 1] low-pass. Returns Y, U, V (codes) at the 320 pixel centres."""
    a = int(t + H_START * FS) - 40
    k = np.arange(a, a + int(H_LEN * FS) + 80)
    lo = np.exp(-1j * W0 * k)
    C = 2 * np.convolve(x[k] * lo, H_CHROMA, mode="same")
    y = x[k] - np.real(C * np.conj(lo))
    y = np.convolve(y, [0.25, 0, 0.5, 0, 0.25], mode="same")
    pos = t + H_START * FS + (np.arange(W) + 0.5) * H_LEN * FS / W - a
    Y = np.interp(pos, np.arange(len(k)), y)
    C = np.interp(pos, np.arange(len(k)), C.real) + 1j * np.interp(pos, np.arange(len(k)), C.imag)
    C = C * 1j * np.conj(ref)
    return Y, -C.imag, C.real


def decode_line_fw(x, t, ref):
    """Firmware-style: chroma sums over 8-sample blocks from the line's integer
    start (130 per line) with per-block zero-mean cos/sin tables (no luma
    leak), [1 2 1] across blocks (-3 dB at 0.42 MHz; the blocks alone reach
    1.1 MHz), rotated by the reference. Luma is video.c's [1 2 1] smooth
    filter with the chroma it lets through (-15 dB) subtracted at each pixel,
    computed from the block chroma."""
    a = int(round(t + H_START * FS))
    nb = int(H_LEN * FS) // FW_BLOCK
    k = a + np.arange(nb * FW_BLOCK).reshape(nb, FW_BLOCK)
    tab = np.exp(-1j * W0 * k)
    tab -= tab.mean(axis=1, keepdims=True)
    S = 2 * (x[k] * tab).mean(axis=1)
    S = np.convolve(np.pad(S, 1, mode="edge"), [0.25, 0.5, 0.25], mode="valid")
    centres = k.mean(axis=1)
    pos = t + H_START * FS + (np.arange(W) + 0.5) * H_LEN * FS / W
    C = np.interp(pos, centres, S.real) + 1j * np.interp(pos, centres, S.imag)
    p = np.floor(pos).astype(int)
    f = pos - p
    smooth = lambda q: x[q - 2] * 0.25 + x[q] * 0.5 + x[q + 2] * 0.25
    y = smooth(p) * (1 - f) + smooth(p + 1) * f
    Y = y - H_FSC * np.real(C * np.exp(1j * W0 * pos))
    C = C * 1j * np.conj(ref)
    return Y, -C.imag, C.real


def decode_field(x, T, field, ref, decoder):
    Y, U, V = (np.zeros((ROWS, W)) for _ in range(3))
    for row, line in field.items():
        Y[row], U[row], V[row] = decoder(x, T[line], ref[line])
    return Y, U, V


def to_rgb(Y, U, V, lo, hi, chroma_scale, sat=1.0):
    """NTSC Y'UV (U = 0.492 (B'-Y'), V = 0.877 (R'-Y')) to R'G'B'. Luma is
    stretched lo..hi like the bridge's auto levels, chroma by the same gain
    after scaling it to luma codes with the burst (20 IRE) and sync (40 IRE)."""
    g = 1 / (hi - lo)
    y = (Y - lo) * g
    u, v = U * chroma_scale * g * sat, V * chroma_scale * g * sat
    r, b = y + v / 0.877, y + u / 0.492
    gg = (y - 0.299 * r - 0.114 * b) / 0.587
    return np.clip(np.dstack([r, gg, b]), 0, 1)


def robust_sigma(d: np.ndarray) -> float:
    return 1.4826 * np.median(np.abs(d - np.median(d)))


def jpeg_bytes(img: np.ndarray, quality: int = 60) -> int:
    """JPEG size with Pillow's libjpeg (gray, or 4:2:0 colour); IJG tables like esp_new_jpeg."""
    from PIL import Image
    buf = io.BytesIO()
    im = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8))
    im.save(buf, "JPEG", quality=quality, subsampling=2)
    return buf.tell()


# ---- Cost on the S3 --------------------------------------------------------

def cost_model(blocks: int = int(H_LEN * FS) // FW_BLOCK) -> list[tuple]:
    """Instructions per drawn line for the firmware-style colour decoder,
    counted from the loops it needs (assumptions in docs/colour-study.md):
    (stage, scalar instructions, PIE SIMD instructions, every line?, optional?)."""
    n_act = int(H_LEN * FS)
    return [
        # 45 samples x (load, 2 table loads, 2 MAC) + unit phasor / IIR / slip test in float
        ("burst + reference", BURST_N * 5 + 60, BURST_N * 5 + 60, True, False),
        # per sample: load, mask, table load, 2 multiply-add, 2 pointer steps; PIE: 16 lanes,
        # ~6 instructions per 16 samples (unaligned load, 2 table loads, 2 MAC) + block read-out
        ("chroma demod, 1044 samples", n_act * 7, n_act * 6 // 16 + blocks * 8, False, False),
        # per block: [1 2 1] (2x3 adds), rotate+scale as a 2x2 matrix (4 mul, 2 add), clamp, store
        (f"{blocks} chroma blocks: [1 2 1], rotate", blocks * 18, blocks * 18, False, False),
        # per pixel: interpolate chroma (2x2 MAC), remodulate (table, 2 MAC), subtract
        ("luma: cancel leaked chroma", W * 12, W * 12, False, True),
        # 320 more bytes per line to PSRAM (U/V for YCbYCr 4:2:2), word stores
        ("write U/V (4:2:2)", W // 4 * 2 + 40, W // 4 * 2 + 40, False, False),
    ]


def cost_totals(blocks: int) -> dict:
    """Instructions per drawn line (a per-line stage counted per drawn line),
    without and with the optional luma cancellation."""
    drawn = ROWS / 262 / 2          # drawn lines: 240 of 262, every other field at 30 fps
    out = {"scalar": 0.0, "PIE": 0.0, "luma cancel": 0.0}
    for _, scalar, simd, every, optional in cost_model(blocks):
        f = 1 / drawn if every else 1
        if optional:
            out["luma cancel"] += scalar * f
        else:
            out["scalar"] += scalar * f
            out["PIE"] += simd * f
    return out


# ---- Main ------------------------------------------------------------------

def save_png(path: pathlib.Path, img: np.ndarray) -> None:
    import matplotlib.pyplot as plt
    plt.imsave(path, np.clip(img, 0, 1), cmap="gray" if img.ndim == 2 else None, vmin=0, vmax=1)


def plot_slips(path, s, mix, B, ok, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
    i, tms = s["i"], s["tms"]
    ax[0].plot(tms, s["ph"] - s["slope"] * i, ".", ms=2, color="#1f77b4", label="burst phase, drift removed")
    for j in s["ev"]:
        ax[0].axvline(tms[j], color="#d62728", lw=0.8)
    ax[0].set_ylabel("degrees")
    ax[0].legend(loc="upper right", fontsize=8)
    ax[1].plot(tms, mix[i] * 100, ".", ms=2, color="#ff7f0e")
    ax[1].set_ylabel("off-ladder samples, %")
    for mode, col in (("line", "#9467bd"), ("reset", "#2ca02c")):
        e = np.degrees(np.angle(references(B, ok, mode)[i] * np.exp(-1j * np.radians(s["truth"]))))
        ax[2].plot(tms, e, ".", ms=2, color=col, label=f"hue error, {mode} reference")
    ax[2].set_ylim(-90, 90)
    ax[2].set_ylabel("degrees")
    ax[2].set_xlabel("time in the recording, ms")
    ax[2].legend(loc="upper right", fontsize=8)
    for a in ax:
        a.grid(alpha=0.3)
    fig.suptitle(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)


def plot_compare(path, gray, colour, colour_fw, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.4))
    for a, img, name in zip(ax, (gray, colour, colour_fw),
                            ("gray (luma only)", "colour, PC reference (1 MHz FIR)",
                             f"colour, firmware-style ({FW_BLOCK}-sample blocks)")):
        a.imshow(np.clip(img, 0, 1), cmap="gray" if img.ndim == 2 else None, vmin=0, vmax=1,
                 interpolation="nearest")
        a.set_title(name, fontsize=10)
        a.axis("off")
    fig.suptitle(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", type=pathlib.Path)
    ap.add_argument("--bits", type=int, choices=(5, 6), default=6, help="5: drop bit 0 (the keys wire)")
    ap.add_argument("--raw5", action="store_true", help="5-bit: keep the codes as received (no ladder table)")
    ap.add_argument("--field", type=int, default=0, help="which complete field to draw")
    ap.add_argument("--sat", type=float, default=1.0, help="extra saturation for the PNGs")
    args = ap.parse_args()

    codes, five, native5, valid = load(args.recording, args.bits, args.raw5)
    x = codes.astype(np.float64)
    tag = "_raw5" if five and args.raw5 else "_5bit" if five and not native5 else ""
    stem = args.recording.with_name(args.recording.stem + tag)
    print(f"{args.recording.name}: {len(x)} samples, {len(x) / FS * 1e3:.1f} ms, "
          f"{'5-bit' + (' (codes as received)' if args.raw5 else ' (codes restored by the ladder table)') if five else '6-bit'}")

    T, R, broad, sync, blank = track_lines(x)
    fl = fields(broad)
    lost = np.isnan(R).sum()
    print(f"lines {len(T)} (flywheel {lost}), line period {np.nanmean(np.diff(R)):.3f} samples, "
          f"sync {sync:.2f}, blanking {blank:.2f} codes ({blank - sync:.2f} apart), complete fields {len(fl)}")

    B, res = bursts(x, T)
    amp = np.abs(B)
    ok = ~broad & (amp > 0.4 * np.median(amp[~broad]))
    mix = bitmix(codes, valid, T)
    s = slip_study(T, R, B, ok, mix)
    snr = 10 * np.log10(np.mean(amp[ok] ** 2 / 2) / np.mean(res[ok] ** 2))
    cl, dr = ok & (mix < 0.005), ok & (mix > 0.05)
    expect = np.degrees(np.mean(res[cl]) / np.mean(amp[cl]) / np.sqrt(BURST_N / 2))
    print(f"burst: {amp[ok].mean():.2f} codes peak (sd {amp[ok].std():.2f}), residual noise {res[ok].mean():.2f} codes rms, "
          f"SNR {snr:.1f} dB per sample, {snr + 10 * np.log10(BURST_N):.1f} dB over the {BURST_N}-sample window")
    print(f"  clean lines: residual {res[cl].mean():.2f} codes, phase noise {s['noise_clean']:.1f} deg per line "
          f"(white noise would give {expect:.1f}); "
          f"bit-mixed lines (> 5 %): residual {res[dr].mean():.2f}, phase noise {s['noise_dirty']:.1f} deg")
    print(f"bit-mixed samples: {mix.mean() * 100:.1f} % of all; lines with < 0.5 %: {s['clean_frac'] * 100:.0f} %, "
          f"with > 5 %: {s['dirty_frac'] * 100:.0f} %")
    at = ", ".join("%.1f" % s["tms"][j] for j in s["ev"])
    ends = ", ".join("%.1f" % s["tms"][j] for j in s["drops"])
    abrupt = ", ".join("%+.0f" % d for d in s["abrupt"])
    steps = ", ".join("%+.0f" % d for d in s["steps"])
    print(f"slips: {len(s['ev'])} abrupt phase steps at {at} ms (abrupt part {abrupt} deg); "
          f"bit-mixing ends at {ends} ms")
    print(f"  full step per slip {steps} deg (one sample = {SLIP_DEG:.1f}); "
          f"drift between slips {s['slope']:+.3f} deg/line")
    if s["mixing"]:
        mixing = np.mean(s["mixing"])
        print(f"  bit-mixing lasts {mixing:.1f} ms before each slip ({mixing / s['t_slip'] * 100:.0f} % of the time): "
              f"the C5's bit transitions are ~{mixing / s['t_slip'] * 1e9 / FS:.0f} ns wide of the {1e9 / FS:.0f} ns sample")
    print(f"  one slip every {s['t_slip']:.1f} ms = every {s['t_slip'] * 1e-3 * FS / LINE:.0f} lines; "
          f"S3-C5 clock offset {s['ppm_slips']:+.2f} ppm from the slips, {s['ppm_lines']:+.2f} ppm from "
          f"line period ({s['p_s3']:.4f} samples) x burst frequency")
    for mode, (rms, p10, p20) in s["hue"].items():
        print(f"  hue error, {mode:5s} reference: {rms:5.1f} deg rms, {p10 * 100:5.1f} % of lines > 10 deg, "
              f"{p20 * 100:5.2f} % > 20 deg")
    print(f"  lines with a slip inside their picture: {s['inline'] * 100:.2f} %")

    if not fl:
        print("no complete field: no pictures")
        return 1
    field = fl[min(args.field, len(fl) - 1)]
    ref = references(B, ok, "reset")
    a_burst = np.median(amp[ok])
    cscale = (20 / a_burst) * (blank - sync) / 40          # chroma codes -> luma codes
    Y, U, V = decode_field(x, T, field, ref, decode_line_pc)
    Yf, Uf, Vf = decode_field(x, T, field, ref, decode_line_fw)
    lo, hi = np.percentile(Y, [1, 99.5])
    gray = np.clip((Y - lo) / (hi - lo), 0, 1)
    colour = to_rgb(Y, U, V, lo, hi, cscale, args.sat)
    colour_fw = to_rgb(Yf, Uf, Vf, lo, hi, cscale, args.sat)
    for suffix, img in (("_gray", gray), ("_colour", colour), ("_colour_fw", colour_fw)):
        save_png(stem.with_name(stem.name + suffix + ".png"), img)
    title = f"{args.recording.name}{' ' + tag[1:] if tag else ''}: field {fl.index(field)}"
    plot_compare(stem.with_name(stem.name + "_compare.png"), gray, colour, colour_fw, title)
    plot_slips(stem.with_name(stem.name + "_slips.png"), s, mix, B, ok, args.recording.name + tag)
    sat = np.hypot(U, V) * 20 / a_burst
    print(f"picture: luma {lo:.1f}..{hi:.1f} codes; chroma {np.median(sat):.1f} IRE median, "
          f"{np.percentile(sat, 95):.1f} IRE at the 95th percentile")

    # Colour noise: the same rows one frame later (same field parity), robust to what moved.
    k = fl.index(field)
    if k + 2 < len(fl):
        for name, dec in (("PC reference", decode_line_pc), ("firmware-style", decode_line_fw)):
            Y2, U2, V2 = decode_field(x, T, fl[k + 2], ref, dec)
            Y1, U1, V1 = (Y, U, V) if dec is decode_line_pc else (Yf, Uf, Vf)
            su = robust_sigma(U1 - U2) / np.sqrt(2) * 20 / a_burst
            sv = robust_sigma(V1 - V2) / np.sqrt(2) * 20 / a_burst
            sy = robust_sigma(Y1 - Y2) / np.sqrt(2) * 40 / (blank - sync)
            print(f"noise per pixel ({name}): luma {sy:.1f} IRE, U {su:.1f} IRE, V {sv:.1f} IRE "
                  f"(frame-to-frame, field {k} vs {k + 2})")
            # The same, split by rows whose lines (in either field) had bit-mixed samples.
            rm = np.maximum(mix[[field[r] for r in range(ROWS)]], mix[[fl[k + 2][r] for r in range(ROWS)]])
            for cname, rows in (("clean rows", rm < 0.005), ("bit-mixed rows (> 5 %)", rm > 0.05)):
                if rows.sum() >= 10:
                    uv = np.concatenate([(U1 - U2)[rows].ravel(), (V1 - V2)[rows].ravel()])
                    print(f"  {cname:22s} ({rows.sum():3d}): luma {robust_sigma((Y1 - Y2)[rows]) / np.sqrt(2) * 40 / (blank - sync):.1f} IRE, "
                          f"U/V {robust_sigma(uv) / np.sqrt(2) * 20 / a_burst:.1f} IRE")
    try:
        g, c, cf = jpeg_bytes(gray), jpeg_bytes(colour), jpeg_bytes(colour_fw)
        print(f"JPEG q60 (Pillow): gray {g / 1024:.1f} KiB, colour 4:2:0 {c / 1024:.1f} KiB (x{c / g:.2f}), "
              f"firmware-style {cf / 1024:.1f} KiB (x{cf / g:.2f})")
    except ImportError:
        print("JPEG sizes skipped (no Pillow)")

    budget = 63.5556e-6 * CPU_HZ
    drawn = ROWS / 262 / 2
    print(f"S3 cost per drawn line (line budget {budget:.0f} cycles at 240 MHz):")
    for stage, scalar, simd, every, optional in cost_model():
        note = " (every line)" if every else " (optional)" if optional else ""
        print(f"  {stage:32s} scalar ~{scalar:6d}, PIE ~{simd:6d} instructions{note}")
    for blocks in (80, int(H_LEN * FS) // FW_BLOCK, 160):
        c = cost_totals(blocks)
        parts = ", ".join(f"{name} ~{c[name]:5.0f} = {c[name] / budget * 100:3.0f} % of a line "
                          f"(+{c[name] * drawn / budget * 100:2.0f} % average)" for name in ("scalar", "PIE"))
        print(f"  {blocks:3d} chroma px: {parts}; luma cancel +{c['luma cancel'] / budget * 100:.0f} % of a line")
    print("  (instructions; ~1 per cycle on the LX7 at best, 1.2-1.5x with load stalls)")
    print(f"wrote {stem.name}_colour.png, _colour_fw.png, _gray.png, _compare.png, _slips.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
