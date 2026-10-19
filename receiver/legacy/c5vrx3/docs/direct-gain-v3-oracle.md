# Direct Gain V3: first measurement gate for issue #105

The existing USB `R` command is the first hardware oracle. It takes control of
gain for six short, fixed-gain measurements and restores the previous gain and
AGC mode afterward. Keep the VTX/channel/power and antenna geometry fixed
during one run. Repeat at close, medium, and far signal strengths.

Each row now reports the decoded RF/BB/Fine tuple, ROM RSSI, raw and centered
Q4 P50, centered P95, phase coherence, clipping, and origin occupancy. The
centered power uses the existing half-cell geometry of signed Q4/I4 samples.

RSSI may become the feed-forward strength input only if it is valid, responds
to VTX strength/distance, and stays approximately invariant when forced gain
changes while Q4 amplitude changes. If RSSI is missing, stale, or follows the
gain writes, use centered Q4 as the primary envelope measurement. Neither
source is promoted based on firmware compilation alone.

The Phase8 live test in PR #104 showed good colors but visible high-gain
artifacts. Its Phase8-specific V2 threshold adjustment did not improve the
picture according to the operator. V3 must be validated against that result,
including visible static around each physical gain write; P50 alone cannot
establish image quality.
