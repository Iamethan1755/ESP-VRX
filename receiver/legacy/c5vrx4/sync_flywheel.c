/* C5VRX by Twotoz and contributors: C5VRX-4 sync flywheel (see header).
 * Ported in idea from the C5VRX-3 flywheel on feat/sync-flywheel; the
 * detection, synthesis, vertical interval and re-lock are new. */
#include "sync_flywheel.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SFW_HOT IRAM_ATTR
#else
#define SFW_HOT
#endif

/* ITU-R BT.470 geometry in 40 MS/s samples. */
#define HSYNC          188u   /* 4.7 us */
#define PORCH          48u    /* front porch part rebuilt with a pulse */
#define WIN_LOCKED     24
#define WIN_UNLOCKED   96
#define CLEAN_ERR      6
#define LOCK_CLEAN     32u
#define LOST_LINES     40000u /* ~2.5 s: the idle raster has taken over by then */
#define RELOCK_EVERY   8u
#define RELOCK_AFTER   16u
#define MAX_LAG        (4u * 2560u + 4096u)
#define NEED_LINE      420u   /* window + a normal pulse (field phase known) */
#define NEED_V_ACQ     2700u  /* + a broad pulse here or half a line later */

static uint32_t s_evals;

static inline uint8_t ph_at(const sfw_ring_t *r, uint64_t k)
{
    return r->phase[r->ring[(uint32_t)k & (r->ring_bytes - 1u)]];
}

static inline int wrap8(int d)
{
    d &= 255;
    return d >= 128 ? d - 256 : d;
}

/* Endpoint phase step over one 75 ns span, Phase8 bins. */
static inline int step3(const sfw_ring_t *r, uint64_t k)
{
    return wrap8((int)ph_at(r, k) - (int)ph_at(r, k - 3u));
}

/* Sync-level test on three consecutive spans (k-3 .. k+6, 225 ns). */
static SFW_HOT bool lowm(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t k)
{
    ++s_evals;
    int p0 = ph_at(r, k - 3u), p1 = ph_at(r, k), p2 = ph_at(r, k + 3u), p3 = ph_at(r, k + 6u);
    return wrap8(p1 - p0) + wrap8(p2 - p1) + wrap8(p3 - p2) <= 3 * f->thr;
}

/* Single-span test: what the TX itself decodes for one 75 ns span. */
static inline bool span_high(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t k)
{
    ++s_evals;
    return step3(r, k) > f->thr;
}

/* A kept pulse must decode clean span by span, not only on average: n
 * single spans across [a, a + len) must all be at sync level. */
static bool clean_pulse(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t a, uint32_t len,
                        unsigned n)
{
    uint32_t stride = (len - 12u) / n;
    for (unsigned i = 0; i < n; ++i)
        if (span_high(f, r, a + 9u + i * stride)) return false;
    return true;
}

/* ---- synthesis -------------------------------------------------------- */
static uint8_t s_cell[256];
static const uint8_t *s_cell_phase;
static int s_cell_mask = -1;

static int signed4(unsigned v) { return v < 8u ? (int)v : (int)v - 16; }

/* Strong cells (radius 4..6.5 fine-lane steps) per target phase; with the
 * acquisition mask only bytes whose bit 0 is the unflagged polarity. */
static void build_cells(const sfw_ring_t *r)
{
    int key = r->mask_bit0 ? (int)(r->clear_bit0 & 1u) : 2;
    if (s_cell_phase == r->phase && s_cell_mask == key) return;
    int best[256];
    for (int t = 0; t < 256; ++t) best[t] = 1 << 30;
    for (int raw = 0; raw < 256; ++raw) {
        if (r->mask_bit0 && (unsigned)(raw & 1) != (r->clear_bit0 & 1u)) continue;
        int i = signed4((unsigned)raw >> 4), q = signed4((unsigned)raw & 15u);
        int r2x4 = (2 * i + 1) * (2 * i + 1) + (2 * q + 1) * (2 * q + 1);
        if (r2x4 < 64 || r2x4 > 169) continue;
        for (int t = 0; t < 256; ++t) {
            int e = wrap8((int)r->phase[raw] - t);
            e = e < 0 ? -e : e;
            int score = e * 1024 + (r2x4 > 100 ? r2x4 - 100 : 100 - r2x4);
            if (score < best[t]) { best[t] = score; s_cell[t] = (uint8_t)raw; }
        }
    }
    s_cell_phase = r->phase;
    s_cell_mask = key;
}

