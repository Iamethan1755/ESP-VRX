/* C5VRX by Twotoz and contributors: native AGC witness analysis regressions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "agc_witness.h"

/* Synthetic 4092-sample capture: trapped gain, then acquisitions every ~1200
 * samples walking 82 -> 60 -> 16 -> 34 -> trapped in 24-sample steps. State
 * bit `active_bit` is high from `lead` samples before the first change until
 * the walk ends; the other bits are static or noisy. */
static void capture(uint8_t *s, unsigned bit, unsigned active_bit, int invert, unsigned lead,
                    unsigned seed)
{
    static const uint8_t walk[] = {82, 60, 16, 34};
    unsigned trapped = 40;
    for (unsigned k = 0; k < 4092; ++k) {
        unsigned phase = (k + seed) % 1200u;
        unsigned gain = trapped;
        int active = 0;
        if (phase >= 600u && phase < 600u + 4u * 24u) gain = walk[(phase - 600u) / 24u];
        else gain = trapped + ((k + seed + 600u) / 1200u) % 3u; /* level moves only via a walk */
        if (phase + lead >= 600u && phase < 600u + 4u * 24u) active = 1;
        unsigned state;
        if (bit == active_bit) state = (unsigned)(active ^ invert);
        else if (bit == 1u) state = 1u;            /* static high */
        else state = ((k * 2654435761u) >> 13) & 1u; /* noisy */
        s[k] = (uint8_t)(gain | (state << 7));
    }
}

int main(void)
{
    static uint8_t s[4092];
    agc_witness_t w;
    agc_witness_result_t r;
    for (int invert = 0; invert < 2; ++invert) {
        agc_witness_init(&w);
        for (unsigned bit = 0; bit < 4; ++bit)
            for (unsigned window = 0; window < 4; ++window) {
                capture(s, bit, 2, invert, 4, window * 333u);
                agc_witness_add(&w, s, sizeof(s), bit);
            }
        assert(agc_witness_choose(&w, &r));
        assert(r.bit == 2 && r.invert == invert && r.separation_pm >= 900 &&
               r.active_trapped_pm == 0 && r.active_acq_pm >= 980);
        assert(r.lead_samples >= 3 && r.lead_samples <= 5);
        assert(r.gain_min_acq == 16 && r.trapped_min >= 40 && r.trapped_max <= 42);
        assert(r.acq_per_ms_x10 > 300 && r.acq_per_ms_x10 < 360); /* 1200 samples = 30 us */
        printf("witness invert=%d bit=%d sep=%u lead=%u lag=%u acq/ms=%u.%u acq_us=%u.%u share=%u pm "
               "gain_min=%u trapped=%u..%u\n", invert, r.bit, r.separation_pm, r.lead_samples,
               r.lag_samples, r.acq_per_ms_x10 / 10, r.acq_per_ms_x10 % 10, r.acq_us_x10 / 10,
               r.acq_us_x10 % 10, r.acq_share_pm, r.gain_min_acq, r.trapped_min, r.trapped_max);
    }
    /* A bit that is also set on 30 % of trapped samples is refused. */
    agc_witness_init(&w);
    for (unsigned bit = 0; bit < 4; ++bit)
        for (unsigned window = 0; window < 4; ++window) {
            capture(s, bit, 2, 0, 4, window * 333u);
            if (bit == 2)
                for (unsigned k = 0; k < sizeof(s); k += 10)
                    for (unsigned j = 0; j < 3 && k + j < sizeof(s); ++j) s[k + j] |= 0x80u;
            agc_witness_add(&w, s, sizeof(s), bit);
        }
    assert(!agc_witness_choose(&w, &r) && r.active_trapped_pm > 50);
    /* Board 2026-10-06: the widest-separating bit is also set on 20 % of
     * trapped samples; a narrower one covers ~60 % of each walk and is never
     * set while trapped. Safety wins over separation. */
    agc_witness_init(&w);
    for (unsigned bit = 0; bit < 4; ++bit)
        for (unsigned window = 0; window < 4; ++window) {
            capture(s, bit, bit == 3 ? 3 : 2, 0, 4, window * 333u);
            for (unsigned k = 0; k < sizeof(s); ++k) {
                unsigned phase = (k + window * 333u) % 1200u;
                int walking = phase >= 600u && phase < 600u + 4u * 24u;
                if (bit == 3 && !walking && k % 5u == 0u) s[k] |= 0x80u;      /* 20 % trapped */
                if (bit == 2 && walking && phase >= 600u + 58u) s[k] &= 0x7fu; /* ~60 % cover */
            }
            agc_witness_add(&w, s, sizeof(s), bit);
        }
    assert(agc_witness_choose(&w, &r));
    assert(r.bit == 2 && !r.invert && r.active_trapped_pm == 0 &&
           r.active_acq_pm >= 500 && r.active_acq_pm < 800);
    /* Forced gain (the PR #122 probe): no changes, no decision. */
    agc_witness_init(&w);
    memset(s, 52 | 0x80, sizeof(s));
    for (unsigned bit = 0; bit < 4; ++bit) agc_witness_add(&w, s, sizeof(s), bit);
    assert(!agc_witness_choose(&w, &r) && w.acquisitions == 0);
    puts("PASS: AGC witness finds the acquisition state bit and polarity, lead, walk depth, rate");
    return 0;
}
