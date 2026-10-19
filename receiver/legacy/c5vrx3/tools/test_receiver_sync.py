"""Compile the production sync observer against synthetic Phase5 IQ windows."""
from pathlib import Path
import subprocess
import tempfile

source = Path("main/video.c").read_text()
luts = source[source.index("static const uint8_t s_phase5_state_lut"):source.index("static uint32_t s_receive_generation;")]
observer = source[source.index("static bool video_standard_observe"):source.index("typedef struct {\n    int p_median;")]
harness = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef enum { VIDEO_STD_NTSC, VIDEO_STD_PAL } video_standard_t;
static void video_standard_vote(video_standard_t standard, uint16_t period) {
    (void)standard; (void)period;
}
""" + luts + observer + r"""
static uint8_t raw[4092];
static void make_video(unsigned period, unsigned width) {
    unsigned phase = 0;
    for (unsigned k = 0; k < sizeof(raw)/2; ++k) {
        bool sync = k % period < width;
        unsigned next = 0;
        for (; next < 32; ++next)
            if (phase5_pair_is_sync(phase, next) == sync) break;
        assert(next < 32);
        unsigned iq = 0;
        for (; iq < 256; ++iq) if (s_phase5_state_lut[iq] == next) break;
        assert(iq < 256);
        raw[2*k] = raw[2*k+1] = iq;
        phase = next;
    }
}
int main(void) {
    assert(!video_standard_observe(NULL, 4092, 0));
    make_video(1271, 94);
    assert(video_standard_observe(raw, sizeof(raw), 0));
    assert(video_standard_observe(raw, sizeof(raw), 1));
    make_video(1280, 94);
    assert(video_standard_observe(raw, sizeof(raw), 0));
    make_video(1000, 94);
    assert(!video_standard_observe(raw, sizeof(raw), 0));
    make_video(1271, 30);
    assert(!video_standard_observe(raw, sizeof(raw), 0));
    for (unsigned i=0; i<sizeof(raw); ++i) raw[i]=0x55;
    assert(!video_standard_observe(raw, sizeof(raw), 0));
    uint32_t rng=123456;
    for (unsigned trial=0; trial<1000; ++trial) {
        for (unsigned i=0; i<sizeof(raw); ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            raw[i]=rng;
        }
        assert(!video_standard_observe(raw, sizeof(raw), 0));
    }
    puts("Sync observer: PAL, NTSC, alignment, malformed pulses, tone and 1000 noise windows passed");
}
"""
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / "sync.c"
    executable = Path(directory) / "sync"
    c.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
