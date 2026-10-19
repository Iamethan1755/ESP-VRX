> **Archived (2026-10-12).** C5VRX-3 was the root firmware until C5VRX-5
> replaced it. This tree is kept as reference and is no longer built; its
> paths, build files and tool commands are left as they were. The current
> firmware is at the repository root ([README](../../README.md)); legacy
> C5VRX-3 releases stay available under **Legacy builds** in the web flasher.

<div align="center">
  <img src="assets/c5vrx-phase8-logo.png" alt="C5VRX Phase8 logo" width="760" />

  <p><strong>ESP32-C5 Analog 5.8 GHz FPV Receiver</strong></p>
  <p>From live 5.8 GHz FPV video to real-time analog CVBS with one Seeed Studio XIAO ESP32-C5 and a passive resistor DAC.</p>

  <p>
    <a href="https://twotoz.github.io/C5VRX/"><img src="https://img.shields.io/badge/Web%20Flasher-Online-1f6feb?style=flat" alt="Web Flasher" /></a>
    <a href="https://discord.gg/3YNgJRHmzD"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&amp;logoColor=white" alt="Join the C5VRX Discord" /></a>
    <img src="https://img.shields.io/badge/status-production%20proven-success" alt="Production proven" />
    <img src="https://img.shields.io/badge/chip-ESP32--C5-111111" alt="ESP32-C5" />
    <img src="https://img.shields.io/badge/RF-5.8%20GHz%20(48%20channels)-6f42c1" alt="5.8 GHz" />
    <img src="https://img.shields.io/badge/output-analog%20CVBS%20NTSC-orange" alt="Analog CVBS" />
    <img src="https://img.shields.io/badge/architecture-Zero--EOF%20Circular%20GDMA-blueviolet" alt="Zero-EOF GDMA" />
    <img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="GPL-3.0-only" />
  </p>
</div>

---

## C5VRX-5

[`v5/`](v5/README.md) is C5VRX-5: the untrained U85Q demodulator (board-validated
on real video), fixed ultrafine IQ, Direct Gain V5, a per-board narrow analog
filter and a simple band/channel/scan menu, without experimental options.
Flash it from the **C5VRX-5** tab of the web flasher. Findings:
[`v5/docs/FINDINGS.md`](v5/docs/FINDINGS.md).

## Current firmware

The `main` build defaults to the live adjacent Phase8 demodulator and
**Direct Gain V5** (Direct Gain V4 below plus a 200 us observer and anti-hunt damping), the default and recommended gain controller. Phase8 maps
the full signed adjacent phase delta (-128 through +127) to the 6-bit DAC. A
transition across the +/-180 degree phase boundary can still alias.

Direct Gain V4 is the Direct Gain V3 core (centered Q4 amplitude and Phase8
coherence, physical RF/BB/Fine tuples, zero-write hold while healthy) with two
changes: it acts on the first 1 ms window that leaves the healthy band and
jumps straight to the predicted gain (about 1-2 ms instead of tens to hundreds
of ms), and without a carrier it listens at the gain table's maximum instead of
parking at G62, so weak carriers at the range edge are found.

**Native hardware AGC** remains selectable (RF page, hold BOOT 2 s through the
profile cycle, or serial `N`; applied with a reboot) but is not the default.
Measurements on PR #122 ([native-agc-v2.md](docs/native-agc-v2.md)) show the
ESP32-C5 packet AGC re-acquiring on a continuous carrier every ~21 us, one to
two times per video line, landing on a different gain each time (Q4 radius
41-266 codes for a static VTX). That is the source of the thin black/rainbow
lines and the grain seen with native AGC; finer IQ lanes, register tuning and
paced (timer-gated) native tracking did not remove it. A firmware controller
that holds one gain is the better fit for continuous analog FM.

Bench status: Direct Gain V4 gave a clean picture and very fast response on the
XIAO ESP32-C5 with the VTX static and moving. Range-edge recovery, near-VTX
overload and long sessions still need dedicated tests.

## Demodulator and gain notes

