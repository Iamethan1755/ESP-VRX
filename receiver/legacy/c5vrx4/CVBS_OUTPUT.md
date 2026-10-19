# Unwrap75 CVBS output conditioning

Extends Twotoz and the C5VRX contributors' Phase8/Unwrap75,
resistor-DAC and sync investigations. This build combines PR #146's detector
and fixed-ultrafine comparison, PR #154's PHY ownership/range-race fixes,
and main's versioned alpha/PR publication. No Phase5 rollback is included.

## Standard-amplitude default (STD150)

Leon reported that some goggles did not detect the small Phase8 AV signal.
The original PR164 HR100 mapping traded amplitude for offset headroom: nominal
sync depth 0.200 V and sync-to-white 0.667 V. Decoder AGC is not a sufficient
compatibility guarantee, so STD150 replaces HR100 in mode 0, including existing
mode-0 NVS settings. No settings erase is required. Explicit legacy/CVBS150
selections remain unchanged.

The connector target under **one 75-ohm load** is approximately 1.000 V from
sync tip to reference white, with 0.286 V NTSC / 0.300 V PAL sync depth and
0.714 / 0.700 V porch-to-white. This is a waveform span, not a required absolute
DC position; chroma peaks may extend beyond reference white. Sources:

- Analog Devices, Understanding Analog Video Signals, amplitude table:
  https://www.analog.com/en/resources/technical-articles/understanding-analog-video-signals.html
- Texas Instruments, Video Designs Using High-Speed Amplifiers, Appendix B:
  https://www.ti.com/lit/pdf/sloa057
- Analog Devices, sync-depth explanation:
  https://ez.analog.com/video/f/q-a/6845/adv7181d-how-to-clamp-and-sync

STD150 uses 0.310 V blanking and 0.150 V/MHz. Under the existing assumed VTX
reference (-2 MHz sync, +4.667 MHz white), targets are 0.010 V sync,
0.310 V porch, 1.010 V white: 0.300 V sync depth and 1.000 V sync-to-white.
This is 50% more amplitude than HR100; static/history phase decoding and all
three bundles remain unchanged. Six-bit rounding and the real VTX deviation
mean these are targets, not measured compliance for every PAL/NTSC camera.

The existing resistor model reaches only about 1.02 V with its 200-ohm shunt
and one 75-ohm receiver. Thus full amplitude leaves only about 10 mV at each
rail; it cannot also provide HR100's 2.2-MHz offset tolerance. Synthetic CFO
tests explicitly retain this limitation. A wrong carrier centre must be
corrected by tuning/validated AFC, rather than making all video smaller.
No automatic AFC or unverified live LUT servo is enabled by this fix.
A wider physical output range/buffer is needed for substantial rail margin
and colour overshoot at the full amplitude; no hardware change is assumed.

Measure at the goggle connector with a high-impedance scope while the goggles
supply the single 75-ohm load, or use one 75-ohm scope termination without the
goggles. A second 75-ohm termination reduces the amplitude and invalidates the
calibration. Do not calibrate against an unloaded output either.

`M` cycles STD150 -> CVBS150 -> LEGACY_FULL. Mode 0 is STD150, 1 remains
legacy, and 2 remains the original floor-referenced CVBS150 comparison.
The sync-referenced `u` servo is now enabled by default and refines this baseline
when valid sync is measured; disable it for a fixed-transfer M comparison.

## Original CVBS150 change

The detector's phase range no longer sets the video-output slope. STATIC and
HISTORY retain their original phase decode, quadrant trajectory decisions,
counter/parity routing, 3 bundles, IQ40M, and [D,D,D] DAC40M / unique13.333M.
Only the low six DAC bits of the two even LUT planes change.

The CVBS150 transfer (original floor-referenced comparison) uses:

    frequency_hz = delta_bins / (256 * 75e-9)
    target_volts = 0.300 + frequency_hz / 1e6 * 0.150
    code = closest measured-or-nominal loaded DAC voltage

Final voltage saturates to the physical output range. Frequency winding is
resolved first, never by multiplying modulo counter terms. An ambiguous
trajectory maps to the blanking reference. The 0.150 V/MHz slope is an initial
Golden-compatible engineering value, not a universal camera/VTX calibration.
Approximately -2 MHz from reference gives sync near zero; +4.667 MHz gives
white near 1 V. Different VTX deviations/CFO need bench assessment. Strong
colour peaks and tails must be checked for clipping; the present passive
network has limited headroom, so standards compliance is not claimed.

The previous Unwrap75 transfer mapped the full +/-6.667 MHz interval to the
DAC's range and put zero deviation near half scale. It remains an A/B option.
The nominal loaded voltages explicitly include 3.3-V GPIO, all six resistor
conductances, the 200-ohm shunt and one 75-ohm AV load. GPIO impedance,
resistor tolerance, dynamic settling and reconstruction filtering are not
measured by this model.

