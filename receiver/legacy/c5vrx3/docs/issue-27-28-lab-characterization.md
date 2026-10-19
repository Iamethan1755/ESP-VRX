# Issue #27/#28 lab characterization

This firmware instrumentation is for controlled hardware A/B work. It does not
claim a measured sensitivity improvement, identify undocumented ESP32-C5 gain
stages, or prove the cause of a visible lag event by software alone.

## Goals

- Build a repeatable map of the production `phy_force_rx_gain()` states used by
  C5VRX without changing the realtime MODEM_DIAG -> PARLIO -> BitScrambler path.
- Separate visible lag/freeze events from PARLIO/GDMA/BitScrambler transport
  faults and from RF/PHY gain-transition disturbances.
- Keep USB output out of the quiet baseline unless the operator explicitly
  requests a marker, snapshot, or diagnostic dump.

## Console commands

| Key | Lab action |
| --- | --- |
| `b` | Enter the quiet baseline: MANUAL gain, forced BW40, AFC OFF / 0 kHz, suppress unsolicited carrier-lock output, wait for setup transients, then clear correlation counters. |
| `g` | Start or abort the fixed-gain sweep over the production states G2..G62 in steps of 2. Each state dwells 1000 ms and is reported after 700 ms. |
| `F` | Run a bounded FFT-scale A/B at fixed RF gain/BW40 using forced values 16, 24, 32 and 40, then restore automatic FFT scaling. This tests whether FFT scaling changes raw MODEM_DIAG Q4/I4 at all. |
| `W` | Run a fixed-gain BW40 -> BW20 A/B using only the already-used `phy_wifi_fbw_sel()` path, then restore the previous bandwidth. |
| `p` | Print one machine-readable `C5VRX_LAB_ROW` using the current state and absolute counters. |
| `r` | Clear lag/transport counters, the event ring, correlation timestamps, and sticky fault state without changing RF settings. |
| `l` | Mark a visible lag/freeze immediately after it is observed. |
| `t` | Dump the Wi-Fi vendor timer inventory captured by the existing OSI wrappers. |
| `q` | Toggle only the unsolicited carrier-lock message suppression. |
| `a`, `s`, `m` | Select ACTIVE, SHADOW, or MANUAL AGC for A/B testing. |

The gain sweep is intentionally a characterization run, so it prints one line
per state. Do not use the sweep itself as the quiet #28 lag baseline.

## Machine-readable row

A row starts with:

```text
C5VRX_LAB_ROW kind=GAIN_SWEEP ...
```

or:

```text
C5VRX_LAB_ROW kind=SNAPSHOT ...
```

The row includes:

- physical gain index and the existing RX gain register snapshot;
- active RF bandwidth, AFC mode, offset, AGC mode/state;
- read-only PHY snapshots for the known RX filter register, ADC-rate register,
  source mux, decoded filter mode and ADC-rate selector;
- FFT force state/value for the dedicated FFT probe;
- `P_median`, `Q_phase`, rail-clipping permille, near-origin permille and the
  current signal-strength score;
- Q4/I4 DC-centre estimates plus I/Q power-skew and cross-correlation metrics,
  so RX DC/IQ quality can be characterized without invoking undocumented
  calibration routines in the realtime path;
- PARLIO TX-empty, RX-overflow and unexpected TX-EOF counts;
- GDMA input/output fault counts;
- BitScrambler FIFO-empty/EOF-overload evidence;
- total lag events, events within 200 ms of a gain write, events within 200 ms
  of any tracked PHY write (gain, bandwidth, frequency-offset/gain reassert, or
  FFT force) and gain-adjacent Q/P collapses;
- age/type of the last PHY write, age of the last gain write and transport
  event plus the last event flags.

The sweep row reports counter deltas for that one gain state. A manual
`p` snapshot reports current absolute counters since the last reset.

## #27 fixed-gain map procedure

Use one VTX/camera/channel/antenna setup for the whole run. A step attenuator or
calibrated RF source is preferred.

1. Close the on-screen menu and leave the normal 6-bit/40 MHz video output in
   place unless that variable is the thing being tested.
2. Run `g`. Do not change channel, antenna, VTX power, scene or attenuation
   during a single sweep.
3. Capture the serial `C5VRX_LAB_ROW kind=GAIN_SWEEP` lines.
4. Repeat at strong, medium, near-threshold and VTX-off/noise conditions.
5. Repeat the same attenuation points rather than comparing uncontrolled
   walk-test distances.
6. On the scope, correlate each one-second gain state with CVBS disturbance and
   note any gain transition that causes a lock/re-lock event.

The first map covers the even gain states C5VRX production currently uses.
Expanding to undocumented or unused gain states should be a separate experiment
with evidence for their validity.

