/* C5VRX-5 by Twotoz and the C5VRX contributors: Direct Gain V5 core.
 * Ported from legacy/c5vrx4/main/direct_gain_v3.c (PR #190) with the fixed-lane,
 * staged-recovery, boost-off configuration that ran on the board. */
#include "gain.h"
#include "direct_gain_v3_lut.h"
#include <string.h>

/* A Q4 cell represents the half-integer position n+0.5. The lookup is built
 * once; the observer only reads tables and counts a histogram. */
static uint8_t s_power[256];
static uint8_t s_flags[256];
static bool s_lut_ready;

/* Direct mode: act on the first window outside the healthy band and jump
 * straight to the predicted destination. The healthy band stays a strict
 * zero-write zone, so a steady carrier gets no gain writes. */
#define DG_STABLE_WINDOWS 1u
#define DG_HIGH_WINDOWS   1u
#define DG_WEAK_WINDOWS   1u
/* Anti-hunt: two direction reversals within DG_REVERSAL_US arm a damped
 * period in which an out-of-band decision needs DG_DAMPED_WINDOWS windows.
 * Saturation is never damped. */
#define DG_REVERSAL_US     20000u
#define DG_REVERSALS_ARM   2u
#define DG_DAMP_US         200000u
#define DG_DAMPED_WINDOWS  8u
/* A moderate excursion must persist 20 ms (a third of a field) before a
 * write: ordinary FM envelope ripple crossed a band edge 30-100 times a
 * second, and every write is a short PHY transient (board, PR #177). */
#define DG_MODERATE_US     20000u
/* Wi-Fi bursts clip every sample for 0.1-0.5 ms (zerowidth/C5VRX PR #3);
 * saturation counts as overload only after it lasted this long. */
#define DG_BURST_US        1000u
/* Fade hold. A moving drone in an open field passes ground-reflection and
 * multipath nulls of a few milliseconds, after which the carrier is back at
 * its old level. Jumping to the table maximum on the first empty window
 * (+~22 dB from G60) made the returning carrier fold in the ultrafine
 * window: the 1 ms burst gate plus a staged recovery turned every null into
 * ~2 ms of extra loss and 4 writes, and a partial null into ~10 writes
 * (test/test_gain_fade.c, host model). Within DG_FADE_HOLD_US of the last
 * healthy window an empty or weak window does not raise the gain. Motion
 * nulls last ~lambda/2 / speed (2.6 cm: ~3 ms at 10 m/s); a longer hold made
 * a carrier that stays weaker (obstacle) wait: 81 ms instead of 1 ms for a
 * -12 dB step with 100 ms. With 10 ms the cost of a sustained drop is
 * bounded by the hold. A carrier that is really gone still reaches the
 * table maximum; distance fades are slower and tracked as before. */
#define DG_FADE_HOLD_US    10000u

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int abs_i(int v) { return v < 0 ? -v : v; }

static void prepare_lut(void)
{
    if (s_lut_ready) return;
    for (unsigned raw = 0; raw < 256; ++raw) {
        int q = (int8_t)((raw & 15u) << 4) >> 4;
        int i = (int8_t)(raw & 240u) >> 4;
        int ci = 2 * i + 1, cq = 2 * q + 1;
        int power = (ci * ci + cq * cq + 2) / 4;
        s_power[raw] = (uint8_t)power;
        s_flags[raw] = (power <= 4 ? 1u : 0u) |
                       ((i == -8 || i == 7 || q == -8 || q == 7) ? 2u : 0u);
    }
    s_lut_ready = true;
}