/* Write [a, a + n1 + n2): n1 samples rotating step1, then n2 at step2 (Q8
 * Phase8 bins per sample), from the real phase before a. When the next real
 * sample is available its phase is met (minus next_step), the residual
 * spread over the run, so neither edge carries a glitch. */
static void synth(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t a, uint32_t n1,
                  int32_t step1, uint32_t n2, int32_t step2, int32_t next_step,
                  uint64_t floor, uint64_t avail_end)
{
    uint64_t b = a + n1 + n2;
    if (a < floor) {
        ++f->skipped_floor;
        if (b <= floor) return;
        uint64_t cut = floor - a;
        if (cut >= n1) { n2 -= (uint32_t)(cut - n1); n1 = 0; }
        else n1 -= (uint32_t)cut;
        a = floor;
    }
    uint32_t n = n1 + n2;
    if (!n) return;
    int64_t p0 = (int64_t)ph_at(r, a - 1u) * 256;
    int64_t rot = (int64_t)n1 * step1 + (int64_t)n2 * step2;
    int64_t resid = 0;
    if (b < avail_end) {
        int64_t target = (int64_t)ph_at(r, b) * 256 - next_step;
        resid = (target - (p0 + rot)) % 65536;
        if (resid >= 32768) resid -= 65536;
        if (resid < -32768) resid += 65536;
    }
    const uint32_t mask = r->ring_bytes - 1u;
    int64_t acc = p0;
    for (uint32_t k = 0; k < n; ++k) {
        acc += k < n1 ? step1 : step2;
        int64_t phase = acc + resid * (int64_t)(k + 1u) / (int64_t)n;
        r->ring[(uint32_t)(a + k) & mask] = s_cell[(uint32_t)((phase + 128) >> 8) & 255u];
    }
}

static inline int32_t sync_step(const sync_flywheel_t *f) { return (int32_t)f->sync_q4 * 16 / 3; }
static inline int32_t blank_step(const sync_flywheel_t *f) { return (int32_t)f->blank_q4 * 16 / 3; }

/* ---- geometry ---------------------------------------------------------- */
static inline bool pal(const sync_flywheel_t *f) { return f->nominal_q8 == (int32_t)SFW_PAL_LINE_Q8; }
static inline unsigned npre(const sync_flywheel_t *f) { return pal(f) ? 5u : 6u; }
static inline unsigned nbroad(const sync_flywheel_t *f) { return pal(f) ? 5u : 6u; }
static inline unsigned npost(const sync_flywheel_t *f) { return pal(f) ? 5u : 6u; }
static inline uint32_t eq_low(const sync_flywheel_t *f) { return pal(f) ? 94u : 92u; }
static inline uint64_t half_q8(const sync_flywheel_t *f) { return (uint64_t)f->period_q8 / 2u; }
static inline uint32_t field_lines(const sync_flywheel_t *f, bool half)
{
    /* 312.5 / 262.5 lines: from a half-line anchor the next one is on a line
     * start 313 (263) lines on; from a line-start anchor half a line after
     * 312 (262) lines. */
    return pal(f) ? (half ? 313u : 312u) : (half ? 263u : 262u);
}
static inline uint64_t rnd(uint64_t q8) { return (q8 + 128u) >> 8; }

void sfw_init(sync_flywheel_t *f)
{
    memset(f, 0, sizeof(*f));
    f->state = SFW_ACQUIRE;
}

bool sfw_locked(const sync_flywheel_t *f)
{
    return f->state == SFW_TRACK && f->clean_since_acq >= LOCK_CLEAN;
}

int sfw_standard(const sync_flywheel_t *f)
{
    if (f->state != SFW_TRACK) return 0;
    return pal(f) ? 1 : 2;
}

