# ESP32-C5 analog-FPV RF range architecture

This note captures the next receiver-front-end architecture for C5VRX. It
separates evidence-backed controls from ROM symbols that are interesting but
not safe to use in production yet.

## Objective

Maximize usable analog-FPV range while keeping live-video latency and visible
re-lock events as close to zero as practical.

For the current C5VRX datapath the key constraint is the live MODEM_DIAG
Q4/I4 tap. Range is therefore not simply "maximum gain". The useful operating
point is the receive-chain state that gives the highest pre-demodulation
carrier coherence while filling the four-bit I/Q range without rail clipping,
large DC displacement, or excessive I/Q imbalance.

## Current safe/used controls

### Forced RX gain

`phy_force_rx_gain(force, gain)` is already in production and is characterized
by the G2..G62 sweep. The sweep now also records the known RX filter/ADC
register state and Q4/I4 DC/IQ metrics. This is intended to reveal discontinuous
state boundaries before assigning physical labels such as LNA, mixer/baseband
or ADC gain.

### FFT scale

The ESP32-C5 ROM exports `phy_fft_scale_force()`, and Espressif's CSI
gain-control design treats FFT gain separately from AGC gain. C5VRX now exposes
this only through the bounded `F` lab probe.

Production must not use FFT gain for range unless a forced FFT change measurably
changes the raw MODEM_DIAG Q4/I4 stream. A CSI amplitude change alone does not
satisfy that requirement.

### Receive bandwidth

BW40/BW20 through `phy_wifi_fbw_sel()` is already a known C5VRX path. The
`W` probe makes a fixed-gain comparison repeatable. Automatic bandwidth
switching is forbidden while TRACK is locked so a filter write cannot create a
CVBS sync disturbance on an otherwise good link.

### Carrier offset

Frequency-offset changes can cause the RF layer to re-assert the forced receive
gain. C5VRX now records these as both a PHY write and gain-write correlation
event. AUTO AFC is acquisition-only; once TRACK is reached it freezes the
current RF state.

## Experimental RX profiles

PR #35 now exposes the range research as explicit menu profiles with
**RANGE EXP on fixed BW40** as the boot/default profile. On the RF page, a normal long press
still cycles BW40/BW20/AUTO; holding for about two seconds cycles the complete
RX profile.

### BALANCED

Known production behavior: C5VRX fixed-gain controller, BW40 default, AFC off,
no undocumented PHY polling, no hardware AGC and no forced FFT scaling.

### RANGE EXP

The ACTIVE range controller in `main/range_control.h` starts from the applied
hardware gain and holds settings during usable video with amplitude headroom.
It scores ten settled observations using recent Phase5 H-sync evidence,
coherence, clipping and near-origin occupancy. This is a heuristic, not a
calibrated SNR or a measurement of visible snow.

Persistent clipping reduces gain. Severe clipping can bypass normal settling
after two 50 ms ticks. Ordinary trial changes retain a conservative 500 ms hold.
A trial must improve its score by a margin; otherwise the previous gain is
restored and repeated unsuccessful trials receive a 4..32 second backoff.
The controller explores lower gain states after sustained absence of sync,
including states near G2, rather than parking permanently at maximum gain.
Channel/profile reset invalidates trial history. Learning is volatile and does
not write NVS. Clean reception performs no optimization gain writes.

BW40 and AFC OFF remain the default test conditions. ACTIVE RANGE bypasses the
legacy automatic bandwidth/AFC loop. No additional undocumented PHY controls
are enabled. IQ DC/skew/cross is diagnostic only, not proof of compression.

Tests exercise clean hold, bounded clipping response, rejected trials/rollback,
noise recovery, state reset and synthetic PAL/NTSC detection. The physical gain
settling time, optimal sensitivity, filtered-noise rejection and visible snow
are not proven by these tests. Measuring blanking noise and gain-transition
waveforms remains hardware work. The score can still be affected by a changing
RF environment during a trial; no pixel-perfect or zero-relock claim is made.

### BLOCKER EXP

Starts at moderate gain and caps the C5VRX controller at G48. BW40 remains
active. This targets strong nearby transmitters, multi-VTX environments and
testing whether apparent weak-signal failures are actually front-end
compression/blocking.

### RECOVERY

Keeps BW40 but makes SEARCH/relearn faster and enables acquisition-only AFC.
TRACK still freezes software PHY writes. This targets the user-visible
brief-RF-disturbance -> slow goggles re-lock failure mode.

### AUTO EXP

Combines the safe experimental evidence rather than blindly enabling every ROM
knob:

- raw Q4 phase coherence remains the primary lock signal;
- rail clipping and near-origin occupancy drive gain direction;
- Q4 DC offsets, I/Q power skew and I/Q cross-correlation reject gain states
  that have good amplitude but poor phase geometry;
- rate-limited PHY noise-floor and wideband-RSSI reads are used only as a
  bounded SEARCH bias when their values are physically plausible;