dg_observation_t dg_measure(const uint8_t *samples, size_t bytes,
                            const uint8_t phase8_lut[256], uint64_t observed_us)
{
    dg_observation_t r = {.observed_us = observed_us};
    if (!samples || !bytes || !phase8_lut) return r;
    prepare_lut();
    uint16_t hist[114] = {0};
    unsigned origin = 0, clip = 0, coherent = 0, repeats = 0;
    uint8_t prior_phase = 0, prior_raw = 0;
    int i_min = 7, i_max = -8, q_min = 7, q_max = -8;
    int64_t sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxp = 0, syp = 0;
    uint64_t spp = 0;
    for (size_t n = 0; n < bytes; ++n) {
        uint8_t raw = samples[n], power = s_power[raw];
        const int x = 2 * ((int8_t)(raw & 240u) >> 4) + 1;
        const int y = 2 * ((int8_t)((raw & 15u) << 4) >> 4) + 1;
        const int p = x * x + y * y;
        sx += x; sy += y;
        sxx += x * x; syy += y * y; sxy += x * y;
        sxp += x * p; syp += y * p;
        spp += (unsigned)(p * p);
        ++hist[power];
        origin += s_flags[raw] & 1u;
        clip += (s_flags[raw] >> 1) & 1u;
        uint8_t phase = phase8_lut[raw];
        if (n && power >= 8) {
            int delta = (((int)phase - (int)prior_phase + 128) & 255) - 128;
            if (abs_i(delta) <= 32) ++coherent;
        }
        repeats += n && raw == prior_raw;
        if (raw != prior_raw || !n) {
            int si = (int8_t)(raw & 240u) >> 4, sq = (int8_t)((raw & 15u) << 4) >> 4;
            i_min = si < i_min ? si : i_min; i_max = si > i_max ? si : i_max;
            q_min = sq < q_min ? sq : q_min; q_max = sq > q_max ? sq : q_max;
        }
        prior_phase = phase;
        prior_raw = raw;
    }
    unsigned cumulative = 0;
    bool p50 = false, p90 = false;
    unsigned r50 = (unsigned)((bytes + 1u) / 2u);
    unsigned r90 = (unsigned)((bytes * 90u + 99u) / 100u);
    unsigned r95 = (unsigned)((bytes * 95u + 99u) / 100u);
    for (unsigned p = 0; p < 114u; ++p) {
        cumulative += hist[p];
        if (!p50 && cumulative >= r50) { r.p50 = (uint8_t)p; p50 = true; }
        if (!p90 && cumulative >= r90) { r.p90 = (uint8_t)p; p90 = true; }
        if (cumulative >= r95) { r.p95 = (uint8_t)p; break; }
    }
    r.origin_pm = (uint16_t)(origin * 1000u / bytes);
    r.clip_pm = (uint16_t)(clip * 1000u / bytes);
    r.coherence = bytes > 1u ? (uint8_t)(coherent * 100u / (bytes - 1u)) : 0;
    r.repeat_pm = bytes > 1u ? (uint16_t)(repeats * 1000u / (bytes - 1u)) : 0;
    /* Envelope C/N: integer raw moments in the existing sample loop, then
     * translate the moments to the window's DC centre once per probe.
     * No per-sample floating point or extra passes in the fast observer. */
    const double inv = 1.0 / (double)bytes;
    const double mx = (double)sx * inv, my = (double)sy * inv;
    const double dc2 = mx * mx + my * my;
    const double mp_raw = (double)(sxx + syy) * inv;
    const double mp = mp_raw - dc2;
    const double mp2 = (double)spp * inv - 4.0 * (mx * (double)sxp + my * (double)syp) * inv
                    + 4.0 * (mx * mx * (double)sxx + 2.0 * mx * my * (double)sxy + my * my * (double)syy) * inv
                    + 2.0 * dc2 * mp_raw - 3.0 * dc2 * dc2;
    const double var = mp2 - mp * mp;
    const double ratio = mp <= 0.0 ? 0.0 : var > 0.0 ? 100.0 * mp * mp / var : 65535.0;
    r.env_ratio_x100 = (uint16_t)(ratio > 65535.0 ? 65535.0 : ratio + .5);
    int span = i_max - i_min > q_max - q_min ? i_max - i_min : q_max - q_min;
    r.span = (uint8_t)(span < 0 ? 0 : span);
    return r;
}

static dg_transition_t transition_kind(const direct_gain_t *g, uint8_t from, uint8_t to)
{
    const arc_gain_tuple_t *a = &g->tuple[from], *b = &g->tuple[to];
    if (a->rf_stage != b->rf_stage) return DG_RF;
    if (a->bb_code != b->bb_code) return DG_BB;
    return DG_FINE;
}

static bool carrier(const dg_observation_t *o)
{
    return o->coherence >= 55 && o->p50 >= 5 && o->origin_pm < 650;
}

