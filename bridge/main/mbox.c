/* SPDX-License-Identifier: GPL-3.0-only */
/* Frame mailbox, see mbox.h. */
#include "mbox.h"
#include <stddef.h>
#include <string.h>

static int index_of(const mbox_t *m, const uint8_t *fb)
{
    for (int i = 0; i < m->frames; i++)
        if (fb && m->frame[i] == fb) return i;
    return -1;
}

void mbox_init(mbox_t *m, uint8_t *const *frames, int readers)
{
    memset(m, 0, sizeof *m);
    m->readers = readers < MBOX_READERS_MAX ? readers : MBOX_READERS_MAX;
    m->frames = 2 + m->readers;
    for (int i = 0; i < m->frames; i++) m->frame[i] = frames[i];
}

uint8_t *mbox_submit(mbox_t *m, uint8_t *fb, int w, int h, int fmt)
{
    const int f = index_of(m, fb);
    if (f < 0) return fb;
    const int r = index_of(m, m->ready);
    int next = -1;
    if (r >= 0 && m->refs[r] == 1) {
        next = r;   /* published only, no reader holds it: the newer picture wins */
    } else {
        for (int i = 0; i < m->frames && next < 0; i++)
            if (i != f && m->refs[i] == 0) next = i;
    }
    if (next < 0) return fb;   /* only if a reader holds more than one: drop this frame */
    if (r >= 0) m->refs[r]--;   /* unpublished; readers that took it still hold it */
    m->ready = fb;
    m->refs[f] = 1;
    m->ready_w = w;
    m->ready_h = h;
    m->ready_fmt = fmt;
    m->unread = (1u << m->readers) - 1;
    return m->frame[next];
}

uint8_t *mbox_take(mbox_t *m, int reader, int *w, int *h, int *fmt)
{
    const unsigned bit = 1u << reader;
    const int r = index_of(m, m->ready);
    if (r < 0 || !(m->unread & bit)) return NULL;
    m->unread &= ~bit;
    m->refs[r]++;
    *w = m->ready_w;
    *h = m->ready_h;
    *fmt = m->ready_fmt;
    return m->ready;
}

void mbox_release(mbox_t *m, const uint8_t *fb)
{
    const int i = index_of(m, fb);
    if (i >= 0 && m->refs[i] > 0) m->refs[i]--;
}
