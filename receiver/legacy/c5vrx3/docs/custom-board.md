# Custom ESP32-C5 pinout and experimental 8-bit DAC

This extends C5VRX by **Twotoz and the C5VRX contributors**, based on
`3ff715a1fe79de3501ba9a51030b14671601f1c0`. Source:
[Twotoz/C5VRX](https://github.com/Twotoz/C5VRX),
[official website and Discord invite](https://twotoz.github.io/C5VRX/).
It reuses the root receiver, Phase8 generator and BitScrambler simulator, and
the configurable-output precedent in `legacy/c5vrx1/main/c5vrx_cvbs_out.c`.
The additions are board configuration, pin validation, console selection support,
full-byte Phase8 emission and menu-level adaptation. Existing license notices apply.

## Configuration

Run `idf.py menuconfig` (or `pio run -t menuconfig`) and open
**C5VRX board and video DAC**. Enable **Configure a custom ESP32-C5 board**.
Set the physical GPIO number for each logical IQ/DAC bit, and the active-low
menu button. `-1` disables the button; it is not valid for an active data bit.
Configuration is compiled into firmware, not changed through NVS at runtime.

Use **GPIO numbers**, not connector pin numbers, `D4`, or Arduino `A0` labels.
The video pins are digital outputs. The resistor network sums their currents
to make one analog composite waveform. The IQ pins are digital internal
MODEM_DIAG-to-PARLIO loopbacks, not ADC inputs and not external video inputs.
Leave them unconnected externally. They still consume physical GPIO resources.

| Logical assignment | Unchanged XIAO default | Meaning |
| --- | --- | --- |
| IQ D0..D3 | GPIO1, 0, 25, 7 | Q nibble, low to high significance |
| IQ D4..D7 | GPIO10, 5, 3, 4 | I nibble, low to high significance |
| DAC D0..D5 | GPIO23, 24, 11, 12, 8, 9 | Output LSB to MSB |
| DAC D6..D7 | Unused | Required only for an 8-bit physical DAC |
| Menu button | GPIO28 | Active low with internal pull-up |

Changing an IQ GPIO changes RF routing, PARLIO RX and the optional phase-tap
probe together. It does not change the captured lane order, nibble encoding,
or the automatic coarse/fine/ultrafine lane selection. In six-bit builds,
the experimental 4BIT@80 mode automatically uses configured DAC D2..D5 and
holds D0/D1 low, retaining the original resistor weighting.

Startup validation runs before RF or probes claim pins. It rejects duplicate
assignments across IQ, DAC and the button, invalid/unassigned active pins,
GPIO16..22 reserved here for flash/PSRAM, and active console pin conflicts.
It prints the role, bit and offending GPIO. It cannot check your PCB wiring,
module pin exposure, external loading or reset-time strap levels.

## USB and UART console

The default remains native USB Serial/JTAG. To use GPIO13/14 for the custom
board, disable both the primary USB console and secondary USB console in
**Component config > ESP System Settings**. Select UART or no console.
For a UART console, select its pins there; default UART0 uses GPIO11/12,
which conflict with the default DAC and must be remapped on one side.
The command task polls the native USB FIFO only for the USB console and uses
nonblocking stdin for UART. USB/debug never paces the live sample pipeline.

Disable incompatible external JTAG use as appropriate to your board. Boot
straps and pins wired to flash/PSRAM cannot be freed merely by disabling an
application peripheral. In particular, verify GPIO2/7/25/27/28 reset loading
and the datasheet for the exact module. EN/reset is not a remappable menu GPIO.

## Why eight output bits are possible

These are three distinct quantities:

- **RF input:** eight captured bits, Q4 + I4. This change does not increase RF
  acquisition resolution or make PARLIO RX wider.
- **Transport:** PARLIO TX already uses an eight-bit bus at 40 MHz in the
  ordinary six-bit mode. Two data GPIO entries were simply unused.
- **DAC code:** the current Phase8 worker emits `A10..A15` and discards
  `A8..A9`. The eight-bit worker emits the complete `A8..A15` result twice.

Both versions use the same LUT, eight instruction slots, two bundles per
output pair, 32-KiB raw ring, 16-bit reads/writes and persistent counter state.
Unique CVBS samples remain 20 MS/s, emitted as `[D,D]` at 40 MHz. No CPU
sample conversion, extra DMA bytes or extra processing stage is introduced.
The exact relationship is `code6 = code8 >> 2`; the added low bits carry real
Phase8 result information, rather than just shifting a six-bit value left.
The signed phase-boundary alias remains unchanged.

Select **8 bits** only with the root **Phase8 HR live** option enabled.
The HC NVS selection is ignored in this mode, and command `P` explains why
HC cannot be selected. Golden/HC/4BIT@80 and the isolated C5VRX-4 experiment
are not advertised as eight-bit implementations. The six-bit default keeps
all existing supported mode choices and persisted output-mode values.

The standalone PAL/NTSC menu keeps its existing byte-oriented timings.
Its sync/burst/blanking/UI amplitudes and TX idle level are rescaled from
0..63 to 0..255, rounding to the nearest code, while preparing the menu.
Exiting the menu restores the configured live Phase8 program and GPIOs.
This assumes the redesigned DAC has the same analog full-scale voltage.

Eight bits still fit in **one byte**. Composite video is a sampled voltage,
not an RGB pixel format: this is not a switch from monochrome to colour, nor
a claim of improved RF sensitivity, range, or a guaranteed visible improvement.

## Hardware required

Use eight available output-capable GPIOs plus eight distinct IQ GPIOs.
An eight-bit binary-weighted resistor DAC needs eight weighted branches.
For a resistor-summing network, D7 has the lowest resistance/highest weight;
D0 has the highest resistance/lowest weight. Extra branches are not shorts:
**do not populate future DAC branches with 0-ohm resistors**.

To retain the significance of the original six branches, map their existing
GPIOs to **D2..D7** and add the two weaker branches as **D0/D1**. The example
below illustrates this remapping. It is not a validated PCB pinout.
Recalculate the network with its shunt, receiving load and coupling/filter
components to retain the intended CVBS voltage. At resistive load,
`Vout = Vgpio * sum(bit_i/R_i) / (sum(1/R_i) + 1/Rshunt + 1/Rload)`;
the actual AC-coupled frequency response also matters.

Nominal digital width is not effective analog accuracy. Resistor matching,
GPIO output resistance, simultaneous-switching glitches, layout and the video
load may dominate the two extra bits. Verify amplitudes, monotonicity,
sync, burst and video on an oscilloscope and the intended receiver.

## Example build

`sdkconfig.custom8.defaults` supplies an **illustrative** eight-bit pin map
using GPIO2/6 as the extra LSBs. GPIO2 is a strap: inspect the hardware before
using it. Use a fresh generated configuration when applying a defaults overlay:

```sh
idf.py -B build-custom8 -D SDKCONFIG=sdkconfig.custom8 \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.custom8.defaults" build
```

For an existing PlatformIO configuration, use `pio run -t menuconfig` and
select the same options; defaults overlays do not override values already
stored in its generated `sdkconfig.xiao_c5`.

On Windows, PlatformIO rejects project paths containing spaces. Use a
space-free checkout/build copy. The local validation below used an isolated
space-free copy, leaving the original checkout and its configuration intact.

## Evidence and remaining validation

- Host simulation exhaustively compares all 65,536 raw endpoint pairs:
  all 256 output codes occur; bytes are duplicated; dropping the extra two
  bits exactly recovers the six-bit result; bundle cadence is unchanged.
- Host tests cover pin conflicts, disabled button, flash/console exclusions
  and PAL/NTSC raster timings/amplitudes at both widths.
- ESP-IDF 6.0.1 / pinned PlatformIO builds succeeded for the default XIAO,
  the illustrative eight-bit map with the button disabled, and a six-bit
  UART0-console map using GPIO13/14 for DAC D2/D3 with native USB console
  disabled (Phase8 off, exercising the legacy selectable output paths).
- `tools/validate_build.py`: 212 checks passed. The existing Phase8 and HC
  host regressions passed. The new pinout, eight-bit oracle and eight-bit
  menu tests are included in the validation CI job.
- Physical eight-bit DAC operation, GPIO-dependent timing and video quality
  require hardware testing. Host simulation and successful compilation are
  not evidence of live RF/AV continuity on a new PCB.

Sources: [ESP32-C5 PARLIO](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c5/api-reference/peripherals/parlio/index.html),
[GPIO and reserved pins](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c5/api-reference/peripherals/gpio.html),
the repository's `main/fm_phase8_hr_live.bsasm`, `tools/gen_phase8_hr_live.py`,
`tools/test_phase8_hr.py`, `docs/pr-derived-findings.md`, and legacy CVBS output.