/* Healthy band P50 13..32 (r ~3.5..5.6 cells), the zero-write zone. */
#define BAND_LO          13
#define BAND_HI          32
#define BAND_P95         65
#define BAND_CLIP_PM     20
#define WEAK_TARGET      17
#define HIGH_TARGET      27
#define HIGH_P90         53
#define HIGH_P95         72
#define SCHMITT_HI       30
#define SCHMITT_P90      47
#define SCHMITT_P95      65
#define SCHMITT_LO       14

static bool healthy(const dg_observation_t *o)
{
    return carrier(o) && o->p50 >= BAND_LO && o->p50 <= BAND_HI &&
           o->p95 <= BAND_P95 && o->clip_pm < BAND_CLIP_PM && o->origin_pm <= 250;
}

static bool valid_learning(const dg_observation_t *o)
{
    return carrier(o) && o->p50 >= 7 && o->p50 <= 45 &&
           o->p95 < 80 && o->clip_pm < 20 && o->origin_pm < 300;
}

static int power_db_q8(unsigned power)
{
    if (power < 1u) power = 1u;
    if (power > 113u) power = 113u;
    return s_dg3_power_db_q8[power];
}

/* Q10 power ratio to Q8 dB with an integer log2 iteration. */
static int ratio_db_q8(int ratio_q10)
{
    if (ratio_q10 < 1) ratio_q10 = 1;
    int64_t x = ((int64_t)ratio_q10 << 16) / 1024;
    int whole = 0;
    while (x < 65536) { x <<= 1; --whole; }
    while (x >= 131072) { x >>= 1; ++whole; }
    int fraction = 0;
    for (int bit = 7; bit >= 0; --bit) {
        x = (x * x) >> 16;
        if (x >= 131072) { x >>= 1; fraction |= 1 << bit; }
    }
    return ((whole * 256 + fraction) * 771 + 128) / 256;
}

#define DG_MAGIC 0x44473521u
static bool same_table(const arc_gain_table_t *a, const arc_gain_table_t *b)
{
    if (!a || !b || a->max_index != b->max_index) return false;
    for (unsigned k = 0; k < ARC_RX_STAGE_COUNT; ++k)
        if (a->spans[k] != b->spans[k]) return false;
    return true;
}

void dg_reset(direct_gain_t *g, const arc_gain_table_t *table, uint8_t current_gain)
{
    if (!g) return;
    /* Keep the measured map across tracking resets on an unchanged table:
     * dropping it made V5 re-explore every step (a burst of corrective
     * writes, each a little grain; operator, 2026-10-06). */
    dg_map_blob_t kept;
    bool keep = g->magic == DG_MAGIC && g->learned && table && same_table(&g->table, table) &&
                dg_export_map(g, &kept) > 0u;
    memset(g, 0, sizeof(*g));
    prepare_lut();
    if (table && table->max_index >= 20u && table->max_index <= ARC_VENDOR_GAIN_MAX) g->table = *table;
    else arc_gain_table_from_bytes(&g->table, NULL, ARC_VENDOR_GAIN_MAX);
    for (unsigned k = 0; k <= g->table.max_index; ++k)
        (void)arc_gain_tuple_decode(&g->table, (uint8_t)k, &g->tuple[k]);
    g->current_gain = (uint8_t)clamp_i(current_gain, 20, g->table.max_index);
    g->target_gain = g->current_gain;
    g->relative_power_q10[g->current_gain] = 1024u;
    g->confidence[g->current_gain] = 1u;
    g->uncertainty_pm[g->current_gain] = 50u;
    g->state = DG_ACQUIRE;
    g->magic = DG_MAGIC;
    if (keep) (void)dg_import_map(g, &kept);
}

unsigned dg_export_map(const direct_gain_t *g, dg_map_blob_t *blob)
{
    if (!g || !blob) return 0u;
    memset(blob, 0, sizeof(*blob));
    blob->version = DG_MAP_VERSION;
    blob->max_index = g->table.max_index;
    blob->band5 = g->table.band5;
    unsigned confident = 0;
    for (unsigned k = 0; k <= g->table.max_index && k < DG_STATES; ++k) {
        if (!g->confidence[k] || !g->relative_power_q10[k]) continue;
        blob->confidence[k] = g->confidence[k];
        blob->power_q10[k] = g->relative_power_q10[k];
        blob->uncertainty_pm[k] = g->uncertainty_pm[k];
        ++confident;
    }
    return confident > 1u ? confident : 0u;   /* a lone anchor says nothing */
}

