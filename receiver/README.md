<div align="center">
  <img src="assets/C5VRX_logo.gif" alt="C5VRX animated logo" width="760" />

  <p><strong>ESP32-C5 Analog 5.8 GHz FPV Receiver</strong></p>
  <p>From live 5.8 GHz FPV video to real-time analog CVBS with one Seeed Studio XIAO ESP32-C5 and a passive resistor DAC.</p>

  <p>
    <a href="https://twotoz.github.io/C5VRX/"><img src="https://img.shields.io/badge/Web%20Flasher-Online-1f6feb?style=flat" alt="Web Flasher" /></a>
    <a href="https://discord.gg/3YNgJRHmzD"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&amp;logoColor=white" alt="Join the C5VRX Discord" /></a>
    <img src="https://img.shields.io/badge/firmware-C5VRX--5-success" alt="C5VRX-5" />
    <img src="https://img.shields.io/badge/chip-ESP32--C5-111111" alt="ESP32-C5" />
    <img src="https://img.shields.io/badge/RF-5.8%20GHz%20(48%20channels)-6f42c1" alt="5.8 GHz" />
    <img src="https://img.shields.io/badge/output-analog%20CVBS%20PAL%2FNTSC-orange" alt="Analog CVBS" />
    <img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="GPL-3.0-only" />
  </p>
</div>

---

Analog 5.8 GHz FPV receiver on one Seeed Studio XIAO ESP32-C5 with a passive
6-bit resistor DAC: live FM video in, composite PAL/NTSC out for goggles.

C5VRX-5 uses one live demodulator. Its receive-chain evidence is recorded in
[docs/FINDINGS.md](docs/FINDINGS.md); the U85Q study it is built on, with
scoring corrections and failed experiments, is preserved in
[docs/pr190-key-findings.md](docs/pr190-key-findings.md) and
[docs/recent-pr-receiver-findings.md](docs/recent-pr-receiver-findings.md). The [IQLaneFusion research](docs/IQLANE_FUSION.md)
is host-only (its read-only firmware shadow `J` was removed 2026-10-12);
it is not a demonstrated video or range improvement.

## What it does

- **U85Q demodulator** in the TX BitScrambler: an untrained 32-phase tracker
  that keeps the picture locked across VTX deviations, off-centre carriers
  and multipath instead of dropping out. Board-validated on real video.
- **Fixed ultrafine IQ** (ADC bits 9,6,5,4) at 40 MS/s, no CPU in the sample
  path.
- **Direct Gain V5**: firmware gain with a zero-write healthy band, fast
  overload handling and a learned, persisted gain map.
- **Narrow analog filter**, measured once per board at startup: the lowest
  noise bandwidth that still passes 12 MHz (plus a 10 MHz class for
  low-deviation VTXs and a 16 MHz class for wide ones; simulation-based,
  board test pending).
- **Calibration only at startup**: anything that interrupts the picture
  runs once after power-on (only what is missing) or from the menu item
  CALIBRATE, never during flight. The goggles show what it does and how long.
- **VTX-matched profile** (narrow for low-deviation VTXs, wide 16 MHz for
  wide ones) **only from a fresh measurement**: every boot starts
  with the default profile; the narrow profile is chosen only when this
  boot measured the VTX in the startup phase. **Continuous VTX matching**
  then measures the deviation passively every 30 s on live video (burst-
  confirmed sync, AFC lock, C/N >= 8 dB, no clipping, held gain). Each
  result is logged (`PROFILE live`), shown in `T` with agree/mismatch
  streaks and feeds the kp ladder's deviation class. With the **live
  switch** on (default, console `A`, NVS `prof_live`) a reliable result
  changes the profile in flight: wider (narrow -> DEFAULT -> WIDE) after one
  result, narrower only after two agreeing results, each through a filter
  relock plus the verified tracker restart (a few ms, shared 5 s cooldown;
  board test pending). `A` off = report only: the result waits as pending
  for the next boot or CALIBRATE.
- **Field diagnostics** (serial `D`, `R`): what is really active, and why
  each dropout happened as far as the receiver can tell
  ([docs/FIELD_TEST.md](docs/FIELD_TEST.md)).