static void start_track(sync_flywheel_t *f, uint64_t start, uint32_t nominal)
{
    f->state = SFW_TRACK;
    f->nominal_q8 = f->period_q8 = (int32_t)nominal;
    f->next_q8 = (start << 8) + nominal;
    f->clean_since_acq = 0;
    f->lines_since_clean = 0;
    f->v_valid = false;
    f->v_end_q8 = 0;
    f->relock_have = false;
    f->rebuild_lines = 0;
    f->pristine_run = 0;
    f->acq_phase = 0;
    ++f->acquisitions;
}

/* Streaming acquisition, resumable under the budget: a step histogram over
 * ~2 lines for the threshold, then sync-width low runs one line apart. */
static void acquire(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end, uint32_t budget)
{
    if (avail_end < 8192u) return;
    uint64_t end = avail_end - 16u;
    if (f->scan_pos + r->ring_bytes / 2u < avail_end || f->scan_pos < 8u) {
        f->scan_pos = avail_end - 6000u;
        f->acq_run = 0;
        f->acq_have_last = false;
    }
    while (f->scan_pos < end && s_evals < budget) {
        uint64_t k = f->scan_pos;
        f->scan_pos += 3u;
        if (f->acq_phase == 0) {
            ++s_evals;
            ++f->acq_hist[step3(r, k) + 128];
            if (++f->acq_n < 6800u) continue;      /* ~8 lines: never one V interval */
            unsigned c = 0, p3 = 0, p50 = 0;
            bool got = false;
            for (unsigned v = 0; v < 256u; ++v) {
                c += f->acq_hist[v];
                if (!got && c * 100u >= 3u * f->acq_n) { p3 = v; got = true; }
                if (c * 2u >= f->acq_n) { p50 = v; break; }
            }
            memset(f->acq_hist, 0, sizeof(f->acq_hist));
            f->acq_n = 0;
            if (p50 < p3 + 10u) continue;               /* no modulation */
            f->thr = (int16_t)((int)p3 - 128 + (int)(p50 - p3) / 3);
            f->acq_phase = 1;
            f->acq_run = 0;
            f->acq_have_last = false;
            f->acq_runs = 0;
            continue;
        }
        if (++f->acq_n > 6800u) { f->acq_phase = 0; f->acq_n = 0; continue; }   /* 8 lines, no match */
        if (lowm(f, r, k)) { f->acq_run += 3u; continue; }
        uint32_t run = f->acq_run;
        f->acq_run = 0;
        if (run < 150u || run > 230u) continue;
        uint64_t start = k - run;
        if (f->acq_have_last) {
            uint64_t d = start - f->acq_last_start;
            if (d >= 2552u && d <= 2568u) { start_track(f, start, SFW_PAL_LINE_Q8); return; }
            if (d >= 2534u && d <= 2550u) { start_track(f, start, SFW_NTSC_LINE_Q8); return; }
        }
        f->acq_last_start = start;
        f->acq_have_last = true;
        if (++f->acq_runs > 24u) { f->acq_phase = 0; f->acq_n = 0; }   /* re-estimate */
    }
}

/* Low-run length from a leading edge (stride 3, one high sample tolerated). */
static uint32_t pulse_width(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t k,
                            uint32_t max, uint32_t *inside)
{
    uint32_t w = 0, highs = 0, total = 0;
    while (w < max && highs < 2u) {
        if (lowm(f, r, k + w)) highs = 0;
        else { ++highs; ++total; }
        w += 3u;
    }
    *inside = total - highs;
    return w - 3u * highs;
}

typedef enum { P_NONE, P_NORMAL, P_EQ, P_BROAD } pulse_t;

static pulse_t classify(uint32_t w, uint32_t inside)
{
    uint32_t n = w / 3u;
    if (w >= 150u && w <= 230u && inside <= n / 8u) return P_NORMAL;
    if (w >= 900u && inside <= n / 8u) return P_BROAD;
    if (w >= 70u && w <= 120u && inside <= 1u) return P_EQ;
    return P_NONE;
}

/* Leading edge (high -> low) nearest the prediction within +-win, refined to
 * one sample; returns the classified pulse. */
