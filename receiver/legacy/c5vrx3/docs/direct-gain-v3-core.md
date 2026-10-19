# Direct Gain V3, issue #109

This experimental build pairs the full-range adjacent Phase8 demodulator with a hybrid
Q4 gain controller. Q4 envelope and Phase8 coherence provide the receiver's
feedback; PHY RSSI and antenna calibration are not used to choose gain.

## Control path

The Phase8 endpoint difference is mapped as `128 + signed_delta`, then
quantized to six bits. This covers the complete signed delta range -128..+127
without the former early arithmetic wrap at -38/+89. The slope is one half of
the earlier `76 + 2*delta` experiment. A true phase step across +/-180 degrees
still aliases at the signed-delta boundary, and low-amplitude phase estimates
can still be noisy; this mapping only removes the artificial early wrap.

The 1 ms observer reads four separated 64-byte regions from the most recently
completed RX descriptor. It computes centered P50, P90 and P95, origin and
rail occupancy, plus coherence using the same Phase8 table as the live video
program. Reads are rejected if DMA ownership or the gain epoch changes.

The controller keeps a virtual gain request in Q8 dB relative to the current
physical tuple. Small coherent errors integrate gradually: gain-up uses a
quarter of the measured correction per observation, while overload attack
uses three quarters. Larger unsaturated errors request a predictive jump.
Clipping bypasses that estimate and takes the emergency gain-down path.

Tuple selection uses the decoded RF, BB and Fine fields. It favors Fine in the
current RF/BB bank, then BB, then RF. Learned response ratios and uncertainty
gate predicted P50/P95 headroom. A class-specific Schmitt margin prevents a
physical transition until the virtual request has crossed the tuple boundary.
Conflicting transition observations raise uncertainty and reduce confidence.
The receiver enters zero-write HOLD when Q4 is healthy.

An ESP timer wakes a separate lightweight sentinel every 500 us. The callback
only wakes its task. That task inspects one fresh 64-byte completed-DMA window
and notifies the controller on strong rail occupancy or a high P95. The normal
observer remains the only gain writer and performs the emergency action.

After a gain write, observations must be fresh and stable in P50, P95, origin
and coherence. The class settle estimate contributes a minimum guard; repeated
stable observations are still required before verification and learning.

## Hardware status

The build and Phase8 source model compile successfully. Hardware tuning remains
necessary for the SWEET boundaries, the Fine/BB/RF response model, settle
classes and visible transition artifacts. In particular, the 500 us sentinel
and Q4-derived settle detector need live validation before promoting this
controller. Keep the V2 profile available for A/B comparison until then.
