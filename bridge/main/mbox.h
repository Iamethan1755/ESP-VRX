/* SPDX-License-Identifier: GPL-3.0-only */
/* Frame mailbox between the decoder (core 1) and the readers of its frames
 * on core 0: the JPEG encoder and, with BRIDGE_SPI_DISPLAY, the screen.
 *
 * Each frame buffer is rendered into by the decoder, published (the newest
 * finished frame), held by readers, or free. A finished frame replaces the
 * published one, which goes straight back to the decoder if no reader holds
 * it (the newer picture wins), so the decoder never waits or skips a submit
 * (a skipped submit once moved the stream to the other field parity: the
 * picture bobbed). A reader takes each published frame at most once and
 * hands it back when done; the decoder never gets a buffer a reader holds.
 * 2 + readers buffers always leave one for the decoder. One reader with
 * three buffers is the original encoder-only mailbox.
 *
 * Plain C without locking (the caller serialises the calls), so the host
 * test checks it on a PC, also against the original mailbox. */
#pragma once
#include <stdint.h>

#define MBOX_READERS_MAX 2
#define MBOX_FRAMES_MAX  (2 + MBOX_READERS_MAX)

typedef struct {
    uint8_t *frame[MBOX_FRAMES_MAX];
    uint8_t refs[MBOX_FRAMES_MAX];   /* 1 while published, +1 per reader holding it */
    int frames, readers;
    uint8_t *ready;                  /* the published frame, NULL when none */
    int ready_w, ready_h;
    unsigned unread;                 /* readers (bit per reader) that have not taken `ready` */
} mbox_t;

/* `frames`: 2 + `readers` buffers, [0] the one the decoder renders into first. */
void mbox_init(mbox_t *m, uint8_t *const *frames, int readers);
/* Decoder: publishes the finished frame `fb` and returns the buffer to
 * render the next frame into. Before mbox_init it returns `fb` (the frame
 * is dropped: the decoder keeps its buffer). */
uint8_t *mbox_submit(mbox_t *m, uint8_t *fb, int w, int h);
/* Reader `reader` (0 .. readers - 1): the published frame if this reader
 * has not had it yet, else NULL. Hand it back with mbox_release(). */
uint8_t *mbox_take(mbox_t *m, int reader, int *w, int *h);
void mbox_release(mbox_t *m, const uint8_t *fb);
