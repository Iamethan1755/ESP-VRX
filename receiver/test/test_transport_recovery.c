/* C5VRX by Twotoz and contributors: fault injection of the production
 * restart coordinator, including rollback and failed cleanup. */
#include <assert.h>
#include <stdio.h>
#include "transport_recovery.h"
#include "output_level.h"
#include "kp_tables.h"
typedef struct { int fail, rollback_fail, clean_fail, cleans, starts, published; bool enabled; } model_t;
static bool clean(void *ctx)
{
    model_t *m = ctx;
    ++m->cleans;
    if (m->cleans == m->clean_fail) return false;
    m->enabled = false;
    return true;
}
static bool step(void *ctx, tr_stage_t stage, bool rollback)
{
    model_t *m = ctx;
    if (stage == TR_BS_ENABLE) { assert(!m->enabled); m->enabled = true; ++m->starts; }
    if ((int)stage == (rollback ? m->rollback_fail : m->fail)) return false;
    if (stage == TR_TX_START) m->published = rollback ? 1 : 2;
    return true;
}
typedef struct { unsigned called; int fail; } stops_t;
static bool stop(void *ctx, tr_part_t part)
{
    stops_t *s = ctx;
    s->called |= 1u << part;
    return (int)part != s->fail;
}
int main(void)
{
    bool tx = true, bs = true, rx = true;
    stops_t stops = {.fail=-1};
    assert(tr_stop_parts(&stops, stop, &tx, &bs, &rx, false));
    assert(!tx && !bs && rx && stops.called == 3u); /* menu calibration still gets fresh IQ */
    stops.called = 0;
    assert(tr_stop_parts(&stops, stop, &tx, &bs, &rx, true));
    assert(!rx && stops.called == 4u);
    for (int part = 0; part < 3; ++part) {
        tx = bs = rx = true;
        stops = (stops_t){.fail=part};
        assert(!tr_stop_parts(&stops, stop, &tx, &bs, &rx, true));
        assert(stops.called == 7u && (tx + bs + rx) == 1);
    }
    for (int s = 0; s < TR_STAGES; ++s) {
        model_t m = {.fail=s, .rollback_fail=-1};
        assert(tr_restart(&m, clean, step, true) == TR_ROLLED_BACK);
        assert(m.published == 1 && m.cleans == 2 && m.starts == 2);
        for (int r = 0; r < TR_STAGES; ++r) {
            m = (model_t){.fail=s, .rollback_fail=r};
            assert(tr_restart(&m, clean, step, true) == TR_LATCHED);
            assert(!m.enabled && !m.published && m.cleans == 3);
        }
        m = (model_t){.fail=s, .rollback_fail=-1, .clean_fail=2};
        assert(tr_restart(&m, clean, step, true) == TR_LATCHED);
        assert(m.starts == 1 && !m.published);
        m = (model_t){.fail=s, .rollback_fail=-1};
        assert(tr_restart(&m, clean, step, false) == TR_LATCHED);
        assert(!m.enabled && m.starts == 1 && !m.published);
    }
    model_t ok = {.fail=-1, .rollback_fail=-1};
    assert(tr_restart(&ok, clean, step, true) == TR_APPLIED && ok.published == 2);
    for (unsigned l = 0; l < OUTPUT_LEVELS; ++l)
        for (unsigned k = 0; k < KP_LEVELS; ++k)
            for (unsigned i = 0; i < 1024; ++i) {
                bool clipped;
                uint16_t v = kp_tables[k][i], out = output_entry(v, l, &clipped);
                assert((v & 0xffc0u) == (out & 0xffc0u));
                if (!l) assert(out == v && !clipped);
                if (clipped) assert((out & 63u) == 0 || (out & 63u) == 63);
            }
    puts("PASS: every restart/rollback stage, terminal cleanup, atomic publication and all DAC/kp maps");
    return 0;
}