The useful result is a table showing where additional gain improves carrier
coherence versus where it only raises rail clipping/noise. The extra PHY
register fields should be diffed between adjacent gain states; discontinuities
are candidates for internal gain-stage boundaries, but they are not named
"LNA" or "BB" until hardware/register evidence proves that mapping. Do not
interpret a larger raw code or gain register value as improved sensitivity by
itself.

## FFT-stage placement probe

Run `F` with a stable VTX, fixed attenuation and fixed scene. The probe keeps
the RF gain constant, forces FFT-scale values 16/24/32/40, and reports the same
raw Q4/I4 metrics at each value.

If `P_median`, the Q4/I4 DC/IQ statistics and raw quality metrics remain
statistically unchanged while FFT gain changes, that is evidence that the FFT
scaling stage is downstream of the MODEM_DIAG source used by C5VRX. In that
case it must not become part of the range controller. A visible CSI effect is
not sufficient; the acceptance criterion is an effect on this receiver's raw
Q4/I4 path.

## Receive-filter probe

Run `W` at a fixed gain and attenuation. It compares BW40 and BW20 using the
known production bandwidth primitive only. Record both image quality and the
machine-readable Q4 metrics. A lower noise level is useful only if carrier
coherence/video bandwidth is not damaged.

The ROM also exports names such as `phy_chan_filt_set()` and
`phy_rx_filter_mode()`, but this PR does not call them: their ESP32-C5 ABI,
valid arguments and physical filter response are not yet proven.

## RX profile A/B

The RF menu has two hold durations on the RF page:

- normal long press: cycle BW40 / BW20 / AUTO;
- hold for about 2 seconds: cycle BALANCED / RANGE EXP / BLOCKER EXP /
  RECOVERY / AUTO EXP / ARC / FUSION EXP / RANGE V2.

For controlled comparisons, keep channel, antenna, VTX scene and attenuation
identical. Reset evidence with `r` before each profile and mark a visible
freeze with `l`.

`AUTO EXP` is evidence-gated: FFT optimization stays dormant until the `F`
probe reports a material raw-Q4 improvement during the same boot.
The old `HW AGC EXP` profile was removed after its one-argument
`phy_agc_max_gain_set()` declaration proved ABI-invalid. `H` now prints the
read-only ARC vendor table/filter/ADC/IQ oracle and performs no PHY writes.

## #28 quiet lag baseline

Use `b` once. The ready line confirms:

```text
MANUAL gain + BW40 + AFC OFF + quiet output + reset counters
```

After `C5VRX_LAB_BASELINE_READY`, do not touch the console during the soak.
When a visible freeze occurs, press `l` once as soon as practical, then use
`p` or `d` after the event.

Interpret the evidence conservatively:

- transport flag/event at the same event: investigate PARLIO/GDMA/BitScrambler;
- transport clean but a gain-adjacent Q/P collapse: investigate the RF/PHY gain
  transition and malformed CVBS/sync;
- fixed MANUAL baseline, no transport event, malformed CVBS on a scope:
  investigate acquisition/Phase5/sync;
- valid continuous CVBS during the visible freeze: investigate the downstream
  decoder/display re-lock path.

For ACTIVE vs SHADOW vs MANUAL, configure the desired mode, press `r`, and run
the same physical setup. Keep BW40 and AFC OFF unless bandwidth or AFC is the
explicit test variable.

Production TRACK is now intentionally a PHY-write-free state for automatic
control: AUTO bandwidth does not switch while TRACK is active, and AUTO AFC
does not retune while TRACK is active. AFC acquisition and experimental
bandwidth changes are confined to SEARCH/LEARN. This turns a clean carrier lock
into a stable RF state rather than continuously optimizing it.

## Vendor timer correlation

Use `t` outside the quiet soak to record the closed-source Wi-Fi timer
inventory. A matching cadence is a reason to investigate a timer callback, not
proof that the timer caused the lag.

## What remains hardware-only

This PR intentionally leaves these conclusions open until measured:

- which C5 gain indices correspond to LNA/baseband stage changes;
- whether FFT gain affects the raw MODEM_DIAG source (the new `F` probe is
  designed to answer this);
- which gain indices correspond to distinct RF/baseband stage tuples;
- which gain state gives the best equivalent-video RF threshold;
- whether any safe narrower filter state exists beyond the coarse BW20/BW40
  control;
- whether RX DC/IQ calibration improves weak-signal Q4 phase coherence and can
  be run safely only at startup/unlock;
- whether any gain write causes a physical CVBS disturbance;
- whether a visible lag event is transport starvation, malformed sync, or
  downstream decoder re-lock.

That evidence should be attached to issues #27 and #28 before changing the
production gain-state table or declaring the lag root cause fixed.
