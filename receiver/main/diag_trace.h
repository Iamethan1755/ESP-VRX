/* C5VRX-5 by Twotoz and the C5VRX contributors. Bounded passive recorder
 * of timestamped control observations, not extra sample-rate processing.
 * No allocation, I/O, waiting or PHY writes. Caller serialises access. */
#pragma once
#include <string.h>
#include "diag_logic.h"

#define DIAG_TRACE_PRE 16u
#define DIAG_TRACE_POST 16u
#define DIAG_TRACE_SIZE (DIAG_TRACE_PRE + 1u + DIAG_TRACE_POST)
#define DIAG_TRACE_GAP_MS 200u

typedef struct {
    diag_record_t pre[DIAG_TRACE_PRE], event[DIAG_TRACE_SIZE];
    unsigned pre_next, pre_count, count, remaining, trigger;
    uint32_t previous_ms, sequence, skipped;
    bool have_time, had_sync, active, ready;
} diag_trace_t;

static inline void diag_trace_boundary(diag_trace_t *s)
{
    /* Finish a partial capture at a menu/retune/gap boundary, never join
     * observations across an interruption or diagnose it as an RF fade. */
    if (s->active) { s->active = false; s->ready = true; }
    s->pre_next = s->pre_count = 0;
    s->had_sync = s->have_time = false;
}

static inline void diag_trace_push(diag_trace_t *s, const diag_record_t *r, bool owned)
{
    if (owned) { diag_trace_boundary(s); return; }
    if (s->have_time && (uint32_t)(r->t_ms - s->previous_ms) > DIAG_TRACE_GAP_MS)
        diag_trace_boundary(s);
    s->previous_ms = r->t_ms;
    s->have_time = true;
    const bool sync = r->measured && diag_video_ok(r);
    const bool onset = s->had_sync && !sync;
    if (s->active) {
        s->event[s->count++] = *r;
        if (--s->remaining == 0) { s->active = false; s->ready = true; }
    } else if (onset && !s->ready) {
        s->count = s->pre_count;
        for (unsigned k = 0; k < s->pre_count; ++k)
            s->event[k] = s->pre[(s->pre_next + DIAG_TRACE_PRE - s->pre_count + k) % DIAG_TRACE_PRE];
        s->trigger = s->count;
        s->event[s->count++] = *r;
        s->remaining = DIAG_TRACE_POST;
        s->active = true;
        ++s->sequence;
    } else if (onset) ++s->skipped; /* slow console: preserve oldest capture */
    s->had_sync = sync;
    s->pre[s->pre_next] = *r;
    s->pre_next = (s->pre_next + 1u) % DIAG_TRACE_PRE;
    if (s->pre_count < DIAG_TRACE_PRE) ++s->pre_count;
}
