/* SPDX-License-Identifier: GPL-3.0-only */
/* Chroma sums for 16-sample blocks with the ESP32-S3's PIE SIMD (16-lane
 * signed 8-bit multiply-accumulate into ACCX), see video.c:
 *
 *   void chroma_sums_pie(const uint8_t *x, const int8_t *c, const int8_t *s,
 *                        int blocks, int32_t *out);
 *
 * out[2k] = sum of x * c and out[2k + 1] = sum of x * s over block k.
 * x, c and s are 16-byte aligned; samples (0..63) pass as signed bytes.
 * About 12 instructions per block against ~100 for the C loop; video.c
 * checks it against the C version at start and falls back if they differ. */
    .section .iram1.chroma_sums_pie, "ax"
    .global chroma_sums_pie
    .type chroma_sums_pie, @function
    .align 4
chroma_sums_pie:
    entry   a1, 32
    loopnez a5, .Lend
    ee.vld.128.ip       q0, a2, 16      /* 16 samples */
    ee.vld.128.ip       q1, a3, 16      /* 16 cos */
    ee.vld.128.ip       q2, a4, 16      /* 16 sin */
    ee.zero.accx
    ee.vmulas.s8.accx   q0, q1
    rur.accx_0          a8
    ee.zero.accx
    ee.vmulas.s8.accx   q0, q2
    rur.accx_0          a9
    s32i                a8, a6, 0
    s32i                a9, a6, 4
    addi                a6, a6, 8
.Lend:
    retw.n
    .size chroma_sums_pie, . - chroma_sums_pie
