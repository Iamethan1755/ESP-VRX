"""Analytic IQLaneFusion reference, C5VRX by Twotoz and contributors.

Extends the untrained U85/PAIR and history-likelihood research. No fitted
weights, image training, or active-SRAM source. This is a host reference,
not a 40 MS/s CPU implementation. ADC units are signed Q10 LSBs.
"""
from dataclasses import dataclass
from functools import lru_cache
from itertools import combinations, product
import math
import numpy as np
from scipy.special import ndtr


@dataclass(frozen=True)
class Plan:
    # Ordered low-to-high captured nibble bits; ordering is part of the ABI.
    i: tuple
    q: tuple
    name: str = "custom"

    def __post_init__(self):
        for bits in (self.i, self.q):
            if len(bits) != 4 or len(set(bits)) != 4 or any(b not in range(10) for b in bits):
                raise ValueError("each component needs four distinct Q10 bits")

    @property
    def key(self):
        return self.i, self.q


COARSE = Plan((6, 7, 8, 9), (6, 7, 8, 9), "coarse")
FINE = Plan((5, 6, 7, 9), (5, 6, 7, 9), "fine")
ULTRA = Plan((4, 5, 6, 9), (4, 5, 6, 9), "ultrafine")
SHADOW_PLANS = (COARSE, FINE, ULTRA,
                Plan((4, 6, 8, 9), (4, 6, 8, 9), "mixed468"),
                Plan((3, 5, 7, 9), (3, 5, 7, 9), "mixed357"),
                Plan((4, 5, 8, 9), (3, 6, 7, 9), "asymmetric"))


def adc10(x):
    """Actual flooring/saturating ADC, BEFORE bit extraction/folding."""
    return np.clip(np.floor(x), -512, 511).astype(np.int16)


def component_code(x, bits):
    unsigned = np.asarray(x, dtype=np.int64) & 1023
    out = np.zeros(unsigned.shape, np.uint8)
    for j, bit in enumerate(bits):
        out |= (((unsigned >> bit) & 1) << j).astype(np.uint8)
    return out


def capture(z, plan=ULTRA):
    z = np.asarray(z)
    return (component_code(adc10(z.real), plan.i) << 4) | component_code(adc10(z.imag), plan.q)


@lru_cache(None)
def intervals(bits):
    """Exact union of ADC input intervals per token, including clipped tails."""
    codes = component_code(np.arange(-512, 512), bits)
    cells = []
    for token in range(16):
        values = np.flatnonzero(codes == token) - 512
        spans = []
        for x in values:
            if spans and spans[-1][1] == x:
                spans[-1][1] = float(x + 1)
            else:
                spans.append([float(x), float(x + 1)])
        if spans[0][0] == -512:
            spans[0][0] = -math.inf
        if spans[-1][1] == 512:
            spans[-1][1] = math.inf
        cells.append(tuple(tuple(s) for s in spans))
    return tuple(cells)


@dataclass(frozen=True)
class Context:
    radii: tuple = (64.,)
    sigma: float = 8.                 # per-axis ADC noise, not an RF C/N claim
    dc_i: float = 0.
    dc_q: float = 0.
    iq_ratio: float = 1.
    skew_rad: float = 0.
    fs: float = 40e6
    min_hz: float = -12e6
    max_hz: float = 12e6
    frequency_diffusion_hz: float = 2e6

    def __post_init__(self):
        if not self.radii or any(r <= 0 or not math.isfinite(r) for r in self.radii):
            raise ValueError("positive finite radius hypotheses required")
        if not (math.isfinite(self.sigma) and self.sigma > 0 and self.fs > 0 and self.iq_ratio > 0):
            raise ValueError("invalid noise, sample rate or IQ ratio")
        if not -self.fs / 2 < self.min_hz < self.max_hz < self.fs / 2:
            raise ValueError("frequency support must lie inside acquired Nyquist")
        if not all(math.isfinite(v) for v in (self.dc_i, self.dc_q, self.skew_rad, self.iq_ratio)):
            raise ValueError("finite context required")
        if not math.isfinite(self.frequency_diffusion_hz) or self.frequency_diffusion_hz <= 0:
            raise ValueError("positive frequency diffusion required")

    def scaled(self, gain):
        if gain <= 0 or not math.isfinite(gain):
            raise ValueError("positive gain ratio required")
        # Frozen input/noise model only. RF-stage NF/DC changes need measured contexts.
        return Context(tuple(r * gain for r in self.radii), self.sigma * gain,
                       self.dc_i, self.dc_q, self.iq_ratio, self.skew_rad,
                       self.fs, self.min_hz, self.max_hz, self.frequency_diffusion_hz)