- **DC correction for every gain row, also with the VTX on**: missing rows
  are measured once at startup on a quiet channel, so far-out (high gain)
  reception has no uncorrected DC.
- **Automatic receive-chain care**: per-gain DC correction, DC drift
  tracking, sampling-phase check, AFC that centres the VTX.
- **Simple menu**: band, channel, scan, video standard, calibrate, output
  level, exit.

## Hardware

XIAO ESP32-C5. DAC on D4..D9 (GPIO 23, 24, 11, 12, 8, 9; LSB first) with
8.2k / 3.9k / 2k / 1k / 470R / 240R and a 200R load, as in C5VRX-3/4
([schematic](docs/hardware/c5vrx-dac-schematic.png)). The
receiver uses the ESP32-C5's own Wi-Fi radio; no extra RF parts.

### Wiring the resistor DAC

Six resistors, one per XIAO pin, all joined on one node: that node is the
video output (CVBS). A seventh resistor from that node to GND is the load.

| XIAO pin | GPIO | DAC bit | resistor to the output node |
| --- | ---: | --- | ---: |
| D4 | 23 | bit 0 (LSB) | 8.2 kΩ |
| D5 | 24 | bit 1 | 3.9 kΩ |
| D6 | 11 | bit 2 | 2.0 kΩ |
| D7 | 12 | bit 3 | 1.0 kΩ |
| D8 | 8 | bit 4 | 470 Ω |
| D9 | 9 | bit 5 (MSB) | 240 Ω |
| GND | - | load | 200 Ω (output node to GND) |

```
D4 --[8.2k]--+
D5 --[3.9k]--+
D6 --[2.0k]--+
D7 --[1.0k]--+-----------> CVBS out (goggle / monitor AV-in, video)
D8 --[470R]--+     |
D9 --[240R]--+   [200R]   (optional: 470 pF from the node to GND,
                   |        improvement unproven)
GND ---------------+------> GND (goggle / monitor AV-in ground)
```

- The pin order matters: the largest resistor (8.2k) goes on D4 and the
  smallest (240R) on D9. A swapped pair gives a scrambled, banded picture.
- Connect the output node to the video input (centre/tip) of the goggles,
  a DVR or an analog monitor, and the XIAO GND to its ground. A common
  ground is required.
- All values are standard E24; 1 % metal-film resistors are fine. Keep
  the wires from the pins to the node short.
- Too dark or too dim on some goggles: try OUTPUT in the menu (1.0-2.5x).
- The XIAO is powered over USB-C or its 5 V pin as usual; the DAC needs no
  separate supply.

## First boot

1. Flash (below) and power the receiver, best with the **VTX off**, for up
   to two minutes: the goggles show CALIBRATING with the time left. With a
   VTX on it calibrates the filter on a quiet channel and comes back.
   Results are stored; later boots skip what is already measured. The
   first DC measurement is the template for every channel: switching
   channels needs no new DC search, also with the VTX on. When a boot finds
   the own channel quiet (VTX off), it measures that channel and replaces
   the template rows with its own; tables are kept for six channels.
2. Switch the VTX on. Without a stored channel the receiver starts on
   A1 (5865 MHz); use the menu or scan to find yours.

## Controls (BOOT button)

| live video | in the menu |
| --- | --- |
| short press: next channel in the band | short press: next item |
| hold 0.6 s: open the menu | hold 0.6 s: do the item |

Menu items: **BAND** (next band), **CHANNEL** (next channel), **SCAN**
(all 48 channels, picks the strongest centred analog video), **VIDEO**
(AUTO / NTSC / PAL for the menu picture; AUTO follows the camera),
**CALIBRATE** (on the ground: measure again), **OUTPUT** (1.0-2.5x DAC
level), **EXIT**.
The menu closes by itself after 30 s without a press.

## Flashing

