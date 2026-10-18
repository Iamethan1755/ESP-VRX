/* SPDX-License-Identifier: GPL-3.0-only */
/* Motion-adaptive temporal denoise of 8-bit grayscale frames: a pixel close
 * to the running picture is noise and is averaged with it (noise drops to
 * ~0.6x after a few frames); one that differs more is motion and is taken
 * as it is, so moving things do not smear. Runs in the JPEG encoder's task,
 * off the realtime decoder (PSRAM traffic there stalled the decoder).
 * Plain C, also used by the host test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DENOISE_MOTION 28   /* a pixel change above this (of 255) counts as motion */
#define DENOISE_W_MAX  320

/* Updates the running picture `acc` (w * h) with frame `in`; `first`
 * restarts it from `in`. Afterwards `acc` is the picture to show. */
void denoise_frame(uint8_t *acc, const uint8_t *in, int w, int h, bool first);