def phases(n):
    if n < 16 or n % 2:
        raise ValueError("even phase grid >=16 required")
    return np.arange(n) * (2 * np.pi / n)


def axis_likelihood(bits, means, sigma):
    out = np.zeros((16, len(means)))
    for token, spans in enumerate(intervals(tuple(bits))):
        for lo, hi in spans:
            out[token] += ndtr((hi - means) / sigma) - ndtr((lo - means) / sigma)
    return out


def likelihood(plan, ctx, n=128):
    """P(raw byte | phase): integrate all cell aliases and radius hypotheses.

    Axis noise is independent in this reference; temporal/channel-correlated
    and non-Gaussian noise are explicit model mismatch tests, not hidden facts.
    """
    theta = phases(n)
    out = np.zeros((256, n))
    for r in ctx.radii:
        il = axis_likelihood(plan.i, r * np.cos(theta) + ctx.dc_i, ctx.sigma)
        ql = axis_likelihood(plan.q, r * ctx.iq_ratio * np.sin(theta + ctx.skew_rad) + ctx.dc_q, ctx.sigma)
        out += (il[:, None, :] * ql[None, :, :]).reshape(256, n) / len(ctx.radii)
    return out


def delta_kernel(ctx, n):
    hz = np.fft.fftfreq(n) * ctx.fs
    kernel = ((hz >= ctx.min_hz) & (hz <= ctx.max_hz)).astype(float)
    kernel /= kernel.sum()
    return hz, kernel


def delta_joint(previous, next_likelihood, kernel):
    # For every possible next raw byte, sum p(previous theta)*L(theta+delta).
    joint = np.fft.ifft(np.fft.fft(next_likelihood, axis=-1) *
                        np.conj(np.fft.fft(previous)), axis=-1).real
    return np.maximum(joint, 0.) * kernel


def frequency_risk(previous, next_likelihood, ctx):
    """Bayes MSE of the *linear, bounded* frequency increment, in Hz^2.

    Screening lower bound: the multimode reference decoder is deliberately
    more conservative than a posterior mean. A modulo-2pi loss would hide
    full-turn FM errors; never use it as the final frequency objective.
    """
    hz, kernel = delta_kernel(ctx, next_likelihood.shape[1])
    joint = delta_joint(previous, next_likelihood, kernel)
    prob = joint.sum(axis=1)
    first = joint @ hz
    second = joint @ (hz * hz)
    return float(max(0., np.sum(second - first * first / np.maximum(prob, 1e-300))))


def expected_pair_risk(current_likelihood, next_likelihood, ctx):
    """Average over *observed* current tokens, not an unobserved uniform phase.

    Uniform phase plus just one future observation cannot identify a frequency
    increment. Conditioning on the current byte is essential.
    """
    sums = current_likelihood.sum(axis=1)
    risk = 0.
    for byte, total in enumerate(sums):
        probability = total / current_likelihood.shape[1]
        if probability > 1e-12:
            risk += probability * frequency_risk(current_likelihood[byte] / total, next_likelihood, ctx)
    return float(risk)


@dataclass
class Decode:
    hz: np.ndarray
    ambiguity: np.ndarray
    concentration: np.ndarray
    posterior: np.ndarray
    frequency_posterior: np.ndarray


