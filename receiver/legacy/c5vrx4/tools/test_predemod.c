/* C5VRX by Twotoz and contributors: pre-demodulation helper regressions. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "predemod.h"
#include "cvbs_tables.h"

static uint8_t iq(int i, int q) { return (uint8_t)(((i & 15) << 4) | (q & 15)); }

int main(void)
{
    /* A smooth carrier at radius 5 cells, 30 degrees per sample: no glitches. */
    uint8_t ring[64];
    for (unsigned k = 0; k < 64; ++k) {
        double a = k * 3.14159265358979 / 6.0;
        ring[k] = iq((int)lround(5 * cos(a) - 0.5), (int)lround(5 * sin(a) - 0.5));
    }
    assert(predemod_glitches(ring, 64, 6) == 0);
    /* A mid-transition read at a zero crossing: -1 -> -8 -> 0 on I. */
    uint8_t mixed[] = {iq(-1, 3), iq(-8, 3), iq(0, 3), iq(1, 3)};
    assert(predemod_glitches(mixed, 4, 6) == 1);
    /* A genuine step (neighbours disagree) is not a glitch. */
    uint8_t step[] = {iq(-6, 0), iq(1, 0), iq(5, 0)};
    assert(predemod_glitches(step, 3, 6) == 0);

    int di, dq;
    uint8_t centred[] = {iq(0, -1), iq(-1, 0)};
    predemod_dc_mcells(centred, 2, &di, &dq);
    assert(di == 0 && dq == 0);
    uint8_t offset[] = {iq(2, 0), iq(2, 0)};
    predemod_dc_mcells(offset, 2, &di, &dq);
    assert(di == 2500 && dq == 500);

    assert(predemod_dc_cal_point(5865, 1) == 5855);
    assert(predemod_dc_cal_point(5917, 1) == 5855);
    assert(predemod_dc_cal_point(5740, 1) == 5775);
    assert(predemod_dc_cal_point(5865, 0) == 2432);

    assert(predemod_filter_code(0xC0 | 20, 8) == (0xC0 | 28));
    assert(predemod_filter_code(55, 16) == 60);
    assert(predemod_filter_code(4, -8) == 0);

    /* Identity response: the step cancels the error, bounded per axis. */
    float j[4] = {1000.f, 0.f, 0.f, 1000.f};
    int a, b;
    assert(predemod_dco_step(j, 3000.f, -2000.f, 32, &a, &b) && a == -3 && b == 2);
    assert(predemod_dco_step(j, 90000.f, 0.f, 32, &a, &b) && a == -32 && b == 0);
    /* Cross-coupled response with swapped mapping still converges. */
    float swapped[4] = {0.f, -800.f, 1200.f, 0.f};
    assert(predemod_dco_step(swapped, 1600.f, 2400.f, 32, &a, &b));
    assert(fabsf(swapped[0] * a + swapped[1] * b + 1600.f) < 500.f);
    assert(fabsf(swapped[2] * a + swapped[3] * b + 2400.f) < 700.f);
    float singular[4] = {1.f, 2.f, 2.f, 4.f};
    assert(!predemod_dco_step(singular, 1.f, 1.f, 32, &a, &b));
    /* The recentring decoder reproduces the generated static table exactly. */
    for (unsigned raw = 0; raw < 256; ++raw)
        assert(predemod_phase8((uint8_t)raw, 0, 0) == c5v4_phase_static[raw]);
    assert(predemod_decoder_word(0x00, 0, 0) == (uint16_t)(((128 + c5v4_phase_static[0]) & 255) |
                                                            (((256 - c5v4_phase_static[0]) & 255) << 8)));
    /* Centre moved +1 cell on I: raw (I=2,Q=0) is (1.49,0.49) from it, about
     * 18 deg; raw (0,0) is (-0.51,0.49), about 136 deg (Phase8 96). */
    assert(predemod_phase8(iq(2, 0), 1000, 0) == 13);
    assert(predemod_phase8(iq(0, 0), 1000, 0) == 97);

    predemod_dc_filter_t f = {0};
    int applied[2] = {0, 0}, out[2];
    int m1[2] = {900, -300}, m2[2] = {950, -280}, far[2] = {9000, 0};
    assert(!predemod_dc_decide(&f, m1, applied, 120, 120, 3000, out)); /* first look */
    assert(predemod_dc_decide(&f, m2, applied, 120, 120, 3000, out) && out[0] == 925 && out[1] == -290);
    applied[0] = out[0]; applied[1] = out[1];
    assert(!predemod_dc_decide(&f, m2, applied, 120, 120, 3000, out)); /* unchanged */
    assert(!predemod_dc_decide(&f, far, applied, 120, 120, 3000, out)); /* jump: not stable */
    assert(predemod_dc_decide(&f, far, applied, 120, 120, 3000, out) && out[0] == 3000); /* clamped */
    int small[2] = {50, -40};
    predemod_dc_filter_t g = {0}; applied[0] = 400; applied[1] = 0;
    assert(!predemod_dc_decide(&g, small, applied, 120, 120, 3000, out));
    assert(predemod_dc_decide(&g, small, applied, 120, 120, 3000, out) && !out[0] && !out[1]);
    /* FFT: a complex tone at +5 bins lands at psd[32+5]. */
    {
        float re[64], im[64];
        for (unsigned k = 0; k < 64; ++k) { re[k] = (float)cos(2 * M_PI * 5 * k / 64); im[k] = (float)sin(2 * M_PI * 5 * k / 64); }
        predemod_fft64(re, im);
        assert(fabsf(re[5] - 64.f) < 1e-3f && fabsf(re[6]) < 1e-3f && fabsf(im[5]) < 1e-3f);
    }
    /* Width: flat to +-12 MHz (bins 32+-19), then 20 dB down. */
    {
        float psd[64];
        for (unsigned k = 0; k < 64; ++k) psd[k] = (abs((int)k - 32) <= 19) ? 1.f : 0.01f;
        psd[32] = 50.f; /* DC spike ignored */
        unsigned w = predemod_psd_width_khz(psd);
        assert(w == 39u * 625u); /* 24.375 MHz full width */
        for (unsigned k = 0; k < 64; ++k) psd[k] = 1.f;
        assert(predemod_psd_width_khz(psd) == 40000u);
        for (unsigned k = 0; k < 64; ++k) psd[k] = 0.f;
        assert(predemod_psd_width_khz(psd) == 0u);
    }
    /* Shaped noise through the real PSD path: a 1-pole low-pass narrows it. */
    {
        float wide[64] = {0}, narrow[64] = {0};
        unsigned seed = 12345;
        float yi = 0, yq = 0;
        for (unsigned r = 0; r < 600; ++r) {
            uint8_t a[64], b[64];
            for (unsigned k = 0; k < 64; ++k) {
                float gi = 0, gq = 0;
                for (unsigned m = 0; m < 6; ++m) {
                    seed = seed * 1103515245u + 12345u; gi += (float)((seed >> 16) & 32767) / 32768.f - 0.5f;
                    seed = seed * 1103515245u + 12345u; gq += (float)((seed >> 16) & 32767) / 32768.f - 0.5f;
                }
                gi *= 2.4f; gq *= 2.4f;
                yi = 0.55f * yi + 0.45f * gi; yq = 0.55f * yq + 0.45f * gq;
                a[k] = iq((int)floorf(gi), (int)floorf(gq));
                b[k] = iq((int)floorf(yi * 2.f), (int)floorf(yq * 2.f));
            }
            predemod_psd_accumulate(a, wide);
            predemod_psd_accumulate(b, narrow);
        }
        unsigned ww = predemod_psd_width_khz(wide), wn = predemod_psd_width_khz(narrow);
        assert(ww == 40000u && wn > 5000u && wn < 30000u);
        /* Noise bandwidth: flat noise fills the whole 40 MHz view; the
         * 1-pole low-pass has a wider noise bandwidth than its -3 dB width. */
        unsigned nw = predemod_psd_nbw_khz(wide), nn = predemod_psd_nbw_khz(narrow);
        assert(nw > 36000u && nw < 44000u);
        assert(nn > wn && nn < nw);
        assert(predemod_nbw_excess_db_x10(nn, wn) > 0);
    }
    /* NBW of exact shapes: a brick wall reads its width, a DC spike is
     * ignored, skirts and folded noise add to it. */
    {
        float psd[64];
        for (unsigned k = 0; k < 64; ++k) psd[k] = (abs((int)k - 32) <= 19) ? 1.f : 0.f;
        psd[32] = 50.f;
        assert(predemod_psd_nbw_khz(psd) == 39u * 625u);
        for (unsigned k = 0; k < 64; ++k) psd[k] = (abs((int)k - 32) <= 19) ? 1.f : 0.25f;
        assert(predemod_psd_nbw_khz(psd) == 39u * 625u + 25u * 625u / 4u);
        for (unsigned k = 0; k < 64; ++k) psd[k] = 0.f;
        assert(predemod_psd_nbw_khz(psd) == 0u);
        assert(predemod_nbw_excess_db_x10(48000, 24000) == 30);
        assert(predemod_nbw_excess_db_x10(0, 24000) == 0);
    }
    /* Two-stage choice: lowest valid noise bandwidth, >= 7 % better than
     * the single-stage entry, width still >= target, noise incoherent. */
    {
        unsigned nbw[] = {30000, 28500, 26000, 25000, 24000};
        unsigned width[] = {24400, 24400, 25000, 23000, 24600};
        bool quiet[] = {true, true, true, true, false};
        assert(predemod_skirt_choose(nbw, width, quiet, 5, 24000) == 2);
        unsigned small[] = {30000, 28500};
        assert(predemod_skirt_choose(small, width, quiet, 2, 24000) == 0); /* 5 %: keep */
        bool q0[] = {false, true};
        assert(predemod_skirt_choose(small, width, q0, 2, 24000) == -1);
        unsigned narrow0[] = {23000, 24400};
        assert(predemod_skirt_choose(nbw, narrow0, quiet, 2, 24000) == -1);
        /* Unreachable target: at most 7 % of the delivered width (board). */
        assert(predemod_skirt_target_khz(24000, 40000) == 24000);
        assert(predemod_skirt_target_khz(24000, 19375) == 18018);
        unsigned board_nbw[] = {22160, 19266, 17184, 15728, 14874};
        unsigned board_w[] = {19375, 18750, 15625, 14375, 13125};
        bool board_q[] = {true, true, true, true, true};
        assert(predemod_skirt_choose(board_nbw, board_w, board_q, 5,
                                     predemod_skirt_target_khz(24000, 19375)) == 1);
    }
    {
        /* Edge: lowest nbw with width >= 14 MHz, valid, >= 0.3 dB better. */
        unsigned nbw[] = {26000, 21000, 16500, 15000, 12000};
        unsigned width[] = {22000, 18000, 15000, 14200, 12500};
        bool valid[] = {true, true, true, true, true};
        assert(predemod_edge_choose(nbw, width, valid, 5, 14000, 27000) == 3);
        valid[3] = false; /* noise too coherent for V5 NO_CARRIER */
        assert(predemod_edge_choose(nbw, width, valid, 5, 14000, 27000) == 2);
        assert(predemod_edge_choose(nbw, width, valid, 5, 14000, 17500) == -1); /* < 0.3 dB */
        assert(predemod_edge_choose(nbw, width, valid, 5, 14000, 18000) == 2);  /* 0.38 dB */
        assert(predemod_edge_choose(nbw, width, valid, 5, 14000, 0) == -1);
        unsigned too_narrow[] = {13000, 12000};
        assert(predemod_edge_choose(nbw, too_narrow, valid, 2, 14000, 27000) == -1);
    }
    {
        unsigned widths[] = {40000, 33000, 27500, 24400, 21000, 0};
        assert(predemod_bw_choose(widths, 6, 24000) == 3);
        unsigned narrow_already[] = {20000, 18000};
        assert(predemod_bw_choose(narrow_already, 2, 24000) == -1);
    }
    /* esp-sdr C5 reference curves (ac627b0b): mode 1 reaches 24 MHz near code
     * 52, code 60 is 22 MHz; mode 0 is 23 MHz wide open. */
    assert(predemod_bw_reference_khz(1, 0) == 48000u && predemod_bw_reference_khz(1, 52) == 24000u);
    assert(predemod_bw_reference_khz(1, 60) == 22000u && predemod_bw_reference_khz(1, 63) == 22000u);
    assert(predemod_bw_reference_khz(0, 0) == 23000u && predemod_bw_reference_khz(0, 20) == 17000u);
    {
        uint8_t codes[16];
        unsigned w0[16], w1[16], err = 0;
        for (unsigned k = 0; k < 16; ++k) {
            codes[k] = (uint8_t)(4 * k);
            w0[k] = predemod_bw_reference_khz(0, codes[k]) + 600u;
            unsigned r1 = predemod_bw_reference_khz(1, codes[k]);
            w1[k] = r1 > 40000u ? 40000u : r1 - 500u;
        }
        assert(predemod_bw_mode_fit(codes, w0, 16, &err) == 0 && err == 600u);
        assert(predemod_bw_mode_fit(codes, w1, 16, &err) == 1 && err < 500u);
        /* Mode 1: narrowest code still >= 24 MHz is code 48 on this coarse grid. */
        int c = predemod_bw_choose(w1, 16, 24000);
        assert(c >= 0 && codes[c] == 48);
        /* Mode 0 never reaches 24 MHz: the caller keeps the widest code. */
        assert(predemod_bw_choose(w0, 16, 24000) == -1);
        unsigned none[2] = {0, 0};
        assert(predemod_bw_mode_fit(codes, none, 2, &err) == -1);
    }
    puts("PASS: glitch metric, DC centre, DC-cal point, relative filter code, DCO solver, exact Phase8 recentring, DC decision, FFT, noise-width estimate, BW choice and esp-sdr curve/mode fit");
    return 0;
}