- [PR #190/#192 receiver findings](../../docs/pr190-key-findings.md) preserves selected
  C5VRX/Twotoz/contributor research, scoring corrections, board observations and
  failed experiments with pinned sources; it does not change firmware defaults.
- [Other recent PR findings](../../docs/recent-pr-receiver-findings.md) adds DC/gain
  recovery, build-identity corrections, AFC placement and direct-IQ timing research.
- [Direct Gain V3 design](docs/direct-gain-v3-core.md) describes the Q4
  observer, gain model, settle logic, and known hardware tuning limits that
  Direct Gain V4 builds on.
- [Native AGC findings](docs/native-agc-v2.md) (PR #122): per-sample AGC state
  from the RF dump, the ~21 us re-acquisition cycle, register sweep, DAC
  self-noise, level-offset and paced-native trials, and why none became the
  default. [Paced native AGC](docs/native-agc-paced.md) and the
  [pre-native noise audit](docs/pre-native-noise-audit.md) complete it.
- [Direct Gain V3 measurement oracle](docs/direct-gain-v3-oracle.md) documents
the guarded USB measurements and their interpretation.
- [Range and demod quality](docs/range-demod-quality-v2.md) records the
  measurement model and validation limits for CVBS sync and demod quality.
- [Range v2](docs/range-v2.md), [range notes](docs/range-v2-knowledge.md),
and [trajectory v2](docs/trajectory-v2.md) are research references; their
experimental ideas are not all part of the default live path.
- [Pre-Q4 lab](docs/pre-q4-lab.md) documents explicit RF/DAC noise checks and
vendor gain-table characterization. Those lab actions are not run as part of
normal video reception.

## Web Flasher (Zero-Install Browser Flashing)

Flash your Seeed Studio XIAO ESP32-C5 directly from your browser (Google Chrome, Microsoft Edge, Brave, Opera) with zero installation required:

**[Open the C5VRX Web Flasher](https://twotoz.github.io/C5VRX/)**

The production web flasher is hosted entirely by **GitHub Pages** at
[twotoz.github.io/C5VRX](https://twotoz.github.io/C5VRX/). There is no VPS,
application server, or separate production web host in the current deployment.

- **Automatic Latest Firmware**: Automatically selects the newest immutable semantic-version release.
- **One-Click Flashing**: Flashes the universal merged production image (`bootloader + partitions + app` at `0x0`) over Web Serial. Firmware is mirrored into the GitHub Pages artifact and fetched same-origin, so browser CORS redirects are not part of the production flash path.
- **Selectable Releases**: The **Releases** tab contains only semantic-version releases such as `v3.0.0`, `v3.0.1`, and newer.
- **Separate PR Builds Tab**: Same-repository pull requests publish a temporary `pr-<number>` GitHub prerelease. Experimental builds appear only under **PR Builds**, never in the normal Releases list, and require an explicit warning confirmation before flashing.
- **Offline / Local Execution**: You can also run the web flasher locally:
  ```bash
  python tools/open_webflasher.py
  ```
  *(Starts a local HTTP server at `http://localhost:8080/web/index.html` and opens your browser.)*

### Automatic releases and versioning

Every push or merged PR to `main` builds the production firmware and publishes a new immutable GitHub release with a semantic version tag.

- `BREAKING CHANGE` or a conventional-commit `!` creates a **major** bump.
- `feat:` / `feat(scope):` creates a **minor** bump.
- `fix:`, `chore:`, `docs:`, and other changes create a **patch** bump.
- If no stable release exists yet, the current `v3.0.0-rc1` line is promoted to `v3.0.0`.
- The resolved version is written to `version.txt` before the ESP-IDF build, so the firmware metadata and GitHub release use the same version.
- Release assets remain attached to their immutable version tag; the old mutable `main` release/tag is retired automatically after the first versioned release succeeds.

### Website deployment

Firmware releases and website deployment are intentionally separate:

- `.github/workflows/build.yml` validates and builds firmware, then publishes a versioned release after a successful push to `main`. For same-repository PRs it also maintains a clearly marked temporary prerelease (`pr-<number>`) and removes it when the PR closes.
- `.github/workflows/deploy-web.yml` is the **only production website deployment**. It always checks out trusted `main`, then packages the web flasher plus a generated firmware mirror into one GitHub Pages artifact.
- `tools/prepare_pages_site.sh` mirrors the newest semantic firmware releases and all active `pr-<number>` prereleases under `firmware/<tag>/`, and generates `firmware/releases.json`.
- The browser reads that Pages manifest and downloads binaries from the same `twotoz.github.io` origin. This avoids GitHub Release storage CORS redirects entirely.
- Successful Production CI triggers a Pages mirror refresh, so newly published/updated PR builds and releases become available without deploying unmerged PR web code.
- When a PR closes or merges, CI deletes its temporary prerelease/tag; the next successful cleanup-triggered Pages refresh removes it from the mirror.

---

## Community

Questions, build photos, feedback, or development discussion? **[Join the C5VRX Discord](https://discord.gg/3YNgJRHmzD)**.

---

## What is C5VRX?

**C5VRX-3** turns the **Seeed Studio XIAO ESP32-C5** (ESP32-C5 RISC-V SoC) into a standalone 5.8 GHz analog video (FPV) receiver.

It captures raw Wi-Fi PHY I/Q samples from the 5 GHz RF front-end at 40 MS/s, demodulates wideband FM in the ESP32-C5 **BitScrambler**, and outputs analog CVBS through **PARLIO TX** and a 6-bit passive resistor DAC ladder. The video-standard detector supports PAL and NTSC timing.

```text
5.8 GHz Analog FPV (48 Channels)
        │
        ▼
ESP32-C5 RF / MODEM_DIAG Bus (40 MS/s Q4/I4)
        │
        ▼
PARLIO RX @ 40 MS/s (POS sample edge, pure continuous hardware GDMA)
        │
        ▼
Circular GDMA Ring (32 KiB in HP SRAM, Zero-EOF patched)
        │
        ▼
Phase8 adjacent-demod BitScrambler (fm_phase8_hr_live.bsasm: full signed delta, 20 MS/s [D,D])
        │
        ▼
PARLIO TX @ 40 MHz ([D,D] mode -> 20 MS/s unique CVBS output)
        │
        ▼
6-bit Resistor DAC Ladder + 470 pF Filter -> 75-ohm Goggles
```

The continuous pixel path runs in AHB GDMA, BitScrambler, and PARLIO TX. A background CPU task observes completed IQ buffers and controls receiver settings; it does not move individual video pixels.

---

## Key Innovations & Architectural Highlights

### 1. The Breakthrough: Zero-EOF Circular GDMA
- **The Problem**: In continuous loop mode, the stock ESP-IDF PARLIO TX driver injected a GDMA EOF (`suc_eof = 1`) on every cyclic ring wrap (confirmed in [espressif/esp-idf#19091](https://github.com/espressif/esp-idf/issues/19091)). This triggered periodic hardware stalls, causing a 1-second vertical sync drop and jagged horizontal line jitter ("kartels").
- **The Solution**: C5VRX-3 patches `dw0.suc_eof = 0` across the descriptor ring in SRAM after driver initialization, paired with 64-byte aligned cache synchronization (`sync_dma_c2m`).
- **The Result**: Removing the cyclic EOF markers eliminated the observed wrap seams, periodic vertical drops and horizontal jitter in the tested live pipeline. This addresses transport boundaries; RF noise and demodulation artifacts remain separate quality limits.

**Public report and Espressif confirmation:** Twotoz reported the continuous-stream problem in [espressif/esp-idf#19091](https://github.com/espressif/esp-idf/issues/19091) on **16 September 2026**, with a live-hardware A/B comparison. On **17 September**, Espressif's maintainer [confirmed: “Your analysis is correct.”](https://github.com/espressif/esp-idf/issues/19091#issuecomment-5712811733) and supplied a driver patch. The maintainer explained that BitScrambler interprets cyclic `suc_eof` as a real stream boundary: a steady-state loop should have no EOF, while buffer switching still needs a one-time EOF notification.

This records C5VRX's concrete contribution to a continuous ESP32-C5 RF → IQ → hardware WBFM → CVBS pipeline. The issue concerns **PARLIO/GDMA/BitScrambler stream continuity**, not finite `adctrig` capture or proof that every RF acquisition path is gapless. The earlier autonomous RF-writer and live NTSC milestones are recorded separately in [continuous IQ hardware findings](docs/continuous-iq-findings.md).

### 2. Direct Gain V5 default gain controller
- V5 = V4 plus a 200 us observer cadence (timer-driven, each completed RX descriptor measured at most once) and anti-hunt damping: two direction reversals of consecutive writes within 20 ms make out-of-band decisions need 8 windows (~1.6 ms) for 200 ms. Saturation is never damped.
- **Range lanes:** once the analog gain is at the table maximum and the envelope is still starved, V5 switches the MODEM_DIAG taps to finer ADC bit sets: fine {9,7,6,5} (+6 dB) and ultrafine {9,6,5,4} (+12 dB). Inside their window these are an exact 2x/4x rescale of the coarse nibble (same angle, same Phase8 LUT, no calibration), and the analog gain trims between the 6 dB steps, so total gain is continuous. Without a carrier it listens on ultrafine. Rail codes or incoherent wide junk on a finer lane (the pre-fold warning / a folded strong carrier) return to coarse at once, and lanes are always the first gain removed.
- The fast observer measures centered Q4 P50/P90/P95, phase coherence, clipping, and origin occupancy from completed RX buffers.
- V4 is the sole automatic gain writer in the default profile. It chooses physical RF/BB/Fine gain tuples and verifies each write after its measured settle time.
- Direct: the first window with P50 below 13 or above 32 triggers the full predicted correction in one step; saturation takes an immediate emergency drop.
- Healthy measurements (P50 13-32) produce a zero-write hold, so a steady carrier gets no gain writes.
- Without a carrier it listens at the table maximum rather than the G62 survival gain, so a quantizer-starved weak carrier is not mistaken for no carrier.
- Native hardware AGC is an opt-in alternative (see above).

### 3. Default RF settings
- The default Direct Gain V4 profile uses BW40 and AFC off. This keeps gain as the changing RF control during normal operation.
- Other receiver profiles and lab modes remain selectable over the serial console. Some experimental profiles can use automatic bandwidth or AFC while acquiring a carrier.

### 4. Phase8 adjacent demodulator
- `fm_phase8_hr_live.bsasm` uses adjacent I/Q samples at 20 MS/s and maps the full signed phase-delta range (-128 through +127 bins) across the 64 DAC levels.
- The mapping preserves direction across the signed range. A phase step that crosses the +/-180 degree representation boundary remains ambiguous and can alias.
- Phase8 is selected by default in the current build and was viewed live with Direct Gain V4.

### Known limits
- A signed adjacent-phase estimate cannot distinguish an actual step beyond 180 degrees from its wrapped equivalent.
- The hardware observation documented above covers a working live picture at the tested VTX and receiver setup. It does not establish performance at all distances, channels, antenna orientations, or gain transitions.

---

## Hardware Pinout (Seeed Studio XIAO ESP32-C5)

Connect a 6-bit binary-weighted resistor DAC ladder to the XIAO pins, meeting at the `VIDEO` node:

| XIAO Pin | ESP32-C5 GPIO | Bit Weight | Series Resistor |
|:---:|:---:|:---:|:---:|
| **D4** | GPIO 23 | Bit 0 (LSB) | 8.2 kΩ |
| **D5** | GPIO 24 | Bit 1 | 3.9 kΩ |
| **D6** | GPIO 11 | Bit 2 | 2.0 kΩ |
| **D7** | GPIO 12 | Bit 3 | 1.0 kΩ |
| **D8** | GPIO 8  | Bit 4 | 470 Ω |
| **D9** | GPIO 9  | Bit 5 (MSB) | 240 Ω |
| **GND** | GND | Ground | Ground reference |

![6-bit resistor DAC schematic: D4..D9 through 8.2k, 3.9k, 2.0k, 1.0k, 470 and 240 ohm to CVBS_OUT, with a 200 ohm shunt to GND and an optional 470 pF capacitor to GND](docs/hardware/c5vrx-dac-schematic.png)

`CVBS_OUT` in the schematic is the `VIDEO` node. C3 is the 470 pF output filter from item 2 below; the drawing marks it optional because its improvement is unproven.

### Output network and controls
1. **Video level**: The reference circuit uses a 200 ohm shunt at `VIDEO`; the connected display or goggles may add their own 75 ohm termination. Check the resulting level with the load you use.
2. **Output filter**: The reference circuit uses a 470 pF ceramic capacitor from `VIDEO` to `GND`. Check image sharpness with your display and termination.
3. **BOOT button**: A short click switches channel. A long press opens the menu when enabled; short clicks move through menu choices and a long press applies one. On the CHANNEL page, a long press scans the available channels and selects the strongest coherent carrier. If Safe Flight mode blocks the menu, hold BOOT for three seconds to restore the default Phase8/6BIT@40/Direct Gain profile and open the recovery menu.
4. **Persistent settings**: Channel, RF bandwidth, AFC, video-standard/output, AGC/gain, and the BOOT-menu preference are saved and restored after restart.

---

## Interactive Serial Console Hotkeys

Connecting to the USB serial console (115200 baud) provides live telemetry and single-key controls:

| Key | Action |
|:---:|:---|
| `c` / `C` | Cycle FPV channel / band (48 standard channels: RaceBand, Boscam A/B/E, FatShark, LowBand) |
| `+` / `-` | Manual RF gain step (±2 index) |
| `a` / `s` / `m` | Switch AGC mode: **Active** (auto-adapting) / **Shadow** (dry-run) / **Manual** (fixed) |
| `f` | Cycle AFC Mode: **Auto Centering** (±1.5 MHz) / **Hold** / **Off** (0 kHz) |
| `,` / `.` | Fine-tune carrier frequency offset in ±50 kHz steps |
| `0` | Reset frequency offset to 0 kHz |
| `e` | Toggle RX sample clock edge (POS / NEG) |
| `d` | Print the live diagnostics summary and available console commands |
| `R` | Run the guarded RSSI and centered-Q4 measurement probe |
| `D` / `I` / `Y` | Select Direct Gain V4 / Direct Gain V1 / ARC V3 profiles |
| `N` | Toggle native hardware AGC (opt-in) / Direct Gain V4 and reboot |
| `E` | Print one P8ENV row (Q4 envelope, native AGC state, transport) |
| `Q` / `T` | Raw Q4/I4 dump (4 x 64 consecutive samples) / read-only AGC register dump |

---

## Build & Flash Guide

### Prerequisites
- **Option A (Docker - Recommended)**: Docker Desktop or Docker engine installed.
- **Option B (Native ESP-IDF)**: [ESP-IDF v6.0.x](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/get-started/) installed with Python 3.10+.
- **Option C (PlatformIO)**: Python 3.10+ and the pioarduino core fork (`pip install pioarduino`).

---

### Step 1: Build the Firmware

#### Option A: Build via Docker (Zero-Install Toolchain)
No ESP-IDF installation required on your host machine. Run from the repository root:

**Linux / macOS / Git Bash:**
```bash
docker run --rm -v "${PWD}:/workspace" -w /workspace espressif/idf:v6.0.2 idf.py build
```

**Windows PowerShell:**
```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace espressif/idf:v6.0.2 idf.py build
```

#### Option B: Build with Native ESP-IDF v6.0+
If you have ESP-IDF installed locally:

**Linux / macOS:**
```bash
. $IDF_PATH/export.sh
idf.py build
```

**Windows (ESP-IDF PowerShell Environment):**
```powershell
export.ps1
idf.py build
```

#### Option C: Build with PlatformIO
The optional `xiao_c5` environment builds C5VRX-3 from `main/`; C5VRX-4 continues to use its isolated ESP-IDF build. This build environment was contributed by [KonradIT](https://github.com/KonradIT) in [PR #44](https://github.com/Twotoz/C5VRX/pull/44).

`platformio.ini` describes the ESP-IDF project, so a plain checkout builds without installing a
toolchain first. The official `platformio/espressif32` platform supports neither the ESP32-C5 nor
ESP-IDF 6, so the project pins the community [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform at a fixed commit, which requires the pioarduino core fork of PlatformIO:

```powershell
pip install pioarduino
pio run -e xiao_c5
```

On Windows run `pio` from PowerShell or cmd: the ESP-IDF tool installer refuses to run under
MSYS/Git Bash. The first build downloads the toolchain and ESP-IDF v6.0.1 (a few hundred MB) into
`~/.platformio`; `tools/pio_idf_toolchain.py` then selects the exact `riscv32-esp-elf` version that
ESP-IDF 6.0 requires, which is newer than the one the platform ships.

The images land in `.pio/build/xiao_c5/` (`bootloader.bin`, `partitions.bin`, `firmware.bin`, plus a
merged `firmware.factory.bin`). All receiver parameters still come from `sdkconfig.defaults`;
PlatformIO writes the generated configuration to the git-ignored `sdkconfig.xiao_c5`, so delete that
file after editing the defaults. To flash, install `esptool` and `pyserial` in the Python environment used to run the script (`python -m pip install esptool pyserial`), then use `python tools/flash_pio.py` (see Step 3, Option E).

The Docker and native ESP-IDF builds produce three critical binaries in `build/`:
- `build/bootloader/bootloader.bin` (at flash offset `0x2000`)
- `build/partition_table/partition-table.bin` (at flash offset `0x8000`)
- `build/c5vrx3.bin` (at flash offset `0x10000`)

---

### Step 2: Validate the firmware sources
CI runs the architectural validator and host demodulation checks. To run the repository validator locally:
```bash
python tools/validate_build.py
```
*(All architectural checks must pass.)*

---

### Step 3: Flash to ESP32-C5

#### Option A: Web Flasher (Zero-Install In-Browser Flasher)
Launch the web flasher directly in your browser (Google Chrome, Microsoft Edge, Brave):
**[Open C5VRX Web Flasher on GitHub Pages](https://twotoz.github.io/C5VRX/)**

#### Option B: Zero-Friction Auto-Flash (Python Watcher)
Run the auto-flash watcher:
```bash
python tools/auto_flash.py
```
*Plug in or reset your Seeed Studio XIAO ESP32-C5 into download mode (hold BOOT while tapping RESET), and the watcher will detect the COM port, flash the firmware, and automatically trigger a watchdog reset into the application!*

#### Option C: Direct Flash Script
Specify your COM port (or omit to auto-detect):
```bash
python tools/flash.py COM10
```

#### Option D: Native ESP-IDF Flasher
```bash
idf.py -p COM10 flash
```

#### Option E: PlatformIO Build (Manual Download Mode)
Flashes the images from `.pio/build/<env>/`:
```bash
python tools/flash_pio.py [ENV] [PORT]
```
The XIAO's USB-Serial/JTAG auto-reset is unreliable while C5VRX firmware is running, so the script
writes the flash straight away if the chip is already in the ROM bootloader, and otherwise asks you
to unplug the board, hold BOOT, plug it back in and release BOOT. It always writes DIO at 80 MHz
(the XIAO's flash does not boot in QIO) and never asserts DTR/RTS. `pio run -t upload` works too
when the board is already in download mode.

---

### Step 4: Interactive Serial Monitor & Diagnostics
Launch the dedicated low-latency serial monitor:
```bash
python tools/monitor.py COM10
```
Use the interactive hotkeys (`c` to cycle channels, `+`/`-` for manual gain, `a` for active AGC, `d` for hardware diagnostics).

---

## Repository Structure

```text
C5VRX/
+-- main/
|   +-- video.c, video.h             # Video pipeline, gain controller, menu, console
|   +-- fm_phase8_hr_live.bsasm      # Default live Phase8 demodulator
|   +-- direct_gain_v3.c, .h         # Default automatic gain controller
|   +-- phase8_gain_lut.h            # Generated Phase8 coherence/gain lookup
|   +-- rf.c, rf.h                   # ESP32-C5 RF and PHY controls
+-- assets/                          # Project branding
+-- docs/                            # Design notes and hardware measurements
+-- tools/                           # Build checks, generators, and flash utilities
+-- web/                             # Browser Web Serial flasher
+-- sdkconfig.defaults               # ESP-IDF defaults
+-- partitions.csv                   # Flash partition table
+-- platformio.ini                   # Optional PlatformIO build of C5VRX-3
```

---

## License

C5VRX is open-source software licensed under the **GNU General Public License v3.0 only** (`GPL-3.0-only`).

See [LICENSE](LICENSE) for full licensing terms. The C5VRX name and logos have separate terms; see [assets/BRANDING.md](assets/BRANDING.md).