def decode(raw, plan, ctx, n=128, reset_at=()):
    """Factorized phase/frequency posterior; no reset at DMA boundaries.

    reset_at is ONLY for actual context changes/gaps. Multimodal delta output
    uses the mean inside the strongest contiguous half-height peak and reports
    the mass outside it. No stale last-frequency hold or manufactured sync.
    """
    L = likelihood(plan, ctx, n)
    hz, kernel = delta_kernel(ctx, n)
    support = kernel > 0
    evolution = np.exp(-.5 * ((hz[:, None] - hz[None, :]) / ctx.frequency_diffusion_hz) ** 2)
    evolution[~support, :] = 0
    evolution /= np.maximum(evolution.sum(axis=0), 1e-300)
    frequency = kernel.copy()
    theta = phases(n)
    previous = np.full(n, 1 / n)
    out = np.zeros(len(raw))
    ambiguity = np.ones(len(raw))
    concentration = np.zeros(len(raw))
    resets = set(reset_at)
    fresh = True
    for k, byte in enumerate(raw):
        if k in resets:
            previous = np.full(n, 1 / n)
            frequency = kernel.copy()
            fresh = True
        obs = L[int(byte)]
        prior_frequency = evolution @ frequency
        predicted = np.maximum(np.fft.ifft(np.fft.fft(previous) * np.fft.fft(prior_frequency)).real, 0.)
        post = obs * predicted
        if post.sum() < 1e-290:
            # Contradiction: discard false certainty and re-acquire this sample.
            post = obs.copy()
            frequency = kernel.copy()
            fresh = True
        total = post.sum()
        if total < 1e-290:
            previous = np.full(n, 1 / n)
            frequency = kernel.copy()
            fresh = True
            continue
        post /= total
        concentration[k] = abs(post @ np.exp(1j * theta))
        if not fresh:
            d = delta_joint(previous, obs[None, :], prior_frequency)[0]
            d /= max(d.sum(), 1e-300)
            # Sort in physical frequency order: never connect +/-Nyquist ends.
            order = np.argsort(hz)
            weights = d[order]
            peak = int(np.argmax(weights))
            lo = hi = peak
            while lo > 0 and weights[lo - 1] >= weights[peak] / 2:
                lo -= 1
            while hi + 1 < n and weights[hi + 1] >= weights[peak] / 2:
                hi += 1
            branch = order[lo:hi + 1]
            mass = d[branch].sum()
            out[k] = d[branch] @ hz[branch] / max(mass, 1e-300)
            ambiguity[k] = max(0., min(1., 1 - mass))
            frequency = d
        previous = post
        fresh = False
    return Decode(out, ambiguity, concentration, previous, frequency)


def all_plans(signs=True, symmetric=False):
    bits = [tuple(c) + (9,) for c in combinations(range(9), 3)] if signs else list(combinations(range(10), 4))
    pairs = ((b, b) for b in bits) if symmetric else product(bits, repeat=2)
    for i, q in pairs:
        yield Plan(i, q, "I" + "".join(map(str, i)) + "Q" + "".join(map(str, q)))


def phase_risk(L):
    """Cheap analytic screening only; NOT an output-video score."""
    n = L.shape[1]
    moment = L @ np.exp(1j * phases(n)) / n
    return float(max(0., 1 - np.abs(moment).sum()))