## Controls

- `M`: cycle STD150 / CVBS150 / LEGACY_FULL and reboot. Default STD150. NVS
  key `c5vrx4/cvbs_legacy` (0 STD150, 1 legacy, 2 CVBS150) is independent of
  HISTORY and gain selection.
- `J`: eight bounded snapshots, 50 ms apart, on a temporary low-priority task.
  No permanent observer and no PHY, gain, LUT, DAC or ring writes.
- `T`: report transfer, nominal/measured calibration, lane and gain ownership.
- `h`: toggle STATIC/HISTORY and reboot. Uppercase `H` remains available for
  PR #154's shared PHY invariant snapshot; it no longer changes the demodulator.
- `Z`: retains the fixed-ultrafine / baseline-lane reboot comparison.

The mapping is selected at boot and every normal live restart/menu exit.
Bounded DAC-only live LUT16 writes are enabled by default; see CVBS_LEVEL.md. The pinned IDF 6.0.2 `bitscrambler_load_lut()`
changes the active LUT width to 32 bits while loading; `load_program()` halts
execution. Neither is a safe seamless in-flight gain actuator for this LUT16
program. Do not turn the diagnostic gain proposal into a live call to either.

## Diagnostic evidence

J copies one completed 4092-byte RX descriptor. It refuses invalid channels,
menu/PHY labs and stale context, checks DMA position stability and a <=50-us
copy duration. Profile, PHY and gain/lane epochs fence the copy. Copying a
completed descriptor is not a sample-gapless capture proof.

The frozen snapshot is decoded as a local stride-3 Phase8/trajectory estimate,
with startup discarded. The actual live alignment/history is not tagged, so
reported codes/volts are **semantic estimates**, not exact DAC bytes or scope
measurements. Sync candidates need negative contrast, 3.9–5.7-us pulse width,
repetition near the PAL/NTSC line period, and robust interior/porch median/MAD.
A five-point majority mask tolerates small quantizer holes only for detection;
level statistics still use the original decoded deltas. Noise, missing pulses,
constant input, strong origin collapse and unrelated pulse widths are rejected.
There is no decoded field/burst lock or validated carrier detector here.

Telemetry separates phase span from nominal output span, origin/ambiguity/rail
occupancy, and raw mean I/Q. Mean IQ is **not** a validated DC estimate: finite
windows and static video can have a real nonzero mean. No offset is subtracted.
Correcting endpoint phase without also correcting the middle-sample quadrant
classifier could invalidate winding; do not silently make that change.

The suggested scale is bounded to 0.75–1.5 and reported only after valid levels.
It does not actuate or persist. A smaller span can represent wrong scaling,
DC/quantization distortion or genuine information loss. Do not normalize noise.

## Scope calibration

Optionally provide `experiments/c5vrx-4/dac_calibration.json` before generating:

    {"load_ohms": 75, "volts_by_code": [64 measured voltages in DAC-code order]}

Measure all codes with one actual 75-ohm input load. The generator rejects a
wrong load, malformed/nonfinite voltages, missing sync reference or <0.95-V
headroom. Generated programs and `cvbs_tables.h` embed the resulting transfer;
T marks it as measured. Rebuild after changing calibration. Static calibration
still does not prove dynamic video bandwidth/settling.

## Acceptance and remaining work

Host checks establish unchanged decoding/routing, saturation, calibrated-table
selection, synthetic PAL/NTSC/CFO levels, the legacy comparison, smaller-phase
telemetry and refusal of tested noise/invalid captures. The 524,386,048-case
trajectory oracle remains applicable independently of the output transfer.

Bench: compare legacy/fixed on the same board, load, VTX, RF channel and lane;
scope sync, porch, white and chroma, then repeat with attenuation. Confirm
HDZero picture/recording, PAL/NTSC, colour/detail, menu exit, native/V5 and
power cycle. Observe FIFO faults and J copy/work timing. No measured range
improvement, PAL/NTSC compliance or HDZero fix is claimed until then.

**Default enabled:** sync-referenced `u` output gain/offset regulation; explicit
opt-out, noise/loss hold, addressing refusal and fault latch are retained.
**Not implemented:** automatic IQ DC correction,
and H/V sync regeneration/coasting. These require a safe hardware actuator,
verified IQ-centering evidence and full field/burst timing respectively. The
previous CPU flywheel was throughput-limited; no unverified raw-ring repair
has been enabled here. Output amplitude is addressed; lost sync information
at the RF threshold is not recovered by this change.

Vendor API source: ESP-IDF v6.0.2
https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_driver_bitscrambler/src/bitscrambler.c