bool dg_import_map(direct_gain_t *g, const dg_map_blob_t *blob)
{
    if (!g || !blob || blob->version != DG_MAP_VERSION ||
        blob->max_index != g->table.max_index || blob->band5 != g->table.band5) return false;
    const uint8_t anchor = g->current_gain;
    if (!blob->confidence[anchor] || !blob->power_q10[anchor]) return false;
    for (unsigned k = 0; k <= g->table.max_index && k < DG_STATES; ++k) {
        if (!blob->confidence[k] || !blob->power_q10[k]) continue;
        g->relative_power_q10[k] = blob->power_q10[k];
        g->uncertainty_pm[k] = blob->uncertainty_pm[k];
        g->confidence[k] = blob->confidence[k];
    }
    g->learned = 1u;
    return true;
}

bool dg_import_preset(direct_gain_t *g, const uint16_t *power_q10, unsigned count, uint8_t max_index, uint8_t band5)
{
    if (!g || !power_q10 || max_index != g->table.max_index || band5 != g->table.band5) return false;
    const uint8_t anchor = g->current_gain;
    if (anchor >= count || !power_q10[anchor]) return false;
    /* Rescale so the anchor keeps its own value: predict() only uses ratios. */
    const uint32_t base = g->relative_power_q10[anchor] ? g->relative_power_q10[anchor] : 1024u;
    unsigned used = 0;
    for (unsigned k = 0; k <= g->table.max_index && k < count; ++k) {
        if (!power_q10[k] || g->confidence[k] > 1u) continue;   /* own data wins */
        uint32_t v = (uint32_t)power_q10[k] * base / power_q10[anchor];
        g->relative_power_q10[k] = (uint16_t)(v < 1u ? 1u : v > 65535u ? 65535u : v);
        g->uncertainty_pm[k] = 600u;
        g->confidence[k] = 1u;
        ++used;
    }
    return used > 1u;
}

/* Measured responses predict any tuple; without them only a fine step in
 * the same BB bank gets a low-confidence local prior. */
static bool predict(const direct_gain_t *g, uint8_t candidate, int *ratio_q10, int *uncertainty_pm)
{
    uint8_t current = g->current_gain;
    if (g->confidence[current] && g->confidence[candidate]) {
        int base = g->relative_power_q10[current];
        if (!base) return false;
        *ratio_q10 = clamp_i((int)g->relative_power_q10[candidate] * 1024 / base, 1, 8192);
        *uncertainty_pm = clamp_i(g->uncertainty_pm[candidate] + g->uncertainty_pm[current], 80, 900);
        return true;
    }
    if (transition_kind(g, current, candidate) != DG_FINE) return false;
    int steps = (int)g->tuple[current].fine_code - (int)g->tuple[candidate].fine_code;
    if (abs_i(steps) > 2) return false;
    int ratio = 1024;
    while (steps > 0) { ratio = ratio * 121 / 100; --steps; }
    while (steps < 0) { ratio = ratio * 100 / 121; ++steps; }
    *ratio_q10 = ratio;
    *uncertainty_pm = 300;
    return true;
}

