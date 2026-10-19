#include "phase8_envelope.h"
#include "phase8_gain_lut.h"
#include "direct_gain_v3.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

unsigned char phy_param[0x800];

static uint8_t cell(int i, int q) { return (uint8_t)(((i & 15) << 4) | (q & 15)); }

int main(void)
{
    /* The four origin-touching cells from issue #119 and their LUT angles. */
    const uint8_t central[4] = {0x00, 0xF0, 0xFF, 0x0F};
    unsigned central_count = 0;
    for (unsigned raw = 0; raw < 256; ++raw) central_count += p8env_is_central((uint8_t)raw);
    assert(central_count == 4);
    for (unsigned k = 0; k < 4; ++k) {
        assert(p8env_is_central(central[k]));
        assert(p8env_power(central[k]) == 1);
        assert(p8env_radius_bin(central[k]) == 0);
    }
    assert(c5vrx_phase8_gain_lut[0x00] == 32);   /* +45 degrees  */
    assert(c5vrx_phase8_gain_lut[0xF0] == 97);   /* +136 degrees */
    assert(c5vrx_phase8_gain_lut[0xFF] == 160);  /* -135 degrees */
    assert(c5vrx_phase8_gain_lut[0x0F] == 223);  /* -46 degrees  */
    /* Every adjacent central-cell move is already a hard (>= 90 degree) step. */
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b)
            if (a != b) {
                int d = p8env_signed_delta(c5vrx_phase8_gain_lut[central[a]],
                                           c5vrx_phase8_gain_lut[central[b]]);
                assert(d >= P8ENV_HARD_DELTA || d <= -P8ENV_HARD_DELTA);
            }
    assert(p8env_is_clip(cell(7, 0)) && p8env_is_clip(cell(0, -8)));
    assert(!p8env_is_clip(cell(6, -7)));
    assert(p8env_radius_bin(cell(3, 3)) == 4);    /* ci=cq=7: r=4.95 cells */
    assert(p8env_radius_bin(cell(-8, -8)) == 10);

    /* Collapsed noise: the vector wanders among the central cells. */
    uint8_t collapsed[512];
    for (unsigned n = 0; n < sizeof(collapsed); ++n)
        collapsed[n] = central[(n * 7u + n / 3u) & 3u];
    p8env_accum_t acc;
    p8env_reset(&acc);
    p8env_add(&acc, collapsed, sizeof(collapsed), c5vrx_phase8_gain_lut);
    p8env_summary_t s = p8env_summarize(&acc, NULL);
    assert(s.central_pm == 1000 && s.origin_pm == 1000);
    assert(s.cls == P8ENV_CLASS_COLLAPSE);
    assert(s.hard_pm > 500);
    assert(s.central_hard_share_pm == 1000);
    assert(s.coherence_pm == 0);

    /* Healthy annulus: a slowly rotating vector at radius ~4.5 cells. */
    static const int ring[16][2] = {
        {4, 0}, {4, 1}, {3, 3}, {1, 4}, {0, 4}, {-2, 4}, {-4, 3}, {-5, 1},
        {-5, 0}, {-5, -2}, {-4, -4}, {-2, -5}, {-1, -5}, {1, -5}, {3, -4}, {4, -2},
    };
    uint8_t annulus[512];
    for (unsigned n = 0; n < sizeof(annulus); ++n)
        annulus[n] = cell(ring[(n / 2u) & 15u][0], ring[(n / 2u) & 15u][1]);
    p8env_reset(&acc);
    p8env_add(&acc, annulus, sizeof(annulus), c5vrx_phase8_gain_lut);
    s = p8env_summarize(&acc, NULL);
    assert(s.central_pm == 0 && s.origin_pm == 0 && s.clip_pm == 0);
    assert(s.hard_pm == 0);
    assert(s.coherence_pm == 1000);
    assert(s.p50 >= 13 && s.p50 <= 32);
    assert(s.cls == P8ENV_CLASS_ANNULUS);
    unsigned radius_total = 0;
    for (unsigned b = 0; b < P8ENV_RADIUS_BINS; ++b) radius_total += s.radius_pm[b];
    assert(radius_total >= 998 && radius_total <= 1002);

    /* Rails classify as CLIP before anything else. */
    uint8_t rails[128];
    for (unsigned n = 0; n < sizeof(rails); ++n) rails[n] = cell(7, (int)(n & 7u) - 4);
    p8env_reset(&acc);
    p8env_add(&acc, rails, sizeof(rails), c5vrx_phase8_gain_lut);
    assert(p8env_summarize(&acc, NULL).cls == P8ENV_CLASS_CLIP);

    /* Separate capture blocks must not create a synthetic pair. */
    p8env_reset(&acc);
    const uint8_t a = cell(4, 0), b = cell(-4, 0);
    p8env_add(&acc, &a, 1, c5vrx_phase8_gain_lut);
    p8env_break(&acc);
    p8env_add(&acc, &b, 1, c5vrx_phase8_gain_lut);
    assert(acc.samples == 2 && acc.pairs == 0 && acc.hard == 0);
    p8env_add(&acc, &a, 1, c5vrx_phase8_gain_lut);
    assert(acc.pairs == 1 && acc.hard == 1 && acc.hard_outer == 1);

    /* P50/P90/P95, origin and clip agree exactly with the DG3 observer. */
    uint8_t mixed[256];
    uint32_t x = 0x1234567u;
    for (unsigned n = 0; n < sizeof(mixed); ++n) {
        x = x * 1664525u + 1013904223u;
        mixed[n] = (uint8_t)(x >> 24);
    }
    dg3_observation_t dg3 = direct_gain_v3_measure(mixed, sizeof(mixed),
                                                   c5vrx_phase8_gain_lut, 0);
    p8env_reset(&acc);
    p8env_add(&acc, mixed, sizeof(mixed), c5vrx_phase8_gain_lut);
    s = p8env_summarize(&acc, NULL);
    assert(s.p50 == dg3.p50 && s.p90 == dg3.p90 && s.p95 == dg3.p95);
    assert(s.origin_pm >= dg3.origin_pm && s.origin_pm <= dg3.origin_pm + 1u);
    assert(s.clip_pm >= dg3.clip_pm && s.clip_pm <= dg3.clip_pm + 1u);
    assert(strcmp(p8env_class_name(s.cls), "EMPTY") != 0);

    puts("Phase8 envelope: origin-collapse / annulus oracle passed");
    return 0;
}