static pulse_t find_pulse(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t pred,
                          int win, uint64_t avail_end, int *found, uint32_t *width)
{
    bool prev = lowm(f, r, pred - (uint64_t)win - 3u);
    for (int e = -win; e <= win; e += 3) {
        uint64_t k = pred + (uint64_t)(int64_t)e;
        bool cur = lowm(f, r, k);
        if (cur && !prev && lowm(f, r, k + 3u)) {
            uint32_t inside;
            uint64_t room = avail_end > k + 12u ? avail_end - k - 12u : 0u;
            uint32_t w = pulse_width(f, r, k, room < 1300u ? (uint32_t)room : 1300u, &inside);
            pulse_t t = classify(w, inside);
            if (t != P_NONE) {
                int edge = e;
                if (lowm(f, r, k - 2u)) edge = e - 2;
                else if (lowm(f, r, k - 1u)) edge = e - 1;
                *found = edge;
                *width = w;
                return t;
            }
        }
        prev = cur;
    }
    return P_NONE;
}

static void learn_levels(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t start)
{
    int s = 0, b = 0;
    for (unsigned k = 0; k < 40u; ++k) s += step3(r, start + 30u + 3u * k);
    for (unsigned k = 0; k < 16u; ++k) b += step3(r, start - 60u + 3u * k);
    for (unsigned k = 0; k < 8u; ++k) b += step3(r, start + 192u + 3u * k);
    s_evals += 64u;
    int sq4 = s * 16 / 40, bq4 = b * 16 / 24;
    if (!f->levels && !f->sync_q4 && !f->blank_q4) { f->sync_q4 = (int16_t)sq4; f->blank_q4 = (int16_t)bq4; }
    else {
        f->sync_q4 = (int16_t)((7 * f->sync_q4 + sq4) / 8);
        f->blank_q4 = (int16_t)((7 * f->blank_q4 + bq4) / 8);
    }
    f->levels = f->blank_q4 - f->sync_q4 >= 12 * 16;
    if (f->levels) f->thr = (int16_t)((f->sync_q4 + f->blank_q4) / 32);
}

static void advance(sync_flywheel_t *f, uint64_t next_q8)
{
    f->next_q8 = next_q8;
    ++f->grid_line;
}

static inline uint64_t v_start_q8(const sync_flywheel_t *f)
{
    return f->v_next_q8 - (uint64_t)npre(f) * half_q8(f) - half_q8(f) / 2u;
}
static inline uint64_t v_stop_q8(const sync_flywheel_t *f)
{
    return f->v_next_q8 + (uint64_t)(nbroad(f) + npost(f)) * half_q8(f) - half_q8(f) / 4u;
}

static void field_done(sync_flywheel_t *f)
{
    f->v_end_q8 = v_stop_q8(f);
    f->v_a_line += field_lines(f, f->v_half);
    f->v_half = !f->v_half;
    f->v_armed = false;
    if (f->v_found) ++f->vsyncs;
    else ++f->v_coasted;
    f->v_found = false;
}

static bool broad_at(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t p, uint32_t h2)
{
    return clean_pulse(f, r, p, h2 - HSYNC, 16u) && !lowm(f, r, p + h2 - 110u);
}

static bool eq_at(const sync_flywheel_t *f, const sfw_ring_t *r, uint64_t p)
{
    return clean_pulse(f, r, p, eq_low(f), 6u) && !lowm(f, r, p + 130u) && !lowm(f, r, p + 400u);
}

/* One half-line slot of the vertical interval. Strict presence (every point
 * of the pulse low, the gap after it high): a noisy real pulse that fails is
 * simply rebuilt (harmless); a broken one that noise lets pass would stay
 * broken. A missing first broad pulse with real ones half a line away is a
 * wrong field parity: shift instead of adding a second V sync. */
