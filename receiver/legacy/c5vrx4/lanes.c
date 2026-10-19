#include "c5vrx4.h"
#include "rf.h"
#include <stdio.h>
#include <inttypes.h>

/* A 4092-byte completed descriptor spans 102.3 us. Its oldest samples may
 * precede publication by another descriptor: 210 us excludes both spans.
 * This only gates ordinary observer evidence, never emergency recovery. */
#define WINDOW_FRESH_US 210u
static uint8_t s_evidence_lane = UINT8_MAX, s_overlap_windows;
static uint32_t s_deferred, s_upgrades, s_recovery, s_stale;

bool c5vrx4_lane_window_ready(uint64_t now_us)
{
    rf_iq_lane_stats_t stats;
    rf_get_iq_lane_stats(&stats);
    bool ready = !stats.switches ||
        (now_us >= stats.last_switch_us &&
         now_us - stats.last_switch_us >= WINDOW_FRESH_US);
    if (!ready) ++s_stale;
    return ready;
}

uint8_t c5vrx4_lane_target(uint8_t current, uint8_t requested,
                         const uint8_t *sample, size_t bytes, uint64_t now_us)
{
    (void)now_us;
    if (current != s_evidence_lane) {
        s_evidence_lane = current;
        s_overlap_windows = 0;
    }
    if (sample && bytes) {
        bool overlap = true;
        for (size_t n = 0; n < bytes; ++n) {
            int i = sample[n] >> 4, q = sample[n] & 15;
            if (i >= 8) i -= 16;
            if (q >= 8) q -= 16;
            /* [-3,2] leaves one source-cell margin within the next lane's
             * valid [-4,3] source-cell window, for both I and Q. */
            if (i < -3 || i > 2 || q < -3 || q > 2) {
                overlap = false;
                break;
            }
        }
        if (!overlap) s_overlap_windows = 0;
        else if (s_overlap_windows < 2) ++s_overlap_windows;
    }
    if (requested < current) {
        ++s_recovery;
        s_overlap_windows = 0;
        return requested; /* Never defer a fold/saturation escape. */
    }
    if (requested == current) return current;
    if (!sample || !bytes || s_overlap_windows < 2) {
        ++s_deferred;
        return current;
    }
    ++s_upgrades;
    s_overlap_windows = 0;
    return current + 1u; /* Upgrade one scale step, never skip the overlap. */
}

void c5vrx4_lane_print(void)
{
    rf_iq_lane_stats_t stats;
    rf_get_iq_lane_stats(&stats);
    printf("C5VRX4_LANES lane=%u history=%d switches=%" PRIu32
           " last=%u->%u route_max_us=%" PRIu32
           " overlap_windows=%u deferred=%" PRIu32 " upgrades=%" PRIu32
           " recovery=%" PRIu32 " stale_windows=%" PRIu32
           " freshness_us=%u atomic=0 transition_tag=0\n",
           rf_get_iq_lanes(), c5vrx4_history_enabled(), stats.switches,
           stats.last_from, stats.last_to, stats.route_max_us,
           s_overlap_windows, s_deferred, s_upgrades, s_recovery, s_stale,
           WINDOW_FRESH_US);
}