- BW40/BW20 changes are acquisition-only and freeze in TRACK;
- AFC is acquisition-only and freezes in TRACK;
- FFT scaling remains automatic unless the bounded `F` probe, on the current
  boot, proves a material improvement in raw MODEM_DIAG Q4 metrics. Only then
  does AUTO reuse the best measured FFT value.

The purpose is a self-characterizing receiver, not a fast software loop that
continually perturbs the PHY.

### Retired HW AGC experiment

The former HW AGC profile was removed by ARC. Binary analysis of the pinned
C5 PHY proved that `phy_agc_max_gain_set()` consumes two arguments describing
the ends of two generated gain tables; the old one-argument declaration had an
invalid ABI. `phy_enable_agc()` also does not reverse the separate
`phy_rfagc_disable()` state. The replacement `H` oracle is strictly read-only
and is documented in [arc-receive-chain.md](arc-receive-chain.md).

## Read-only observability added in this PR

Every lab row can include:

- raw forced-gain status register;
- RX filter register and decoded filter-mode field;
- ADC-rate register and selector;
- MODEM source-mux register;
- Q4/I4 median magnitude and phase coherence;
- clipping and near-origin rates;
- mean I and Q offsets;
- I/Q power-skew metric;
- I/Q cross-correlation metric;
- transport faults and proximity to the last PHY write.

This makes a single attenuator sweep useful for both sensitivity and internal
state mapping.

## Interesting C5 ROM controls that remain gated

The ESP32-C5 ROM symbol map also exposes receive-side functions including:

- `phy_pbus_set_rxgain`
- `phy_rx_gain_force`
- `phy_agc_max_gain_set`
- `phy_wifi_agc_sat_gain`
- `phy_set_rx_sense` / `phy_rx_sense_set`
- `phy_noise_floor_auto_set` / `phy_read_hw_noisefloor`
- `phy_bb_gain_index`
- `phy_gen_rx_gain_table`
- `phy_write_gain_mem`
- `phy_chan_filt_set`
- `phy_rx_filter_mode`
- RX DC/IQ calibration/correction helpers

ARC has since reconstructed the gain-table ABI and stage representation; see
[arc-receive-chain.md](arc-receive-chain.md). It still does not invoke the
PBUS, sense, filter or calibration writers directly. Read-only
`phy_get_noise_floor()` / `phy_get_rssi()` observations remain confined to the
AUTO experiment, and ARC does not poll them during LOCK.

The remaining promotion rule is:

1. establish the function ABI or the exact register changes;
2. test it in a bounded lab build;
3. measure Q4/I4, CVBS and transport before/after;
4. prove the setting survives without periodic recalibration or packet-state
   hunting;
5. only then expose it as a C5VRX profile parameter.

## Desired final gain architecture

Do not build a fast software AGC that continuously chases metrics. The target
architecture is a small number of measured receive profiles, for example
NEAR/MID/FAR/BLOCKER, where each profile is eventually a proven tuple of RF
gain, baseband gain and filter state.

The labels must not be assigned to concrete ROM settings until the stage map is
measured.

During SEARCH/LEARN the controller may change profile. During TRACK:

- no automatic gain write;
- no automatic bandwidth write;
- no automatic AFC/offset write;
- no FFT-force write;
- no calibration call.

A severe overload can leave TRACK and reacquire rather than silently modifying
the PHY underneath a valid CVBS stream.

## DC/IQ calibration direction

Four-bit phase demodulation makes receiver centering unusually important.
A DC-shifted or elliptical I/Q cloud spends fewer effective codes on the desired
carrier and introduces phase-dependent error.

The new Q4/I4 DC/skew/cross metrics are the first step. Candidate ROM RX-DC/IQ
calibration calls should only be promoted after their ABI is proven and their
transient is measured. The preferred lifecycle is startup/channel-change or
long-unlocked calibration, never periodic calibration during TRACK.

## Filter direction

A narrower receive filter can improve integrated noise, but analog FPV is
wideband FM. Therefore the optimum is not automatically BW20. The correct
hardware experiment is a calibrated RF frequency sweep at constant power,
recording Q4 magnitude/coherence and recovered CVBS versus offset for each
filter state.

Only a state that rejects additional noise/interference without clipping useful
FM sidebands belongs in a FAR profile.

## Latency / lag rule

The steady-state hardware pipeline already has sub-frame transport latency.
Large visible freezes are more likely to be caused by a short PHY/CVBS
disturbance that makes the downstream analog decoder re-lock.

Therefore the optimization priority is not larger buffering. It is:

1. preserve uninterrupted MODEM_DIAG/PARLIO transport;
2. eliminate avoidable PHY writes while locked;
3. make acquisition writes measurable and infrequent;
4. preserve valid CVBS sync through weak-signal behavior where possible.

Issue #28 now correlates transport faults with both gain writes and all tracked
PHY writes so those hypotheses can be separated on hardware.
