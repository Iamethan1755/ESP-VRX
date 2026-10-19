/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the dropout
 * classifier (main/diag_logic.h) on synthetic per-second records, and of
 * the coherence -> C/N estimate and AFC gate (main/cn_estimate.h).
 *
 * The records model the situations the field report asks to separate:
 * a fast fade with gain headroom, a slow walk into the receiver noise at
 * the table maximum, a carrier that stays coherent while the picture goes
 * (demod/sync cliff), chain faults, and ambiguous cases that must stay
 * UNKNOWN. The thresholds are evidence rules, not a calibrated RF model;
 * physical validation needs board logs (docs/FIELD_TEST.md).
 *
 *   cc -O2 -std=c11 -I../main test_diag.c -o test_diag && ./test_diag
 */
#include <stdio.h>
#include <string.h>
#include "cn_estimate.h"
#include "diag_logic.h"
#include "diag_trace.h"

#define MAX_GAIN 83u

static unsigned s_fail, s_run;

static diag_record_t rec(uint8_t video, uint8_t gain, int coherence, uint8_t flags)
{
    diag_record_t r;
    memset(&r, 0, sizeof(r));
    r.video_pct = video;
    r.coverage_pct = 100;
    r.measured = true;
    r.gain = gain;
    r.gain_max = MAX_GAIN;
    r.coherence = (int16_t)coherence;
    r.cn_x10 = (int16_t)cn_estimate_x10(coherence, 20);
    r.flags = flags;
    return r;
}

static void expect(const char *name, const diag_record_t *r, unsigned n, int want)
{
    int got = diag_classify(r, n);
    ++s_run;
    if (got != want) {
        ++s_fail;
        printf("FAIL %s: got %s want %s\n", name, diag_category_name(got), diag_category_name(want));
    }
}

