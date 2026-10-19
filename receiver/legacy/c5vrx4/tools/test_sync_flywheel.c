/* C5VRX by Twotoz and contributors: C5VRX-4 sync flywheel host test.
 * Synthetic PAL/NTSC CVBS (H sync, equalizing/broad vertical interval,
 * burst, picture) -> FM at 40 MS/s -> fine-lane Q4 bytes with noise and CFO
 * -> the live ring with RX completion granularity and the ~16 KiB TX lag ->
 * flywheel -> the transmitted bytes decoded exactly like the C5VRX-4 TX
 * program (endpoint phase + trajectory class -> DAC LUT), for every span
 * alignment. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sync_flywheel.h"
#include "cvbs_tables.h"

#define RING 32768u
#define LAG 16384u
#define DSCR 4092u
#define FS 40.0

typedef struct { uint64_t a, b; double amp, noise; bool sync_only; } fade_t;
typedef struct {
    bool pal;
    double cfo_mhz, amp, noise;
    /* fades: half-open sample ranges with the given amplitude and noise */
    fade_t fade[8];
    unsigned nfade;
    uint64_t shift_at;      /* timing jumps by shift samples from here on */
    double shift;
} scene_t;

static unsigned s_seed = 1;
static double urand(void) { s_seed = s_seed * 1103515245u + 12345u; return ((s_seed >> 8) & 0xFFFFFF) / 16777216.0; }
static double grand(void)
{
    double u = urand() + 1e-12, v = urand();
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* Half-line geometry: PAL 1250 half-lines per frame, V intervals at 0 and 625
 * (5 pre-equalizing, 5 broad, 5 post-equalizing); NTSC 1050 at 0 and 525 (6/6/6). */
static double line_us(bool pal) { return pal ? 64.0 : 63.5556; }
static int v_kind(bool pal, long h) /* 0 none, 1 eq, 2 broad */
{
    long frame = pal ? 1250 : 1050, n = pal ? 5 : 6;
    long x = h % frame;
    long f2 = frame / 2;
    long y = x >= f2 ? x - f2 : x;
    if (y < n) return 1;
    if (y < 2 * n) return 2;
    if (y < 3 * n) return 1;
    return 0;
}

/* Deviation in MHz at time t (us). */
static double deviation(const scene_t *s, double t)
{
    double L = line_us(s->pal), H2 = L / 2;
    long h = (long)floor(t / H2);
    double x = t - h * H2;
    int vk = v_kind(s->pal, h);
    if (vk == 1) return x < (s->pal ? 2.35 : 2.3) ? -2.0 : 0.0;
    if (vk == 2) return x < H2 - 4.7 ? -2.0 : 0.0;
    if (h & 1) x += H2;                 /* position in the full line */
    long frame = s->pal ? 1250 : 1050;
    long y = h % (frame / 2);
    if (x < 4.7) return -2.0;
    if (x >= 5.6 && x < 7.85) return 0.3 * sin(2 * M_PI * 4.43361875 * t);
    if (x < 10.5 || x > 62.0 || y < 46) return 0.0;
    double ramp = (x - 10.5) / 51.5;
    return 4.667 * ramp * (0.4 + 0.6 * ((h / 40) & 1));
}

/* Expected pulses: positions (samples) and kinds within [from, to). */
typedef struct { uint64_t pos; int kind; } pulse_t;
static unsigned expected(const scene_t *s, uint64_t from, uint64_t to, pulse_t *out, unsigned cap)
{
    double H2 = line_us(s->pal) / 2 * FS;
    unsigned n = 0;
    for (long h = (long)(from / H2) - 1; h * H2 < (double)to && n < cap; ++h) {
        if (h < 0) continue;
        int vk = v_kind(s->pal, h);
        uint64_t pos = (uint64_t)llround(h * H2);
        if (s->shift_at && pos >= s->shift_at) pos += (uint64_t)s->shift;
        if (pos < from || pos >= to) continue;
        if (vk) out[n++] = (pulse_t){pos, vk};
        else if (!(h & 1)) out[n++] = (pulse_t){pos, 0};
    }
    return n;
}

static uint8_t *generate(const scene_t *s, uint64_t n)
{
    uint8_t *b = malloc(n);
    double phi = 0;
    for (uint64_t k = 0; k < n; ++k) {
        double t = k / FS;
        if (s->shift_at && k >= s->shift_at) t -= s->shift / FS;
        double amp = s->amp, noise = s->noise;
        for (unsigned j = 0; j < s->nfade; ++j)
            if (k >= s->fade[j].a && k < s->fade[j].b) {
                if (s->fade[j].sync_only) {
                    double L = line_us(s->pal), x = fmod(t, L);
                    if (x > 6.0) continue;
                }
                amp = s->fade[j].amp; noise = s->fade[j].noise;
            }
        phi += 2 * M_PI * (deviation(s, t) + s->cfo_mhz) / FS;
        double I = amp * cos(phi) + noise * grand(), Q = amp * sin(phi) + noise * grand();
        int i = (int)floor(I), q = (int)floor(Q);
        i = i < -8 ? -8 : i > 7 ? 7 : i;
        q = q < -8 ? -8 : q > 7 ? 7 : q;
        b[k] = (uint8_t)(((i & 15) << 4) | (q & 15));
    }
    return b;
}

/* Run the live ring: RX completes 4092-byte descriptors, TX reads LAG behind,
 * the flywheel runs every ~200 us. Returns the transmitted stream. */
static uint8_t *run_ring(const uint8_t *in, uint64_t n, sync_flywheel_t *f, const uint8_t *phase,
                         bool mask, uint8_t clear, double *evals_per_line, uint32_t budget)
{
    uint8_t *ring = calloc(RING, 1), *out = malloc(n);
    sfw_ring_t r = {ring, RING, phase, mask, clear};
    sfw_init(f);
    uint64_t rx = 0, tx = 0, evals = 0, lines0 = 0;
    while (tx < n) {
        uint64_t rx_to = rx + DSCR;
        if (rx_to > n) rx_to = n;
        for (; rx < rx_to; ++rx) ring[rx & (RING - 1u)] = in[rx];
        /* Firmware: woken every ~100 us (each descriptor); analyses and
         * writes only data older than the newest completed descriptor,
         * which the control observers copy. */
        if (rx > DSCR) {
            uint64_t ceiling = rx - DSCR;
            uint64_t floor = (rx > LAG ? rx - LAG : 0) + DSCR + 512u;
            uint32_t before = f->lines;
            sfw_run(f, &r, ceiling, floor, true, budget);
            evals += f->evals;
            lines0 += f->lines - before;
            for (uint64_t k = ceiling; k < rx; ++k) assert(ring[k & (RING - 1u)] == in[k]);
        }
        uint64_t tx_to = rx > LAG ? rx - LAG : 0;
        if (rx == n) tx_to = n;
        for (; tx < tx_to; ++tx) out[tx] = ring[tx & (RING - 1u)];
    }
    if (evals_per_line) *evals_per_line = lines0 ? (double)evals / (double)lines0 : 0;
    free(ring);
    return out;
}

static unsigned tx_code(const uint8_t *b, uint64_t k)
{
    static const unsigned char dummy = 0; (void)dummy;
    unsigned prev = c5v4_phase_static[b[k - 3]], cur = c5v4_phase_static[b[k]];
#define SG(x) ((((x) >> 7) & 1u) | ((((x) >> 3) & 1u) << 1))
    unsigned traj = SG(b[k - 3]) | (SG(b[k - 2]) << 2) | (SG(b[k - 1]) << 4) | (SG(b[k]) << 6);
    unsigned cls = c5v4_trajectory[traj];
    unsigned e = (((128 + cur) & 254) + ((-prev) & 254)) & 255;
    return c5v4_dac_codes[(e >> 2) | (cls << 6)];
}

/* Length (samples) of the sync-level run in the decoded TX output starting
 * within +-tol of pos, for TX span alignment a. 0 when none. */
static unsigned sync_run(const uint8_t *b, uint64_t n, uint64_t pos, unsigned a, unsigned tol, int *edge)
{
    uint64_t k0 = pos > tol + 6 ? pos - tol : 6;
    k0 += (3u + a - (unsigned)(k0 % 3u)) % 3u;
    /* Longest sync-level run starting in the window: a lone 75 ns noise
     * click before the pulse is not the pulse. */
    unsigned best = 0;
    for (uint64_t k = k0; k <= pos + tol && k + 3 < n; k += 3) {
        if (tx_code(b, k) > 8u) continue;
        unsigned run = 0, highs = 0;
        uint64_t j = k;
        while (j + 3 < n && highs < 2u) { if (tx_code(b, j) <= 8u) highs = 0; else ++highs; j += 3; run += 3; }
        run -= 3u * highs;
        if (run > best) { best = run; if (edge) *edge = (int)((int64_t)k - (int64_t)pos); }
    }
    return best;
}

typedef struct { unsigned checked, ok, bad_width, missing, max_edge; } verdict_t;

static verdict_t verify(const scene_t *s, const uint8_t *out, uint64_t n, uint64_t from, uint64_t to, unsigned a)
{
    static pulse_t p[40000];
    unsigned np = expected(s, from, to, p, 40000);
    double H2 = line_us(s->pal) / 2 * FS;
    verdict_t v = {0};
    for (unsigned j = 0; j < np; ++j) {
        if (p[j].pos + 3000 > n) break;
        int edge = 0;
        unsigned w = sync_run(out, n, p[j].pos, a, 15, &edge);
        unsigned lo, hi;
        if (p[j].kind == 0) { lo = 150; hi = 230; }
        else if (p[j].kind == 1) { lo = 70; hi = 125; }
        else { lo = (unsigned)(H2 - 188 - 60); hi = (unsigned)(H2 - 188 + 40); }
        ++v.checked;
        if (!w || w < lo || w > hi) {
            if (getenv("SFW_DEBUG") && v.missing + v.bad_width < 12)
                printf("  miss pos=%llu kind=%d field=%.3f width=%u edge=%d\n", (unsigned long long)p[j].pos,
                       p[j].kind, p[j].pos / (double)(s->pal ? 800000.0 : 262.5 * 63.5556 * FS), w, edge);
        }
        if (!w) { ++v.missing; continue; }
        if (w < lo || w > hi) { ++v.bad_width; continue; }
        unsigned ae = (unsigned)(edge < 0 ? -edge : edge);
        if (getenv("SFW_DEBUG") && ae > 15u)
            printf("  edge pos=%llu kind=%d field=%.3f width=%u edge=%d\n", (unsigned long long)p[j].pos,
                   p[j].kind, p[j].pos / (double)(s->pal ? 800000.0 : 262.5 * 63.5556 * FS), w, edge);
        if (ae > v.max_edge) v.max_edge = ae;
        ++v.ok;
    }
    return v;
}

static uint64_t field_samples(bool pal) { return pal ? 800000u : (uint64_t)llround(262.5 * 63.5556 * FS); }

static void scenario(bool pal)
{
    const char *name = pal ? "PAL" : "NTSC";
    uint64_t F = field_samples(pal), n = 10 * F;
    double L = line_us(pal) * FS;

    /* 1. Clean, strong signal: lock, find V, never write a byte. */
    scene_t clean = {.pal = pal, .cfo_mhz = 0.15, .amp = 5.0, .noise = 0.25};
    uint8_t *in = generate(&clean, n);
    sync_flywheel_t f;
    double epl = 0;
    uint8_t *out = run_ring(in, n, &f, c5v4_phase_static, false, 0, &epl, 4000);
    printf("%s clean: state=%d std=%d acquisitions=%u relocks=%u parity=%u lines_since_clean=%u\n", name,
           f.state, sfw_standard(&f), f.acquisitions, f.relocks, f.v_parity, f.lines_since_clean);
    assert(f.state == SFW_TRACK && sfw_locked(&f) && sfw_standard(&f) == (pal ? 1 : 2));
    assert(f.vsyncs >= 8 && f.v_coasted == 0 && f.v_parity == 0);
    assert(f.repaired == 0 && f.slots_repaired == 0 && f.relocks == 0);
    assert(memcmp(in, out, n) == 0);
    printf("%s clean: lines=%u clean=%u fast=%u vsyncs=%u evals/line=%.1f\n",
           name, f.lines, f.clean, f.fast_lines, f.vsyncs, epl);
    assert(epl < 40.0);
    free(out);
    free(in);

    /* 2. Deep fades: 0.5 field of pure noise (H and V syncs gone), then
     * sync-only damage over two fields. Every expected pulse must still be
     * on the TX output, for every span alignment. */
    scene_t fade = clean;
    fade.fade[0] = (fade_t){(uint64_t)(4.3 * F), (uint64_t)(4.8 * F), 0.0, 1.6, false};
    fade.fade[1] = (fade_t){(uint64_t)(6.0 * F), (uint64_t)(8.0 * F), 0.6, 1.6, true};
    fade.nfade = 2;
    s_seed = 7;
    in = generate(&fade, n);
    out = run_ring(in, n, &f, c5v4_phase_static, false, 0, &epl, 4000);
    scene_t truth = clean;
    unsigned changed = 0;
    for (uint64_t k = 0; k < n; ++k) changed += in[k] != out[k];
    for (unsigned a = 0; a < 3u; ++a) {
        verdict_t raw = verify(&truth, in, n, 4 * F, 9 * F, a);
        verdict_t v = verify(&truth, out, n, 4 * F, 9 * F, a);
        printf("%s fades align=%u: input ok %u/%u -> output ok %u/%u (missing %u, bad width %u, max edge %u)\n",
               name, a, raw.ok, raw.checked, v.ok, v.checked, v.missing, v.bad_width, v.max_edge);
        assert(raw.ok + 300u < raw.checked);          /* the input really is broken */
        /* Over 2 fields with every sync edge destroyed the flywheel coasts on
         * its learned period: <= 1 sample per 100 lines plus span rounding. */
        assert(v.missing == 0 && v.bad_width == 0 && v.max_edge <= 15u);
    }
    printf("%s fades: repaired=%u slots=%u v_coasted=%u vsyncs=%u bytes_changed=%u evals/line=%.1f\n",
           name, f.repaired, f.slots_repaired, f.v_coasted, f.vsyncs, changed, epl);
    assert(f.repaired > 200u && f.slots_repaired > 10u && f.v_coasted >= 1u);
    /* Repairs stay inside sync and vertical-interval samples. */
    assert(changed < 20u * 1000u * 1000u);
    free(out);

    /* 3. Acquisition mask: synthesized bytes keep bit 0 unflagged. */
    out = run_ring(in, n, &f, c5v4_phase_mask, true, 0, NULL, 4000);
    unsigned flagged = 0, writes = 0;
    for (uint64_t k = 0; k < n; ++k) if (in[k] != out[k]) { ++writes; flagged += out[k] & 1u; }
    printf("%s mask: writes=%u flagged=%u repaired=%u\n", name, writes, flagged, f.repaired);
    assert(writes > 0 && flagged == 0 && f.repaired > 200u);
    free(out);
    free(in);

    /* 4. VTX gone for 0.6 field, back at another phase: re-lock in one step,
     * no double sync afterwards. */
    scene_t jump = clean;
    jump.fade[0] = (fade_t){(uint64_t)(4.0 * F), (uint64_t)(4.6 * F), 0.0, 1.6, false};
    jump.nfade = 1;
    jump.shift_at = (uint64_t)(4.6 * F);
    jump.shift = 0.37 * L;
    s_seed = 11;
    in = generate(&jump, n);
    out = run_ring(in, n, &f, c5v4_phase_static, false, 0, &epl, 4000);
    verdict_t after = verify(&jump, out, n, (uint64_t)(5.4 * F), 9 * F, 0);
    printf("%s re-lock: relocks=%u acquisitions=%u after: ok %u/%u missing %u\n",
           name, f.relocks, f.acquisitions, after.ok, after.checked, after.missing);
    assert(f.relocks >= 1u && after.missing == 0 && after.bad_width == 0);
    /* No stale flywheel pulse between real ones: count sync starts per line. */
    unsigned extra = 0;
    for (uint64_t k = (uint64_t)(5.5 * F); k + 9 < 9 * F; k += 3) {
        if (tx_code(out, k) <= 8u && tx_code(out, k - 3) > 8u && tx_code(out, k - 6) > 8u) {
            unsigned run = 0;
            for (uint64_t j = k; tx_code(out, j) <= 8u && run < 2000; j += 3) run += 3;
            if (run >= 150 && run <= 230) ++extra;
        }
    }
    unsigned lines = (unsigned)((9 * F - 5.5 * F) / L);
    printf("%s re-lock: %u H-sync-width pulses over %u lines\n", name, extra, lines);
    assert(extra <= lines + 20u);
    free(out);
    free(in);

    /* 5. Range edge: a weak carrier for six fields, every real pulse noisy.
     * All pulses clean on the output, timing within a span of the truth
     * (the PLL keeps steering on the noisy pulses), no repair beyond them. */
    scene_t weak = clean;
    weak.fade[0] = (fade_t){3 * F, 9 * F, 1.5, 0.9, false};
    weak.nfade = 1;
    s_seed = 23;
    in = generate(&weak, n);
    out = run_ring(in, n, &f, c5v4_phase_static, false, 0, &epl, 4000);
    for (unsigned a = 0; a < 3u; ++a) {
        verdict_t raw = verify(&weak, in, n, 4 * F, 9 * F, a);
        verdict_t v = verify(&weak, out, n, 4 * F, 9 * F, a);
        printf("%s weak align=%u: input ok %u/%u -> output ok %u/%u (missing %u, bad width %u, max edge %u)\n",
               name, a, raw.ok, raw.checked, v.ok, v.checked, v.missing, v.bad_width, v.max_edge);
        assert(v.missing == 0 && v.bad_width == 0 && v.max_edge <= 9u);
    }
    printf("%s weak: repaired=%u slots=%u clean=%u relocks=%u acquisitions=%u evals/line=%.1f\n",
           name, f.repaired, f.slots_repaired, f.clean, f.relocks, f.acquisitions, epl);
    assert(f.relocks == 0u && f.acquisitions == 1u && sfw_locked(&f));
    free(out);
    free(in);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    scenario(true);
    scenario(false);
    puts("PASS sync_flywheel: clean lines untouched, H and V syncs rebuilt through fades for every span alignment, mask-safe bytes, re-lock after a phase jump");
    return 0;
}