- **Web flasher**: [twotoz.github.io/C5VRX](https://twotoz.github.io/C5VRX/),
  tab **C5VRX-5**, *Full firmware* when coming from another generation.
- **ESP-IDF 6.0.2**: `idf.py build flash` in the repository root. The filter and DC
  correction need the exact libphy of IDF 6.0.2 (checked at build time);
  with another PHY they are disabled and the build warns.
- A running receiver enters USB download mode from the console key `` ` ``
  (`tools/enter_download.py`), so no BOOT button is needed for flashing.

## Serial console (USB, 115200)

`T` status, `D` range/health report, `R` per-second FIELD_DIAG lines on/off
(`tools/field_diag_compare.py`), `I` IQ snapshot
(`tools/capture_iq_snapshot.py`), `o` menu, `c` next channel, `b` next band,
`n`/`x` menu next/select, `s` scan, `F` calibrate again (as the menu item),
`M` GAINMAP lines (for a gain-map preset), `L` bench-only IQ lane comparison (video stops ~3 s),
`` ` `` download mode, `h` help.

`v` cycles the video standard AUTO / NTSC / PAL using the same action as
the on-screen VIDEO item. `e` closes the on-screen menu without opening it
when already closed. Both are direct controls for the web receiver menu.

`K` cycles OFF / AUTO kp / AUTO kp+LP. `k` steps kp calmer, `j` faster;
`P` manually toggles U85Q/U85Q-LP. `P`, `k` and `j` persist AUTO OFF,
including across reboot; repeated presses do not rewrite unchanged NVS.
Fresh settings default to AUTO kp+LP (operator decision 2026-10-12);
an explicitly saved OFF remains OFF. Kp and program changes
use a brief pipeline restart with a fully verified LUT; they are not seamless.
All live changes share a **five-second minimum interval**, including manual
P/k/j, K mode, OUTPUT and live profile changes. AUTO runs on every
profile: a profile sets the filter and the kp it starts from (MATCHED 0.5 /
0.6, else 0.85), the ladder then owns kp and U85Q-LP, with its own curves on
the narrow MATCHED filter. With AUTO OFF the profile's start kp stays.
AUTO also requires the same kp/program target
for **two seconds of usable measurements**. Rapid P presses are refused
with `wait_ms`; they are not queued for delayed programme changes.

Test on the ground and check `T`: `kp_verify_fail`, `prog_verify_fail`,
`kp_restart_fail`, `prog_restart_fail`, transport faults and measured
`kp_restart_us` / `prog_restart_us`. `switch_wait_ms` shows the remaining
cooldown. Live TX uses an owned fixed DMA ring and has a bounded
20-ms readiness wait with task delays, so a failed start cannot keep the
control task spinning indefinitely.

`g` and menu **OUTPUT** cycle 1.0 / 1.5 / 2.0 / 2.5x. Every boot starts at
1.0x. Increasing the level can help the weak sync reported in #200, but can
clip a wide VTX; check both sync and white with the actual 75-ohm load.
Only DAC bits 0..5 are transformed, preserving all phase/token bits across
kp changes, LP changes, menu return and rollback. The menu raster retains
its own levels. `T` reports the setting, clipped **LUT entries** (not an
observed waveform clip rate), recovery errors and a latched failure.

A failed restart cleans up before one attempt at the last verified program
and LUT, including its OUTPUT setting. Failed rollback latches live OFF;
the USB status/download tasks remain available. AUTO stops after a restart
failure and reconciles to the verified configuration instead of chasing the
failed target. This recovery coordinator is fault-injection tested on the
host; actual SDK/peripheral error behaviour and picture acceptance still
need hardware qualification.

Automatic OUTPUT is **research only**: `research/output_level.py` compares
manual scaling with an ideal sync/white-referenced gain and pedestal.
Its reference levels are ideal source measurements, not a proven live
estimator. See `research/data/output_level.json` and the evidence notes in
`docs/FINDINGS.md`. It does not establish HDZero compatibility or range gain.
The follow-up passive `research/output_probe_study.py` uses the production
AFC/Direct Gain helpers; its conservative white reserve and wide-VTX
confidence remain unqualified. Multi-Rescue comparisons against MATCHED
are in `research/rescue_study.py` and `research/data/rescue_study.json`.
The active acceptance list is `docs/PR196_TASK_PLAN.md`.

## Web flasher

[twotoz.github.io/C5VRX](https://twotoz.github.io/C5VRX/) (Chrome, Edge or
another browser with Web Serial) has three tabs:

- **C5VRX-5**: the newest versioned build first, then unmerged PR builds
  (marked, with a confirmation before flashing).
- **Local File**: flash a `.bin` you built yourself.
- **Legacy builds**: archived C5VRX-3 releases, no longer developed.

Firmware is mirrored into the GitHub Pages site and downloaded same-origin.

The **Terminal** view has a visual receiver menu matching the module:
BAND, CHANNEL, SCAN, VIDEO, CALIBRATE, OUTPUT and EXIT. Selecting a menu
page only changes the browser view; its action button sends the command.
It identifies C5VRX-5 from firmware help/status and displays the last reported
band, channel, frequency, signal, video standard, output level, screen-menu
state, scan result and calibration state. It updates by itself: the firmware
sends a short `STATUS` line every 2 s (`control_print_beat`), which the menu
reads and keeps out of the log. Firmware without that beat gets a `T` request
after a command, when a scan or calibration finishes and every 10 s while
idle. The firmware lists also refresh by themselves (every minute and when
the page is shown again), keeping the selected build. VIDEO/EXIT direct actions require
firmware advertising `v`/`e` in its help; older C5VRX-5 builds can still use
the on-screen menu controls. Legacy builds do not enable the C5VRX-5 action
menu. `#terminal` opens the Terminal view directly.

While a status screen owns the picture (first-boot calibration, CALIBRATE,
lane test) the firmware ignores menu keys; it now answers each with
`COMMAND <key> refused=busy` and reports `busy=` in `CONTROL` and the `HB`
heartbeat, and the web menu greys out those buttons until it ends.

Diagnostics and bench controls are collapsed by default. Commands send one
case-sensitive byte without Enter. Commands are sent at once without a
browser popup; what an action changes is shown as the button's tooltip.
Connecting the
terminal releases the flasher, and connecting the flasher releases the terminal;
they never open a port together. **Download log** saves the displayed text,
including FIELD_DIAG and raw IQ captures. The display retains the latest
120,000 characters; download before reconnecting or clearing.

## Releases and CI

`.github/workflows/c5vrx5.yml` runs the host tests and research checks,
builds the firmware and publishes it: `c5vrx5-v5.0.N` for every firmware
change on `main` (immutable), and a mutable `c5vrx5-pr-N` prerelease per
same-repository pull request, removed when the PR closes. Unchanged firmware
inputs (`tools/firmware_input_hash.py`) keep the existing release, so a
documentation or website change does not mint firmware. A successful build
refreshes the Pages mirror (`deploy-web.yml`, always from trusted `main`).

## Repository layout

```text
main/        C5VRX-5 firmware (U85Q, Direct Gain V5, calibration, menu, console)
test/        host tests (CI)
research/    simulations, generators and data behind the design choices
docs/        findings, field-test protocol, task plan, DAC schematic
tools/       console/log tools and the CI/Pages helpers
web/         web flasher (GitHub Pages)
legacy/      archived generations, never built:
             c5vrx1, c5vrx2, c5vrx3 (former root firmware), c5vrx4
             (former experiments/c5vrx-4), legacy-issues
```

The archived generations stay as reference: their findings explain why
C5VRX-5 looks the way it does (see `AGENTS.md` for the research ledger).

## Community

Questions, build photos, feedback, or development discussion? **[Join the C5VRX Discord](https://discord.gg/3YNgJRHmzD)**.

## Credits and license

C5VRX-5 is part of [C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and
the C5VRX contributors; the official website
([twotoz.github.io/C5VRX](https://twotoz.github.io/C5VRX/)) has the project
and its Discord invite. It builds on C5VRX-2..4 (the continuous MODEM_DIAG
to BitScrambler video path, Direct Gain V3-V5, the PAIR schedule and the
U85Q study of PR #190), on zerowidth/C5VRX PR #3 (interference-burst gate,
sampling-phase probe, off-grid channel tuning), on Louis Hitchcock's staged
overload recovery (C5VRX #182/#184), and on ESPARGOS esp-sdr (GPL-3.0) for
the C5 RX filter control. Espressif supplied the GDMA EOF fix
(espressif/esp-idf#19091).

Like the rest of this repository, C5VRX-5 is licensed under
**GPL-3.0-only** (see [LICENSE](LICENSE)). The GPL allows
commercial use, including selling receivers with this firmware, provided
the source (with your changes) is passed on under the same license.

The C5VRX name and logos have separate terms; see [assets/BRANDING.md](assets/BRANDING.md).