static void v_slot(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end,
                   uint64_t floor, bool repair)
{
    int j = f->v_slot;
    uint64_t h2q = half_q8(f);
    uint64_t pos_q8 = f->v_next_q8 + (uint64_t)((int64_t)j * (int64_t)h2q);
    uint64_t p = rnd(pos_q8);
    uint32_t h2 = (uint32_t)rnd(h2q);
    bool broad = j >= 0 && j < (int)nbroad(f);
    bool present = broad ? broad_at(f, r, p, h2) : eq_at(f, r, p);
    /* Wrong parity means an equalizing slot here: its middle is high. A real
     * broad pulse with only a broken start stays put and is rebuilt. */
    if (j == 0 && !present && p + 2u * h2 + 16u <= avail_end &&
        !lowm(f, r, p + h2 / 4u) && !lowm(f, r, p + h2 / 2u) && !lowm(f, r, p + 3u * h2 / 4u)) {
        if (broad_at(f, r, p + h2, h2)) {
            f->v_next_q8 += h2q;
            if (f->v_half) ++f->v_a_line;
            f->v_half = !f->v_half;
            ++f->v_parity;
            return;
        }
        if (broad_at(f, r, p - h2, h2)) {
            f->v_next_q8 -= h2q;
            if (!f->v_half) --f->v_a_line;
            f->v_half = !f->v_half;
            ++f->v_parity;
            return;
        }
    }
    if (present && j == 0) f->v_found = true;
    if ((!present || f->rebuild_lines) && repair) {
        uint32_t low = broad ? h2 - HSYNC : eq_low(f);
        synth(f, r, p, low, sync_step(f), h2 - low, blank_step(f), blank_step(f), floor, avail_end);
        ++f->slots_repaired;
    }
    if (++f->v_slot >= (int16_t)(nbroad(f) + npost(f))) field_done(f);
}