static uint8_t adjacent_physical(const direct_gain_t *g, bool up)
{
    const arc_gain_tuple_t *current = &g->tuple[g->current_gain];
    for (int distance = 1; distance <= 2; ++distance) {
        int wanted_fine = (int)current->fine_code + (up ? -distance : distance);
        for (unsigned k = 20u; k <= g->table.max_index; ++k) {
            const arc_gain_tuple_t *t = &g->tuple[k];
            if (g->confidence[k] && g->confidence[g->current_gain]) {
                bool measured_up = g->relative_power_q10[k] > g->relative_power_q10[g->current_gain];
                if (measured_up != up) continue;
            }
            if (t->rf_stage == current->rf_stage && t->bb_code == current->bb_code &&
                t->fine_code == wanted_fine) return (uint8_t)k;
        }
    }
    /* Fine exhausted: the adjacent BB bank at its least aggressive state. */
    int wanted_bb = up ? (int)current->bb_code * 2 + 1 : ((int)current->bb_code - 1) / 2;
    for (int distance = 0; distance < 6; ++distance) {
        int boundary_fine = up ? 5 - distance : distance;
        for (unsigned k = 20u; k <= g->table.max_index; ++k) {
            const arc_gain_tuple_t *t = &g->tuple[k];
            if (t->rf_stage == current->rf_stage && t->bb_code == wanted_bb &&
                t->fine_code == boundary_fine) return (uint8_t)k;
        }
    }
    /* Last resort: the adjacent RF stage's nearest entry in vendor order
     * (the exact 5 GHz table starts stages at counters 12..15). */
    int wanted_rf = (int)current->rf_stage + (up ? 1 : -1);
    if (up) {
        for (unsigned k = 20u; k <= g->table.max_index; ++k)
            if ((int)g->tuple[k].rf_stage == wanted_rf) return (uint8_t)k;
    } else {
        for (int k = (int)g->table.max_index; k >= 20; --k)
            if ((int)g->tuple[k].rf_stage == wanted_rf) return (uint8_t)k;
    }
    return g->current_gain;
}

/* Overload: remove late (BB) gain first; the RF stage sets the noise
 * figure. Still saturated a window later, the next drop takes RF. */
static uint8_t emergency_drop(const direct_gain_t *g)
{
    const arc_gain_tuple_t *current = &g->tuple[g->current_gain];
    if (current->bb_code > 1u) {
        unsigned lower_bb = (current->bb_code - 1u) / 2u;
        for (unsigned k = 20u; k < g->current_gain; ++k) {
            const arc_gain_tuple_t *t = &g->tuple[k];
            if (t->rf_stage == current->rf_stage && t->bb_code == lower_bb) return (uint8_t)k;
        }
    }
    if (current->rf_stage > 0u) {
        for (unsigned k = 20u; k < g->current_gain; ++k)
            if (g->tuple[k].rf_stage + 1u == current->rf_stage) return (uint8_t)k;
    }
    return adjacent_physical(g, false);
}

static uint8_t select_destination(const direct_gain_t *g, const dg_observation_t *o,
                                  bool up, int32_t desired_q8)
{
    uint8_t best = g->current_gain;
    int best_class = 4, best_error = 99999, best_uncertainty = 999, best_artifact = 999;
    for (unsigned k = 20u; k <= g->table.max_index; ++k) {
        if (k == g->current_gain) continue;
        int ratio, uncertainty;
        if (!predict(g, (uint8_t)k, &ratio, &uncertainty)) continue;
        int p50 = (int)o->p50 * ratio / 1024;
        int p95_worst = (int)o->p95 * ratio * (1000 + uncertainty) / 1024000;
        if (p50 < BAND_LO || p50 > BAND_HI || p95_worst > BAND_P95) continue;
        if (up && ratio <= 1024) continue;
        if (!up && ratio >= 1024) continue;
        int kind = transition_kind(g, g->current_gain, (uint8_t)k);
        int delta_q8 = ratio_db_q8(ratio);
        int hysteresis_q8 = kind == DG_FINE ? 90 : kind == DG_BB ? 192 : 384;
        if ((up && desired_q8 < delta_q8 + hysteresis_q8) ||
            (!up && desired_q8 > delta_q8 - hysteresis_q8)) continue;
        int error = abs_i((int)desired_q8 - delta_q8);
        int artifact = g->artifact_score[k];
        if (kind < best_class ||
            (kind == best_class && error < best_error) ||
            (kind == best_class && error == best_error && uncertainty < best_uncertainty) ||
            (kind == best_class && error == best_error && uncertainty == best_uncertainty &&
             artifact < best_artifact)) {
            best = (uint8_t)k;
            best_class = kind;
            best_error = error;
            best_uncertainty = uncertainty;
            best_artifact = artifact;
        }
    }
    return best == g->current_gain ? adjacent_physical(g, up) : best;
}

