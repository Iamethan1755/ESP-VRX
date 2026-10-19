#include "rx_auto_lab.h"

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static int p_distance(int p)
{
    return iabs(p - 24);
}

static int iq_geometry_penalty(const rx_auto_observation_t *o)
{
    return o->iq_skew_permille > o->iq_cross_permille ?
           o->iq_skew_permille : o->iq_cross_permille;
}

rx_auto_class_t rx_auto_classify(const rx_auto_observation_t *o)
{
    if (!o) return RX_AUTO_REJECT;

    /* Transport corruption and significant Q4 rail clipping are never allowed
     * to win just because they produce a larger P/Q number. */
    if (o->transport_faults != 0 || o->clip_permille > 30)
        return RX_AUTO_REJECT;

    if (o->q_phase >= 65 &&
        o->origin_permille <= 250 &&
        o->p_median >= 14 && o->p_median <= 34 &&
        o->iq_skew_permille <= 300 &&
        o->iq_cross_permille <= 300)
        return RX_AUTO_SWEET;

    if (o->q_phase >= 45 &&
        o->origin_permille <= 500 &&
        o->p_median >= 8 && o->p_median <= 45 &&
        o->iq_skew_permille <= 500 &&
        o->iq_cross_permille <= 500)
        return RX_AUTO_USABLE;

    return RX_AUTO_POOR;
}

const char *rx_auto_class_name(rx_auto_class_t c)
{
    switch (c) {
    case RX_AUTO_SWEET: return "SWEET";
    case RX_AUTO_USABLE: return "USABLE";
    case RX_AUTO_POOR: return "POOR";
    default: return "REJECT";
    }
}

bool rx_auto_better(const rx_auto_observation_t *a,
                    const rx_auto_observation_t *b)
{
    if (!a) return false;
    if (!b) return true;

    rx_auto_class_t ca = rx_auto_classify(a);
    rx_auto_class_t cb = rx_auto_classify(b);
    if (ca != cb) return ca > cb;

    /* Lexicographic comparison. No weighted magic score and no monotonic P
     * reward: once candidates are in the same class, preserve headroom first,
     * then coherence/near-origin evidence, then Q4 placement/geometry. */
    if (a->clip_permille != b->clip_permille)
        return a->clip_permille < b->clip_permille;
    if (a->q_phase != b->q_phase)
        return a->q_phase > b->q_phase;
    if (a->origin_permille != b->origin_permille)
        return a->origin_permille < b->origin_permille;

    int ap = p_distance(a->p_median);
    int bp = p_distance(b->p_median);
    if (ap != bp) return ap < bp;

    int ag = iq_geometry_penalty(a);
    int bg = iq_geometry_penalty(b);
    if (ag != bg) return ag < bg;

    if (a->winding_permille != b->winding_permille)
        return a->winding_permille < b->winding_permille;
    return a->sync_quality > b->sync_quality;
}

bool rx_auto_reference_stable(const rx_auto_observation_t *before,
                              const rx_auto_observation_t *after)
{
    if (!before || !after) return false;
    if (before->transport_faults || after->transport_faults) return false;

    return iabs(before->q_phase - after->q_phase) <= 15 &&
           iabs(before->p_median - after->p_median) <= 10 &&
           iabs(before->origin_permille - after->origin_permille) <= 150 &&
           iabs(before->clip_permille - after->clip_permille) <= 80;
}

bool rx_auto_is_rf_limit(const rx_auto_observation_t *o)
{
    if (!o || o->transport_faults) return false;
    return o->clip_permille <= 8 &&
           o->p_median <= 4 &&
           o->q_phase < 15 &&
           o->origin_permille >= 800;
}

bool rx_auto_is_overload(const rx_auto_observation_t *o)
{
    if (!o) return false;
    return o->clip_permille >= 80 || o->p_median > 45;
}