unsigned sfw_run(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end,
                 uint64_t write_floor, bool allow_repair, uint32_t budget_evals)
{
    if (!f || !r || !r->ring || !r->phase || !r->ring_bytes ||
        (r->ring_bytes & (r->ring_bytes - 1u))) return 0;
    build_cells(r);
    s_evals = 0;
    f->evals = 0;
    if (f->state == SFW_ACQUIRE) {
        acquire(f, r, avail_end, budget_evals);
        f->evals = s_evals;
        if (f->state == SFW_ACQUIRE) return 0;
    }
    unsigned processed = 0;
    while (f->state == SFW_TRACK && s_evals < budget_evals) {
        bool locked = sfw_locked(f);
        bool repair = allow_repair && locked && f->levels;
        uint64_t h2q = half_q8(f);
        /* Arm the coming vertical interval 4 lines ahead, on the line grid. */
        if (f->v_valid && !f->v_armed) {
            int32_t ahead = (int32_t)(f->v_a_line - f->grid_line);
            if (ahead < 0) { field_done(f); continue; }        /* passed unseen */
            if (ahead <= 4) {
                f->v_next_q8 = f->next_q8 + (uint64_t)ahead * (uint64_t)f->period_q8 +
                               (f->v_half ? h2q : 0u);
                f->v_slot = (int16_t)-(int)npre(f);
                f->v_found = false;
                f->v_armed = true;
            }
        }
        if (f->v_armed) {
            uint64_t slot_q8 = f->v_next_q8 + (uint64_t)((int64_t)f->v_slot * (int64_t)h2q);
            if (slot_q8 <= f->next_q8 + h2q / 2u) {
                uint64_t p = rnd(slot_q8);
                if (p + 2u * rnd(h2q) + 16u > avail_end) break;
                if (avail_end - p > MAX_LAG) {                  /* too late to write */
                    ++f->skipped_lines;
                    if (++f->v_slot >= (int16_t)(nbroad(f) + npost(f))) field_done(f);
                    continue;
                }
                v_slot(f, r, avail_end, write_floor, repair);
                continue;
            }
        }
        uint64_t pred = rnd(f->next_q8);
        if (pred + (f->v_valid ? NEED_LINE : NEED_V_ACQ) > avail_end) break;
        if (pred + r->ring_bytes / 2u < avail_end) {
            f->state = SFW_ACQUIRE;          /* data gone: start over */
            f->scan_pos = 0;
            break;
        }
        uint64_t base = f->next_q8;
        /* Lines inside the vertical interval belong to the slots. */
        if ((f->v_armed && base >= v_start_q8(f) && base < v_stop_q8(f)) ||
            (f->v_valid && base < f->v_end_q8)) {
            {
                advance(f, base + (uint64_t)(int64_t)f->period_q8);
                ++f->lines;
                ++processed;
                continue;
            }
        }
        if (avail_end - pred > MAX_LAG) {
            advance(f, base + (uint64_t)(int64_t)f->period_q8);
            ++f->skipped_lines;
            if (f->lines_since_clean < 0xFFFFFFFFu) ++f->lines_since_clean;
            continue;
        }
        int win = WIN_UNLOCKED;
        if (locked) {
            win = WIN_LOCKED + (int)(f->lines_since_clean / 16u);
            if (win > WIN_UNLOCKED) win = WIN_UNLOCKED;
        }
        int found = 0;
        uint32_t width = 0;
        pulse_t type = P_NONE;
        /* Fast path: locked and the previous line was clean. */
        if (locked && f->lines_since_clean == 0u &&
            !lowm(f, r, pred - 15u) && lowm(f, r, pred + 3u) && lowm(f, r, pred + 60u) &&
            lowm(f, r, pred + 120u) && lowm(f, r, pred + 165u) && !lowm(f, r, pred + 205u)) {
            int e = -9;
            while (e < 6 && !lowm(f, r, pred + (uint64_t)(int64_t)e)) e += 3;
            if (e > -9 && lowm(f, r, pred + (uint64_t)(int64_t)(e - 2))) e -= 2;
            else if (e > -9 && lowm(f, r, pred + (uint64_t)(int64_t)(e - 1))) e -= 1;
            found = e;
            width = HSYNC;
            type = P_NORMAL;
            ++f->fast_lines;
        } else {
            type = find_pulse(f, r, pred, win, avail_end, &found, &width);
        }
        uint64_t start = pred + (uint64_t)(int64_t)found;
        bool normal = type == P_NORMAL, broad = type == P_BROAD;
        uint32_t line = f->grid_line;

        /* PLL: phase 1/4, frequency 1/64 (1/16, 1/256 while pulses are
         * noisy), period within 0.3 %. Only pulses
         * with both edges intact (width 188 +- 12) steer: a pulse whose start
         * the noise ate has a late leading edge. */
        bool steer = normal && width + 12u >= HSYNC && width <= HSYNC + 12u;
        bool clean = steer && (!locked || (found >= -CLEAN_ERR && found <= CLEAN_ERR));
        if (steer) {
            /* Noisy pulses (rebuild mode): 4x lower gains for a calm raster. */
            int32_t err = found * 256;
            int32_t pg = f->rebuild_lines ? 16 : 4, fg = f->rebuild_lines ? 256 : 64;
            f->period_q8 += err / fg;
            int32_t lim = f->nominal_q8 / 333;
            if (f->period_q8 > f->nominal_q8 + lim) f->period_q8 = f->nominal_q8 + lim;
            if (f->period_q8 < f->nominal_q8 - lim) f->period_q8 = f->nominal_q8 - lim;
            advance(f, base + (uint64_t)(int64_t)(f->period_q8 + err / pg));
        } else {
            advance(f, base + (uint64_t)(int64_t)f->period_q8);
        }

        /* First vertical sync: a broad pulse at this line start or half a
         * line later fixes the field phase on the line grid. */
        if (!f->v_valid && locked && f->levels) {
            int half = -1;
            uint32_t h2 = (uint32_t)rnd(h2q);
            if (broad) half = 0;
            else if (!normal && broad_at(f, r, pred + h2, h2)) half = 1;
            if (half >= 0) {
                /* Arm this very interval from its anchor: its remaining
                 * slots and lines are vertical-interval, not missing H. */
                f->v_valid = true;
                f->v_a_line = line;
                f->v_half = half == 1;
                f->v_next_q8 = base + (half ? h2q : 0u);
                f->v_slot = 0;
                f->v_found = false;
                f->v_armed = true;
            }
        }
        bool in_v = f->v_armed && base >= v_start_q8(f) && base < v_stop_q8(f);

        ++f->lines;
        ++processed;
        if (clean) {
            ++f->clean;
            if (f->clean_since_acq < 0xFFFFu) ++f->clean_since_acq;
            f->lines_since_clean = 0;
            f->relock_have = false;
            if ((f->clean & 15u) == 1u) learn_levels(f, r, start);
        } else {
            if (type == P_NONE) ++f->missed;
            if (f->lines_since_clean < 0xFFFFFFFFu) ++f->lines_since_clean;
        }

        /* Only missing or malformed pulses are rebuilt; a real pulse off the
         * prediction steers the PLL instead. Needs the field phase so the
         * vertical interval is never mistaken for missing H syncs. */
        bool keep = in_v || (normal && found >= -CLEAN_ERR && found <= CLEAN_ERR &&
                             clean_pulse(f, r, start, HSYNC, 12u));
        /* Only lines that must carry a normal H sync (field phase known,
         * outside the vertical interval) judge the signal. */
        if (in_v) {
            /* vertical interval: judged by its slots */
        } else if (keep) {
            if (f->pristine_run < 0xFFFFu) ++f->pristine_run;
            if (f->pristine_run >= 64u) f->rebuild_lines = 0;
        } else if (f->v_valid) {
            f->pristine_run = 0;
            f->rebuild_lines = 1;
        }
        if (keep && !in_v && f->rebuild_lines) { keep = false; ++f->rebuilt; }
        /* With the field phase known every line here must carry a normal H
         * sync (the vertical interval belongs to the slots), so a run that
         * noise made look like an equalizing or broad pulse is rebuilt too. */
        if (!keep && repair && f->v_valid) {
            /* With 1.2 us of the 1.65 us front porch at blanking, so noise
             * before the pulse cannot move its leading edge. */
            synth(f, r, rnd(base) - PORCH, PORCH, blank_step(f), HSYNC, sync_step(f), blank_step(f),
                  write_floor, avail_end);
            ++f->repaired;
        }

        /* Coasting: look for the real sync anywhere in the line (a VTX that
         * came back at another phase); two matching finds 8 lines apart
         * move the flywheel there in one step. */
        if (locked && f->lines_since_clean >= RELOCK_AFTER && (f->lines % RELOCK_EVERY) == 0u &&
            pred > (uint64_t)rnd((uint64_t)f->period_q8) + 8u) {
            /* The previous line is complete: scan it outside the window
             * around its own prediction (where our pulse may sit). */
            uint32_t period = (uint32_t)rnd((uint64_t)f->period_q8);
            uint64_t line0 = pred - period;
            bool prev = lowm(f, r, line0 + (uint64_t)win);
            for (uint32_t e = (uint32_t)win + 3u; e + (uint32_t)win < period; e += 3u) {
                uint64_t k = line0 + e;
                bool cur = lowm(f, r, k);
                if (cur && !prev) {
                    uint32_t inside, w = pulse_width(f, r, k, 260u, &inside);
                    if (classify(w, inside) == P_NORMAL && inside == 0u) {
                        int32_t off = (int32_t)e > (int32_t)period / 2 ? (int32_t)e - (int32_t)period : (int32_t)e;
                        int32_t d = off - f->relock_off;
                        if (f->relock_have && f->lines - f->relock_line == RELOCK_EVERY && d >= -12 && d <= 12) {
                            f->next_q8 += (uint64_t)((int64_t)off * 256);
                            f->period_q8 = f->nominal_q8;
                            f->v_valid = false;     /* field phase is unknown again */
                            f->v_armed = false;
                            f->v_end_q8 = 0;
                            f->lines_since_clean = 0;
                            f->relock_have = false;
                            ++f->relocks;
                        } else {
                            f->relock_off = off;
                            f->relock_line = f->lines;
                            f->relock_have = true;
                        }
                        break;
                    }
                }
                prev = cur;
            }
        }

        if (f->lines_since_clean > LOST_LINES) {
            f->state = SFW_ACQUIRE;
            f->scan_pos = 0;
        }
    }
    f->evals = s_evals;
    return processed;
}