static void learn_transition(direct_gain_t *g, const dg_observation_t *after)
{
    if (!g->learn_enabled) { ++g->learn_refused; return; }
    if (!valid_learning(&g->before) || !valid_learning(after) ||
        abs_i((int)g->before.coherence - (int)after->coherence) > 15) return;
    const dg_observation_t *prior = &g->before_previous;
    if (!prior->observed_us || prior->observed_us >= g->before.observed_us ||
        g->before.observed_us - prior->observed_us > 10000u ||
        abs_i((int)prior->p50 - (int)g->before.p50) > clamp_i(g->before.p50 / 4, 2, 10) ||
        abs_i((int)prior->coherence - (int)g->before.coherence) > 12)
        return;
    uint8_t a = g->prior_gain, b = g->current_gain;
    uint32_t base = g->relative_power_q10[a];
    if (!base) return;
    /* A "fine" step the vendor table makes a BB jump is learned anyway, with
     * high uncertainty: rejecting it kept that index mis-predicted. */
    bool mismatch = false;
    if (g->transition == DG_FINE) {
        int steps = abs_i((int)g->tuple[a].fine_code - (int)g->tuple[b].fine_code);
        int high = 1024, low = 1024;
        while (steps-- > 0) { high = high * 18 / 10; low = low * 10 / 18; }
        int observed = (int)after->p50 * 1024 / g->before.p50;
        mismatch = observed < low || observed > high;
    }
    uint32_t estimate = (uint32_t)clamp_i((int)(base * after->p50 / g->before.p50), 1, 65535);
    if (mismatch) ++g->model_mismatches;
    if (!g->confidence[b]) {
        g->relative_power_q10[b] = (uint16_t)estimate;
        g->uncertainty_pm[b] = mismatch ? 600u : 350u;
    } else {
        uint32_t old = g->relative_power_q10[b];
        uint32_t residual_pm = old ? (uint32_t)(abs_i((int)old - (int)estimate) * 1000) / old : 1000u;
        g->uncertainty_pm[b] = (uint16_t)clamp_i((3 * (int)g->uncertainty_pm[b] + (int)residual_pm) / 4, 60, 900);
        if (residual_pm > 450u) {
            if (g->confidence[b]) --g->confidence[b];
            return;
        }
        g->relative_power_q10[b] = (uint16_t)((3u * old + estimate) / 4u);
    }
    if (g->confidence[b] < 15u) ++g->confidence[b];
    ++g->learned;
}

static void note_direction(direct_gain_t *g, int8_t dir, uint64_t now)
{
    if (g->last_write_dir && dir != g->last_write_dir &&
        now >= g->dir_write_us && now - g->dir_write_us < DG_REVERSAL_US) {
        if (++g->reversals >= DG_REVERSALS_ARM) {
            g->damp_until_us = now + DG_DAMP_US;
            g->reversals = 0;
            ++g->damp_events;
        }
    } else if (now < g->dir_write_us || now - g->dir_write_us >= DG_REVERSAL_US) {
        g->reversals = 0;
    }
    g->last_write_dir = dir;
    g->dir_write_us = now;
}

static uint8_t start_write(direct_gain_t *g, const dg_observation_t *o,
                           const dg_observation_t *prior, uint8_t target)
{
    if (target == g->current_gain) return target;
    note_direction(g, target > g->current_gain ? 1 : -1, o->observed_us);
    int ratio_q10 = 0, uncertainty_pm = 0;
    if (predict(g, target, &ratio_q10, &uncertainty_pm)) g->virtual_gain_q8 -= ratio_db_q8(ratio_q10);
    else g->virtual_gain_q8 = 0;
    g->virtual_gain_q8 = clamp_i(g->virtual_gain_q8, -12 * 256, 12 * 256);
    g->before = *o;
    g->before_previous = *prior;
    g->prior_gain = g->current_gain;
    g->target_gain = target;
    g->transition = transition_kind(g, g->current_gain, target);
    g->current_gain = target;
    g->state = DG_SETTLE;
    g->stable_windows = 0;
    g->write_us = o->observed_us;
    ++g->writes;
    return target;
}

static uint64_t settle_guard_us(const direct_gain_t *g)
{
    return g->settle_us[g->transition] > 400u ? (uint64_t)g->settle_us[g->transition] * 3u / 4u : 300u;
}