def select(plans, contexts, previous=None, gains=(1.,), n=64, shortlist=12):
    """Bounded model-based search: cheap screen, then worst-context Hz^2 risk.

    Contexts express uncertainty; reject improvements that only work in one
    hypothesis. No historical reward fitting. Gain comparisons assume a frozen
    linear receive chain; real gain tuples require separately measured contexts.
    """
    import heapq
    if not contexts or shortlist < 1:
        raise ValueError("contexts and a positive shortlist required")
    contexts = tuple(contexts)
    if previous is not None:
        previous = np.asarray(previous, float)
        if previous.shape != (n,) or np.any(previous < 0) or not np.isfinite(previous).all() or previous.sum() <= 0:
            raise ValueError("invalid phase posterior")
        previous = previous / previous.sum()
    heap, count, cache = [], 0, {}
    def axes(bits, ctx, axis):
        key = (bits, ctx, axis)
        if key not in cache:
            theta = phases(n)
            cache[key] = tuple(axis_likelihood(bits, r * (np.cos(theta) if axis == 0 else
                             ctx.iq_ratio * np.sin(theta + ctx.skew_rad)) +
                             (ctx.dc_i if axis == 0 else ctx.dc_q), ctx.sigma) for r in ctx.radii)
        return cache[key]
    for plan in plans:
        for gain in gains:
            worst = 0.
            for ctx in contexts:
                c = ctx.scaled(gain)
                il, ql = axes(plan.i, c, 0), axes(plan.q, c, 1)
                L = sum((i[:, None, :] * q[None, :, :]).reshape(256, n)
                        for i, q in zip(il, ql)) / len(il)
                worst = max(worst, phase_risk(L))
            entry = (-worst, count, plan, gain)
            count += 1
            if len(heap) < shortlist:
                heapq.heappush(heap, entry)
            elif worst < -heap[0][0]:
                heapq.heapreplace(heap, entry)
    ranked = []
    for neg_screen, _, plan, gain in heap:
        risks = []
        for c in contexts:
            next_L = likelihood(plan, c.scaled(gain), n)
            risk = (expected_pair_risk(likelihood(ULTRA, c, n), next_L, c) if previous is None else
                    frequency_risk(previous, next_L, c))
            risks.append(risk)
        ranked.append(dict(plan=plan, gain=gain, screen=-neg_screen, risk_hz2=max(risks), risks=risks))
    ranked.sort(key=lambda r: (r['risk_hz2'], r['screen'], r['plan'].key, r['gain']))
    return dict(candidates=count, ranked=ranked, best=ranked[0] if ranked else None,
                guarantee="shortlisted model optimum; not global optimum or RF range")


def route_transition(z, old, new, start, samples_per_write=1):
    """Model sequential GPIO writes, including briefly mixed component maps."""
    if start < 0 or samples_per_write < 1:
        raise ValueError("invalid transition timing")
    z = np.asarray(z)
    raw = capture(z, old)
    i, q = list(old.i), list(old.q)
    writes = [(axis, bit) for bit in range(4) for axis in ('q', 'i')
              if (old.q if axis == 'q' else old.i)[bit] != (new.q if axis == 'q' else new.i)[bit]]
    epochs = np.zeros(len(z), np.uint16)
    for k, (axis, bit) in enumerate(writes):
        at = start + k * samples_per_write
        (q if axis == 'q' else i)[bit] = (new.q if axis == 'q' else new.i)[bit]
        # A sequential swap can temporarily duplicate a source line. It is
        # a real intermediate route, although not a valid final Plan.
        raw[at:] = (component_code(adc10(z[at:].real), i) << 4) | component_code(adc10(z[at:].imag), q)
        epochs[at:] = k + 1
    return raw, epochs


class DecisionGate:
    """Single-owner mathematical proposal gate; no hardware actuator here."""
    def __init__(self, improvement=.10, dwell_us=5000, confirmations=3):
        self.improvement, self.dwell_us, self.confirmations = improvement, dwell_us, confirmations
        self.epoch = None
        self.pending = None
        self.count = 0
        self.last_us = None

    def propose(self, key, current_risks, proposed_risks, now_us, epoch, valid=True):
        if self.epoch != epoch or (self.last_us is not None and now_us < self.last_us):
            self.epoch, self.pending, self.count, self.last_us = epoch, None, 0, now_us
        self.last_us = now_us
        a, b = np.asarray(current_risks), np.asarray(proposed_risks)
        if not valid or not a.size or a.shape != b.shape or not np.isfinite(a).all() or not np.isfinite(b).all() or np.any(a <= 0) or np.any(b < 0) or not np.all(b < a * (1 - self.improvement)):
            self.pending, self.count = None, 0
            return False
        if self.pending != key:
            self.pending, self.count, self.since_us = key, 1, now_us
        else:
            self.count += 1
        return self.count >= self.confirmations and now_us - self.since_us >= self.dwell_us