int main(void)
{
    /* Fast fade: strong carrier 20 steps below the maximum, gone in 1 s. */
    {
        diag_record_t r[] = {rec(100, 50, 92, DIAG_F_AFC_LOCKED), rec(100, 52, 91, DIAG_F_AFC_LOCKED),
                             rec(0, MAX_GAIN, 60, 0)};
        expect("fast fade", r, 3, DIAG_PROPAGATION_FADE);
    }
    /* Fade with a one-second partial picture between: still within 2 s. */
    {
        diag_record_t r[] = {rec(100, 60, 90, 0), rec(30, 75, 70, 0), rec(0, MAX_GAIN, 48, 0)};
        expect("fade via partial second", r, 3, DIAG_PROPAGATION_FADE);
    }
    /* Slow walk into the noise: at the maximum with a weak carrier, then
     * noise-like IQ. */
    {
        diag_record_t r[] = {rec(100, 76, 88, 0), rec(100, 81, 84, 0), rec(90, MAX_GAIN, 82, 0),
                             rec(0, MAX_GAIN, 60, 0)};
        expect("rf floor", r, 4, DIAG_AT_RF_FLOOR);
    }
    /* Long gap since the last picture with headroom then: not "fast". */
    {
        diag_record_t r[] = {rec(100, 60, 90, 0), rec(30, 75, 70, 0), rec(20, 80, 66, 0), rec(10, 82, 60, 0),
                             rec(0, MAX_GAIN, 60, 0)};
        expect("slow loss with headroom -> unknown", r, 5, DIAG_UNKNOWN);
    }
    /* Coherent carrier but no confirmed sync: demod / sync cliff. */
    {
        diag_record_t r[] = {rec(100, MAX_GAIN, 86, 0), rec(0, MAX_GAIN, 84, 0)};
        expect("demod cliff", r, 2, DIAG_DEMOD_CLIFF);
    }
    /* Chain faults win over every RF reading. */
    {
        const uint8_t flags[] = {DIAG_F_FILTER_MISMATCH, DIAG_F_SPHASE_BAD, DIAG_F_NO_DC, DIAG_F_FAULT, DIAG_F_AFC_STEP};
        for (unsigned k = 0; k < sizeof(flags); ++k) {
            diag_record_t r[] = {rec(100, 50, 92, 0), rec(0, MAX_GAIN, 60, flags[k])};
            expect("chain flag at dropout", r, 2, DIAG_CALIBRATION_OR_PHY);
            diag_record_t q[] = {rec(100, 50, 92, flags[k]), rec(0, MAX_GAIN, 60, 0)};
            expect("chain flag at last picture", q, 2, DIAG_CALIBRATION_OR_PHY);
        }
    }
    /* Informational flags are not chain faults. */
    {
        diag_record_t r[] = {rec(100, 50, 92, DIAG_F_OUT_OF_SPEC | DIAG_F_AFC_LOCKED), rec(0, MAX_GAIN, 60, DIAG_F_OUT_OF_SPEC)};
        expect("out of spec is informational", r, 2, DIAG_PROPAGATION_FADE);
    }
    /* A chain flag before the last picture does not count. */
    {
        diag_record_t r[] = {rec(100, 50, 92, DIAG_F_FAULT), rec(100, 50, 92, 0), rec(0, MAX_GAIN, 60, 0)};
        expect("old fault ignored", r, 3, DIAG_PROPAGATION_FADE);
    }
    /* At the maximum, middling coherence at the dropout: ambiguous. */
    {
        diag_record_t r[] = {rec(100, MAX_GAIN, 82, 0), rec(0, MAX_GAIN, 72, 0)};
        expect("ambiguous coherence", r, 2, DIAG_UNKNOWN);
    }
    /* Strong carrier before at the maximum (antenna disconnected, say):
     * coherence was not weak, so not the floor. */
    {
        diag_record_t r[] = {rec(100, MAX_GAIN, 95, 0), rec(0, MAX_GAIN, 60, 0)};
        expect("strong at max then noise", r, 2, DIAG_UNKNOWN);
    }
    /* No picture before: not a dropout at all. */
    {
        diag_record_t r[] = {rec(0, MAX_GAIN, 60, 0), rec(30, MAX_GAIN, 60, 0), rec(0, MAX_GAIN, 60, 0)};
        expect("no picture before", r, 3, DIAG_UNKNOWN);
        expect("single record", r, 1, DIAG_UNKNOWN);
    }
    /* Exhaustive sweep: with any chain flag set at the dropout, nothing but
     * CALIBRATION_OR_PHY; without, never CALIBRATION_OR_PHY. */
    for (unsigned g = 0; g <= MAX_GAIN; ++g)
        for (int c0 = 0; c0 <= 100; c0 += 5)
            for (int c1 = 0; c1 <= 100; c1 += 5) {
                diag_record_t r[] = {rec(100, (uint8_t)g, c0, 0), rec(0, MAX_GAIN, c1, 0)};
                int a = diag_classify(r, 2);
                r[1].flags = DIAG_F_NO_DC;
                int b = diag_classify(r, 2);
                ++s_run;
                if (a == DIAG_CALIBRATION_OR_PHY || b != DIAG_CALIBRATION_OR_PHY) {
                    ++s_fail;
                    printf("FAIL sweep g=%u c0=%d c1=%d: %s / %s\n", g, c0, c1, diag_category_name(a),
                           diag_category_name(b));
                }
                /* A fade needs headroom. */
                ++s_run;
                if (a == DIAG_PROPAGATION_FADE && g + DIAG_FADE_HEADROOM > MAX_GAIN) {
                    ++s_fail;
                    printf("FAIL sweep fade without headroom g=%u\n", g);
                }
            }
    /* C/N estimate: monotonic in coherence, noise alone is "unknown", and
     * the model table points map back to their C/N. */
    {
        const int filters[] = {10, 12, 16, 20};
        const int noise[] = {61, 58, 51, 42};             /* afc_bias.py --coherence, noise only */
        const int at2db[] = {78, 76, 70, 63}, at8db[] = {94, 92, 89, 84};
        for (unsigned f = 0; f < 4u; ++f) {
            int prev = -10000;
            for (int c = 0; c <= 100; ++c) {
                int v = cn_estimate_x10(c, filters[f]);
                ++s_run;
                if (v < prev) { ++s_fail; printf("FAIL cn not monotonic f=%d c=%d\n", filters[f], c); }
                prev = v;
            }
            ++s_run;
            if (cn_estimate_x10(noise[f], filters[f]) != CN_UNKNOWN_X10) {
                ++s_fail;
                printf("FAIL noise-only coherence %d at %d MHz reads %d\n", noise[f], filters[f],
                       cn_estimate_x10(noise[f], filters[f]));
            }
            /* AFC: no retune at the range edge, retune on a clear carrier. */
            s_run += 2;
            if (afc_correction_allowed(at2db[f], filters[f])) { ++s_fail; printf("FAIL AFC allowed at 2 dB f=%d\n", filters[f]); }
            if (!afc_correction_allowed(at8db[f], filters[f])) { ++s_fail; printf("FAIL AFC refused at 8 dB f=%d\n", filters[f]); }
            int e2 = cn_estimate_x10(at2db[f], filters[f]), e8 = cn_estimate_x10(at8db[f], filters[f]);
            s_run += 2;
            if (e2 < 0 || e2 > 40) { ++s_fail; printf("FAIL 2 dB point reads %d f=%d\n", e2, filters[f]); }
            if (e8 < 60 || e8 > 100) { ++s_fail; printf("FAIL 8 dB point reads %d f=%d\n", e8, filters[f]); }
        }
        /* The same coherence means a weaker carrier through a narrower filter. */
        ++s_run;
        if (!(cn_estimate_x10(80, 10) < cn_estimate_x10(80, 12) && cn_estimate_x10(80, 12) < cn_estimate_x10(80, 16) &&
              cn_estimate_x10(80, 16) < cn_estimate_x10(80, 20))) {
            ++s_fail;
            printf("FAIL filter ordering\n");
        }
    }
    /* Coverage regression: ten successes in twenty windows is 50%,
     * never 100% overall sync. No measurement is UNKNOWN, not RF floor. */
    {
        diag_record_t r = {0};
        diag_coverage(&r, 20, 10, 10);
        ++s_run;
        if (r.video_pct != 50 || r.coverage_pct != 50 || r.sync_measured_pct != 100) ++s_fail;
        diag_coverage(&r, 20, 0, 0);
        ++s_run;
        if (r.video_pct || r.coverage_pct || r.sync_measured_pct) ++s_fail;
        diag_coverage(&r, 0, 0, 0);
        ++s_run;
        if (r.video_pct || r.coverage_pct || r.sync_measured_pct) ++s_fail;
        diag_record_t q[] = {rec(100, MAX_GAIN, 82, 0), rec(0, MAX_GAIN, 60, 0)};
        q[1].coverage_pct = 0;
        expect("missing evidence", q, 2, DIAG_UNKNOWN);
        q[1].coverage_pct = 100;
        q[1].cn_x10 = CN_UNKNOWN_X10;
        expect("unknown CN is not an RF floor", q, 2, DIAG_UNKNOWN);
        q[0].gain = 50;
        q[1].cn_x10 = 90;
        expect("coherent IQ takes precedence over headroom", q, 2, DIAG_DEMOD_CLIFF);
    }
    /* Recorder: first onset, a single 50-ms loss/recovery, chronological
     * pre/post history, bounded wrap, slow console, ownership and gap. */
    {
        diag_trace_t trace = {0};
        diag_record_t r = rec(100, 50, 92, 0);
        for (unsigned k = 0; k < 40; ++k) {
            r.t_ms = k * 50;
            diag_trace_push(&trace, &r, false);
        }
        r.t_ms = 2000; r.video_pct = 0;
        diag_trace_push(&trace, &r, false);
        ++s_run;
        if (!trace.active || trace.trigger != DIAG_TRACE_PRE || trace.event[trace.trigger].t_ms != 2000 ||
            trace.event[0].t_ms != 1200) ++s_fail;
        for (unsigned k = 1; k <= DIAG_TRACE_POST; ++k) {
            r.t_ms = 2000 + k * 50; r.video_pct = 100;
            diag_trace_push(&trace, &r, false);
        }
        ++s_run;
        if (!trace.ready || trace.active || trace.count != DIAG_TRACE_SIZE || trace.event[trace.trigger + 1].t_ms != 2050)
            ++s_fail;
        r.t_ms += 50; r.video_pct = 0;
        diag_trace_push(&trace, &r, false);
        ++s_run;
        if (trace.skipped != 1 || trace.event[trace.trigger].t_ms != 2000) ++s_fail;
        trace.ready = false;
        r.video_pct = 100; r.t_ms += 50;
        diag_trace_push(&trace, &r, true);
        r.video_pct = 0; r.t_ms += 50;
        diag_trace_push(&trace, &r, false);
        ++s_run;
        if (trace.active || trace.ready) ++s_fail;
        r.video_pct = 100; r.t_ms += 50;
        diag_trace_push(&trace, &r, false);
        r.video_pct = 0; r.t_ms += 1000;
        diag_trace_push(&trace, &r, false);
        ++s_run;
        if (trace.active) ++s_fail;
        /* Timestamp wrap is not a time gap. Missing measurement triggers
         * evidence loss but retains measured=0 for an UNKNOWN verdict. */
        memset(&trace, 0, sizeof(trace));
        r.video_pct = 100; r.measured = true; r.t_ms = UINT32_MAX - 20;
        diag_trace_push(&trace, &r, false);
        r.t_ms = 29; r.measured = false; r.video_pct = 0;
        diag_trace_push(&trace, &r, false);
        ++s_run;
        if (!trace.active || trace.event[trace.trigger].measured) ++s_fail;
        diag_trace_boundary(&trace);
        ++s_run;
        if (!trace.ready || trace.active || trace.had_sync || trace.count != 2) ++s_fail;
    }
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