uint8_t dg_tick(direct_gain_t *g, const dg_observation_t *o)
{
    if (!g || !o) return 0u;
    dg_observation_t prior = g->last_tracking;
    g->last_tracking = *o;
    bool no_carrier = o->p50 <= 4 && o->origin_pm >= 650 && o->coherence < 20;
    bool saturated = o->clip_pm >= 100 || o->p95 >= 95;
    if (!saturated) g->saturated_run = false;
    else if (!g->saturated_run) { g->saturated_run = true; g->saturated_since_us = o->observed_us; }
    if (saturated && o->observed_us - g->saturated_since_us < DG_BURST_US) {
        /* Possibly an interference burst: not an overload, not a prior. */
        ++g->bursts_ignored;
        g->last_tracking = prior;
        return g->current_gain;
    }
    bool at_max = g->current_gain == g->table.max_index;
    if (no_carrier) {
        /* Near-origin IQ during a settling write is not carrier loss yet. */
        if (g->state == DG_SETTLE &&
            (o->observed_us <= g->write_us || o->observed_us - g->write_us < settle_guard_us(g)))
            return g->current_gain;
        /* Fade hold: an empty window right after a healthy carrier is a
         * null, not a lost VTX; keep the gain the carrier will need again. */
        if (!g->overload_recovery && !at_max && g->last_healthy_us &&
            o->observed_us - g->last_healthy_us < DG_FADE_HOLD_US) {
            ++g->fade_holds;
            return g->current_gain;
        }
        /* After an overload a low radius is no proof the carrier vanished:
         * walk back through physical tuples instead of jumping to max. */
        if (g->overload_recovery && !at_max)
            return start_write(g, o, &prior, adjacent_physical(g, true));
        /* No carrier: listen at the table maximum, where a weak carrier
         * becomes visible; a strong one takes the saturation path next. */
        g->state = DG_ACQUIRE;
        g->stable_windows = 0;
        g->high_windows = g->weak_windows = 0;
        g->virtual_gain_q8 = 0;
        g->last_direction = 0;
        return start_write(g, o, &prior, g->table.max_index);
    }
    /* Saturation does not wait for a settling write once the window can no
     * longer hold pre-write samples; that write's learning is dropped. */
    if (saturated && g->state == DG_SETTLE &&
        (g->current_gain > g->prior_gain ||
         (o->observed_us >= g->write_us && o->observed_us - g->write_us >= 300u))) {
        g->before.p50 = 0u;
        g->state = DG_VERIFY;
    }
    if (g->state == DG_SETTLE) {
        if (o->observed_us <= g->write_us || o->observed_us - g->write_us < settle_guard_us(g))
            return g->current_gain;
        if (g->stable_windows &&
            abs_i((int)o->p50 - (int)g->previous.p50) <= 2 &&
            abs_i((int)o->p95 - (int)g->previous.p95) <= 4 &&
            abs_i((int)o->origin_pm - (int)g->previous.origin_pm) <= 60 &&
            abs_i((int)o->coherence - (int)g->previous.coherence) <= 12) {
            if (g->stable_windows < DG_STABLE_WINDOWS) ++g->stable_windows;
        } else g->stable_windows = 1u;
        g->previous = *o;
        if (g->stable_windows < DG_STABLE_WINDOWS) return g->current_gain;
        uint64_t elapsed = o->observed_us - g->write_us;
        if (elapsed < 65535u) {
            uint16_t *settle = &g->settle_us[g->transition];
            *settle = *settle ? (uint16_t)((3u * *settle + elapsed) / 4u) : (uint16_t)elapsed;
        }
        int quality_drop = (int)g->before.coherence - (int)o->coherence;
        int clipping_rise = (int)o->clip_pm - (int)g->before.clip_pm;
        int artifact = clamp_i((quality_drop > 0 ? quality_drop * 4 : 0) +
                               (clipping_rise > 0 ? clipping_rise / 4 : 0), 0, 1000);
        uint16_t *score = &g->artifact_score[g->current_gain];
        *score = *score ? (uint16_t)((3u * *score + artifact) / 4u) : (uint16_t)artifact;
        learn_transition(g, o);
        g->state = DG_VERIFY;
        ++g->verified;
    }
    if (saturated) {
        ++g->overloads;
        g->high_windows = g->weak_windows = 0;
        g->virtual_gain_q8 = 0;
        g->overload_recovery = true;
        return start_write(g, o, &prior, emergency_drop(g));
    }
    if (healthy(o)) {
        g->last_healthy_us = o->observed_us;
        g->overload_recovery = false;
        g->state = DG_HOLD;
        g->corrections = 0;
        g->high_windows = g->weak_windows = 0;
        g->virtual_gain_q8 = 0;
        g->last_direction = 0;
        ++g->holds;
        return g->current_gain;
    }
    /* Act as soon as the envelope leaves the band, before it gets grainy. A
     * starved envelope climbs whether or not the carrier is recognised yet. */
    bool high = o->p50 > BAND_HI || o->p90 >= HIGH_P90 || o->p95 > HIGH_P95 || o->clip_pm >= BAND_CLIP_PM;
    bool weak = o->p50 < BAND_LO;
    /* Schmitt bands keep the direction through small fluctuations. */
    if (g->last_direction == 2 &&
        (o->p50 > SCHMITT_HI || o->p90 > SCHMITT_P90 || o->p95 > SCHMITT_P95)) high = true;
    if (g->last_direction == 1 && o->p50 < SCHMITT_LO) weak = true;
    if (high) {
        g->weak_windows = 0;
        g->last_direction = 2;
        if (!g->high_windows) g->excursion_since_us = o->observed_us;
        if (g->high_windows < 255u) ++g->high_windows;
    } else if (weak) {
        g->high_windows = 0;
        g->last_direction = 1;
        if (!g->weak_windows) g->excursion_since_us = o->observed_us;
        if (g->weak_windows < 255u) ++g->weak_windows;
    } else {
        g->high_windows = g->weak_windows = 0;
        g->last_direction = 0;
        return g->current_gain;
    }
    /* Weak at the table maximum: there is no higher state. Receiver noise
     * reads here (P50 ~9 between "carrier" and "no carrier"); every write
     * found by the search was a sideways step and V5 hunted ~20 times a
     * second at G83 with every VTX off (board, 2026-10-11). Hold. */
    if (weak && at_max) {
        g->state = DG_HOLD;
        return g->current_gain;
    }
    /* Severe excursions act on one window, moderate ones on two windows
     * and 20 ms; damping after reversals needs eight. */
    bool damped = o->observed_us < g->damp_until_us;
    /* Weak right after a healthy carrier is treated as moderate (it must
     * last 20 ms): a partial null would otherwise raise the gain on one
     * window and fold the returning carrier. Too strong always acts at once. */
    const bool fading = g->last_healthy_us && o->observed_us - g->last_healthy_us < DG_FADE_HOLD_US;
    bool severe = high ? ((int)o->clip_pm >= BAND_CLIP_PM || (int)o->p95 > HIGH_P95 ||
                          (int)o->p50 > BAND_HI + 8)
                       : ((int)o->p50 + 4 < BAND_LO && !fading);
    unsigned need_high = damped ? DG_DAMPED_WINDOWS : severe ? DG_HIGH_WINDOWS : 2u;
    unsigned need_weak = damped ? DG_DAMPED_WINDOWS : severe ? DG_WEAK_WINDOWS : 2u;
    if ((high && g->high_windows < need_high) || (weak && g->weak_windows < need_weak))
        return g->current_gain;
    if (!severe && o->observed_us - g->excursion_since_us < DG_MODERATE_US) return g->current_gain;
    int target_power = weak ? WEAK_TARGET : HIGH_TARGET;
    /* Direct: request the full relative correction in one step. */
    g->virtual_gain_q8 = clamp_i(power_db_q8((unsigned)target_power) - power_db_q8(o->p50),
                                 -12 * 256, 12 * 256);
    uint8_t target = select_destination(g, o, weak, g->virtual_gain_q8);
    if (target == g->current_gain) return target;
    if (g->state == DG_VERIFY) ++g->corrections;
    g->high_windows = g->weak_windows = 0;
    if (severe) ++g->writes_severe;
    else ++g->writes_moderate;
    return start_write(g, o, &prior, target);
}

void dg_sync_applied(direct_gain_t *g, uint8_t gain, uint64_t write_us)
{
    if (!g) return;
    if (gain != g->current_gain) {
        g->current_gain = gain;
        g->target_gain = gain;
        g->stable_windows = 0;
    }
    g->write_us = write_us;
    g->state = DG_SETTLE;
}
