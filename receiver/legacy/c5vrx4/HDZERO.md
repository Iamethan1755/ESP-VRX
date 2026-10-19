# HDZero goggles and the no-carrier idle raster

C5VRX by Twotoz and the C5VRX contributors. Background: Leon reported that
modern HDZero goggles sometimes showed no video from C5VRX while 14-year-old
analog goggles did. On 2026-10-04 Leon shared an HDZero user's experience
(community report, not a measurement):

- HDZero shows a green screen on signal loss, or when a module sends a signal
  it does not accept. A TBS Fusion module shows green before it has locked a
  signal; it goes away after lock or after opening the module menu.
- Dominator-class goggles digitise and show almost anything. HDZero passes
  the analog signal through a TP2825 decoder, a deinterlacer and a scaler, so
  it is much stricter about the PAL/NTSC specification.
- Module menus are NTSC. NTSC seems slightly more tolerant than PAL.

## What the HDZero firmware does (source reading)

[hd-zero/hdzero-goggle](https://github.com/hd-zero/hdzero-goggle) (GPL-3.0),
revision `adb901d9493a3f3d0ca3b052eaa693e51046dd3a`. The relevant code is
`src/driver/hardware-goggle.c` and `hardware-goggle2.c` (`AV_in_detect`),
`src/core/thread.c` (`thread_peripheral`) and `src/driver/tp2825-goggle*.c`.
This section describes the behaviour; no HDZero code is copied. The status
bits are named only through the masks the firmware uses.

| Behaviour | Detail |
|---|---|
| Poll rate | TP2825 register 0x01 is read once per peripheral pass, about every 100 ms (51 x 2 ms sleeps plus I2C). |
| PAL/NTSC switch | If the decoder reports lock in the other standard on two consecutive polls, the firmware switches the TP2825 mode. Goggles 2 also switches the display timing (720p50/720p60). The lock state is then reset. |
| Lock state | video loss -> search on any H or V/H lock; search -> locked after 10 consecutive polls with V/H lock (~1 s); locked -> search after 5 polls without it; a video-loss bit returns to video loss. Goggles 1 also changes TP2825 register 0x23 (clamp) with this state. |
| Use of "locked" | DVR auto-record start/stop and the video alarm. |
| Default | `analog_format` defaults to NTSC; booting into the analog module uses that setting. |

## What this means for C5VRX

1. **Noise is the worst output for HDZero.** With no carrier, the live
   demodulator outputs receiver noise. A Fatshark shows snow. A TP2825 sees no
   valid sync (green screen) and may briefly report lock in either standard.
   Two such polls switch the goggles' standard, and the display mode with it,
   which must be undone and re-locked when the transmitter appears.
2. **Keep one standard.** The menu raster already follows the live
   standard (AUTO). The last stable live standard is now stored, so the menu
   and the idle raster start in it after a reboot, not in a default.
3. **Amplitude and sync depth** are handled separately: STD150 (1.0 V
   sync-to-white under 75 ohm) and the default-on sync-referenced level servo
   (CVBS_OUTPUT.md, CVBS_LEVEL.md).
4. **Every reboot and TX restart is a video loss** for the decoder, and it
   then takes ~1 s to count as locked again. Settings toggles reboot; avoid
   them in flight.

## Why HDZero drops a live picture (research 2026-10-04)

The goggles show TP2825 output only while the decoder holds H/V lock. The
firmware enables the decoder AGC (`TP2825_REG06 0xB2 // AGC enabled`), so
moderate amplitude errors are normalised. What breaks lock is a sync that is
too small for that AGC, a sync clipped into the rail, or a disturbed or
missing sync. A Fatshark-class sync separator still triggers on all three.
The repository evidence:

| Cause | Evidence | C5VRX-4 status |
|---|---|---|
| Too little amplitude | PR #157: 0.2–0.3 Vpp at HDZero AV-in, no picture; v3.18.1 worked. C5VRX-3 Phase8 FULL is ~0.35 V sync-to-white | STD150, 1.0 V sync-to-white |
| VTX deviation | [Logicenios/C5VRX](https://github.com/Logicenios/C5VRX) (`refactor/phase4-fpga` measurements): Tank II deviation 0.4–0.47× nominal, i.e. -7 dB sync depth with a fixed transfer | Sync-referenced servo `u` (0.32–3.2× gain) |
| Carrier offset (CFO) | 0.150 V/MHz: -1.5 MHz leaves <150 mV sync (`test_cvbs.py`); the fixed transfer has 10 mV sync margin | Servo removes the offset in the phase domain |
| Close-in overload | #158: 73.8 % clipping, HDZero black while a scope locked | G20 severe-overload escape |
| No carrier | Noise; HDZero firmware may switch PAL/NTSC on two polls | Idle raster (below) |
| Weak, damaged sync | Community reports of rolling at weak signal; FM clicks; ambiguous spans emit blanking level inside sync | Sync flywheel (2026-10-05, `SYNC_FLYWHEEL.md`): missing or noisy H and V sync rebuilt on a PLL grid; hardware pending |

**Gap fixed here.** The servo was off whenever native AGC owned the gain, so
native mode, and with it the native acquisition mask, ran the fixed transfer:
no deviation and no CFO correction, exactly the HDZero failure class above.
The servo now also runs under native AGC.
- Its levels are phase-domain and do not depend on RF gain.
- Native acquisitions are outliers that the existing MAD, ambiguity and origin
  limits and the three-window agreement reject.
- While masking, every CPU snapshot decodes Q3 like the program
  (`c5v4_phase_mask`).
- Host: with acquisition-like garbage bursts (100 samples every 1480), 22 of
  26 masked snapshots stay valid, with a correct period and sync depth. The
  servo needs three consecutive consistent ones.

**Not fixable without new work.** Keeping lock on a weak, damaged sync needs
sync regeneration, which the AGENTS invariant parks. A cheap BitScrambler
"hold instead of blanking" for ambiguous spans was examined. It does not fit:
the class is known only one bundle before the DAC byte is formed, and both
counter-op bundles are already occupied.

## Fewer live disturbances (2026-10-04)

The goggle decoder loses lock on brief output disturbances that a Fatshark
ignores. Every live LUT write, gain write and TX owner switch is a potential
disturbance, so their rate is now reduced at the source:

- **Level servo, settled hold** (`cvbs_level.c`).
  - Host test: with ±1-bin evidence jitter the old servo rewrote the live
    even-bank LUT 247 times in 5 s, every 20-ms update. After a pass that
    finds nothing beyond 8 mV it is now "settled": it writes again only for
    an entry more than 24 mV off target, at most every 250 ms.
  - Same test: 0 writes in 5 s of jitter. A real ~30 mV black shift is still
    corrected.
  - A context change or reacquisition unsettles it, so initial and
    gain-step convergence are unchanged.
- **DC recentring:** at least 10 s between decoder-bank rewrites (was 2 s).
- **Radius boost:** opt-in (`y`). It adds gain writes for a benefit an
  earlier lane/radius A/B did not show.
- **Idle raster:** it leaves on a sync, or on two consecutive carrier windows
  instead of one. Each exit holds off the next entry for 5 s, doubling to
  10 s and 20 s for repeated exits; 60 s of live video resets this. A fringe
  carrier can no longer toggle the TX owner every ~2 s.

## TP2825 status and how to read it on the goggles

Register 0x01 (video input status), per the Techpoint TP9950 datasheet, the
family the HDZero firmware masks match:

| Bit | Name | Meaning |
|---|---|---|
| 7 | VDLOSS | 1 = video loss: sync missed for MISSCNT consecutive lines |
| 6 | VLOCK | vertical PLL lock |
| 5 | HLOCK | horizontal PLL lock |
| 4 | SLOCK | colour carrier PLL lock |
| 3 | VDET | video detected |
| 2 | EQDET | in SD mode: 1 = 50 Hz |
| 1 | NINTL | 1 = progressive |
| 0 | CDET | colour carrier detect |

Rockchip's TP2825 driver agrees on bit 7, on "locked = 0x60", and on bit 2 as
PAL (1) / NTSC (0). It also uses register 0x26 as clamp control: 0x01 on
loss, 0x02 when locked.

**How the goggles use it** (`AV_in_detect`, revision `adb901d9`):
- **Goggles 2:**
  - locked = VLOCK + HLOCK (+ VDET); loss = VDLOSS, or no lock/detect at all;
  - SLOCK is not needed;
  - the PAL/NTSC auto-switch fires when VLOCK + VDET are set and **bit 0**
    disagrees with the current mode on two consecutive ~100 ms polls. Each
    switch changes decoder mode and display timing, so it is a visible
    desync;
  - it therefore depends on the colour carrier detection of our burst.
- **Goggles 1:**
  - same lock logic, but the auto-switch uses **bit 2** (50 Hz), with
    HLOCK + VDET and NINTL = 0;
  - it rewrites TP2825 register 0x23 (labelled clamp) on every lock-state
    change.

**Reading it on the goggles (no extra hardware):**
1. Menu Storage → Logging on, then reboot. The app writes
   `/mnt/extsd/HDZGOGGLE.log` on the SD card. Every standard switch logs
   `AV_in_detect -- switch: av_pal = N, rdat = XX` with the raw register;
   Goggles 1 also logs every `Clamp = ..` state change.
2. Optional live view: WiFi page → SSH on, root password set there. Then:
   `while true; do i2cget -y 2 0x44 0x01; usleep 50000; done` (busybox
   i2cget; TP2825 at 0x44 on bus 2).

**Reading the result:**
- `0x68`/`0x78` steady: locked; the problem is downstream, or a short glitch.
- Bit 7 flashing: syncs are being missed, so the sync is too shallow,
  clipped, or the output stalls (gaps in the stream).
- 0x60 dropping while VDET stays: H/V PLL loss, so line timing jumps
  (dropped or inserted samples) or the sync edges are disturbed.
- `switch` lines in the log: PAL/NTSC flapping. On Goggles 2 the trigger is
  bit 0 (carrier detect), so our colour burst. Our burst is ~2 dB (NTSC) to
  ~3 dB (PAL) below nominal from the 75 ns span and hold (sinc²), plus
  quantization noise.

## No-carrier idle raster

`idle_raster.h` (host-tested in `tools/test_idle_raster.c`) and
`idle_raster_service()` in `main/video.c`.

- **Entry.** All of the following must hold for 2 s (40 control windows of
  50 ms):
  - carrier coherence `q_phase < 25`;
  - no valid sync for at least 2 s;
  - the gain owner is in its no-carrier survival state (V5 at the table
    maximum and not settling, or native AGC);
  - TX is live: no menu, lab, sweep or channel scan;
  - the first-boot fixed-BW calibration has already been tried (it needs 3 s
    of live no-carrier listening).
- **Output.** The standalone BT.470 menu raster: sync, equalising/broad pulses
  and colour burst unchanged, in the live, last stable, or selected standard.
  The picture is blanking-level black with one dim line,
  `C5VRX <channel> <MHz> NO SIGNAL`. The level matches the live STD150
  blanking (code 20, about 0.33 V vs 0.31 V), so the decoder clamp barely
  moves.
- **Exit.** On the first window with `q_phase >= 40`, or on any valid sync,
  live video restarts (the normal menu-exit path, a few ms). A weak
  transmitter showing sync fragments never enters the raster: every fragment
  restarts the 2 s count.
- **Gain.** While the raster owns TX, the V5 observers are paused as in the
  menu, so the gain stays at the table maximum. AGENTS requires NO_CARRIER to
  return to the known high-gain survival state, and that is also the most
  sensitive state for detecting a carrier. Native AGC keeps running in
  hardware.
- **Buttons.**
  - Short press: changes channel as in live video, and the raster shows the
    new channel.
  - Long press: opens the menu. Safe Flight still applies.
  - The 3 s recovery hold works.
  - The menu inactivity timeout does not apply to the raster.
- **Labs.** RX-only labs (`=`, `@`, `#`, `$`) run while the raster owns TX.
  Other labs refuse as they do in the menu.
- **Standard memory.** After 20 valid sync windows in one detected standard,
  that standard is stored once in NVS `c5vrx4/last_std`.
- **Failure.** If the menu raster cannot get its DMA descriptors, live video
  continues and the raster retries after 10 s.

| Key | Action |
|---|---|
| `_` | Toggle the idle raster (NVS `c5vrx4/idle_raster`, default on), reboot |
| `!` | `IDLE_RASTER` line: enabled/active, entries/exits/failures, last event, standard, stored last standard |
| `v` | Existing standard mode AUTO/NTSC/PAL; AUTO uses live, then stored |

Opting out (`_`) restores snow, for example for a crash search with
tolerant goggles. Sync fragments keep live video either way, but very faint
carriers below `q_phase` 40 without sync are hidden by the raster.

## Hardware acceptance (pending)

1. HDZero, VTX off: after ~2 s, black with the status line, no green screen,
   no PAL/NTSC toggling; `!` shows `active=1`.
2. Power on the VTX: live picture without a standard switch, and time to
   picture compared with `_` off. Repeat with a PAL camera after one PAL
   session (stored standard).
3. Walk to the range edge: the raster must not appear while any sync is
   visible on a tolerant monitor; record the `IDLE_RASTER` enter/exit lines.
4. Native AGC with the mask, menu open/close from the raster, short/long press,
   Safe Flight and the 3 s recovery, `=` with the VTX off.
5. Fatshark: unchanged live picture; black instead of snow without a carrier.

## Limits

- The raster hides receiver noise only when no carrier is present. Through a
  weak, damaged signal the sync flywheel (`SYNC_FLYWHEEL.md`, AGENTS exception
  of 2026-10-05) keeps the H and V sync valid instead.
- Carrier detection uses the existing coherence metric at maximum gain. The
  thresholds (25 / 40) come from the BW calibration and witness gates and are
  not measured at the range edge.
- The TP2825 status bits and their reaction to the C5VRX waveform are not
  measured here. The firmware behaviour above is from source, not a bench
  log.
