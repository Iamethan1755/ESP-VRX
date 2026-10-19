# C5VRX-4 sync flywheel

C5VRX by Twotoz and the C5VRX contributors.

This extends the C5VRX-3 flywheel on branch `feat/sync-flywheel` (a PLL line
tracker that repairs broken pulses in the raw ring before the TX read). The
detection, synthesis, vertical interval and re-lock are new for the C5VRX-4
span75 pipeline.

**Goal (operator decision 2026-10-05).** The goggles must never see a moment
without valid PAL/NTSC sync: never "no sync", never the other standard.
HDZero's TP2825 drops lock after ~5 polls (~0.5 s) without V/H lock, and
switches standard after 2 polls with lock in the other one (`HDZERO.md`).

The live path still recovers the transmitted waveform. The flywheel only
supplies the sync pulses the RF lost. AGENTS.md records the exception for
C5VRX-4. Hardware acceptance is pending.

## How it works (`sync_flywheel.c`)

**Detection without demodulation.**
- The 75 ns endpoint phase step is `phase[raw[k]] − phase[raw[k−3]]` (Phase8
  bins per span). Sync is about −38 and blanking about 0.
- It is averaged over three spans for detection, and checked span by span
  (what the TX decodes) before a pulse is kept.
- Each evaluation is a few table lookups. It is valid for any span alignment,
  because the TX grouping is not tagged.
- Host model: ~32 evaluations per clean line, ~50–65 at a weak signal or in a
  fade.

**Line tracking.**
- Acquisition: a step histogram over ~8 lines gives the threshold. Two
  sync-width runs one line apart give PAL (2560 samples) or NTSC (2542.2).
- PLL: phase gain 1/4, frequency gain 1/64, period limited to ±0.3 %. The
  gains drop to 1/16 and 1/256 while pulses are noisy.
- Only pulses with both edges intact (188 ± 12 samples) steer the PLL. A
  pulse whose start the noise ate does not pull the timing late.
- Sync and blanking levels are learned from clean lines.

**Repair.** It runs once locked and with the field phase known.
- A line whose pulse is missing, malformed or not clean span by span is
  rebuilt at the PLL position.
- The rebuild covers 4.7 µs sync plus 1.2 µs of the front porch at blanking,
  so noise cannot move the leading edge.
- While any recent pulse was noisy (rebuild mode), every pulse is rebuilt,
  because a sampled check cannot see every 75 ns span. Rebuild mode ends after
  64 clean lines.
- Synthesis uses a constant phase step per 25 ns sample on strong cells
  (radius 4–6.5 steps). Every 3-byte TX grouping therefore decodes to the
  learned sync or blanking step. The wrap residual to the next real sample is
  spread over the run, so the edges carry no glitch.

**Vertical interval.**
- The first real broad pulse (at a line start or half a line later) fixes the
  field phase on the PLL line grid. The following fields are counted as
  312/313 lines (NTSC 262/263) with a half-line flag, so the field timing
  follows the H-locked grid instead of drifting.
- Every half-line slot is checked:
  - PAL: 5 pre-equalizing, 5 broad, 5 post-equalizing pulses;
  - NTSC: 6 of each.
- A missing or noisy slot is rebuilt as a whole half line.
- A missing first broad pulse with real broad pulses half a line away is a
  parity error: the field shifts instead of getting a second V sync.

**Coasting and re-lock.**
- Without any real sync the flywheel keeps the raster going. After 2 s with
  no carrier the idle raster takes over (`idle_raster.h`). After ~2.5 s the
  flywheel acquires anew.
- While coasting, the previous line is scanned every 8 lines for a real sync
  elsewhere. Two matching finds move the flywheel there in one step: a VTX
  that returns at another phase gives one timing jump, never a double sync.

**Left alone:**
- the colour burst (HDZero Goggles 2 uses colour-carrier detect for its
  PAL/NTSC switch);
- picture content;
- clean pulses.

## Firmware integration (`main/video.c`)

- **Task.** `sync_fw`, priority 3, woken every 100 µs by the V5 timer. The
  timer runs at 100 µs only when the flywheel is enabled. The other V5 tasks
  keep their 200 µs cadence.
- **Positions.** Absolute byte positions are tracked across ring wraps with
  the timer (40 bytes/µs).
- **Write window.**
  - Writes go only beyond the TX descriptor in flight (+512 bytes).
  - The flywheel never analyses or writes the newest completed RX descriptor.
    That is the one every control observer copies (V5 NO_CARRIER coherence,
    idle raster sync, level servo, AFC), so they keep seeing the unmodified
    reception.
  - Host model: synthetic pulses in pure noise would otherwise raise the
    coherence from ~17–21 to ~26–30, above the no-carrier threshold of 25.
  - This leaves a ~150–180 µs window per line, hence the 100 µs wake.
- **Budget.** The work per wake is limited to ~30 µs, from the learned
  ns/evaluation.
- **Native AGC acquisition mask.** It uses the mask phase table, and
  synthesized bytes keep data bit 0 at the unflagged polarity.
- **Paused** during the menu, the idle raster, PHY labs, the RSSI probe, the
  gain sweep and the pre-Q4 probe (labs measure raw noise).
- Internal SRAM is not cached on the C5, so no cache maintenance is needed.

## Controls

| Key | Action |
|---|---|
| `w` | Toggle the flywheel (NVS `c5vrx4/sync_fw`, default off since the first hardware run: task watchdog, USB console and menu starved), reboot |
| `!` | `SYNC_FW` line: lock and standard, lines/clean/repaired/slots/rebuilt/missed, V syncs found/coasted, parity fixes, re-locks, acquisitions, skipped lines, floor skips, levels, period, noisy mode, `last_us`/`max_us`, budget, ns/evaluation |

## Host evidence (`tools/test_sync_flywheel.c`)

The test generates synthetic PAL and NTSC CVBS (H sync, equalizing and broad
pulses, burst, picture). It is FM-modulated at 40 MS/s into fine-lane Q4
bytes with a 150 kHz CFO. It runs through the live ring model:
- 4092-byte RX descriptors;
- a 16 KiB TX lag;
- the newest-descriptor ceiling.

The transmitted bytes are decoded exactly like the C5VRX-4 TX program
(endpoint phase, trajectory class, DAC LUT), for all three span alignments.

| Scenario | Input pulses intact | Output pulses at sync level, right width |
|---|---|---|
| Clean, strong | all | all; **0 bytes changed** |
| Deep fade (0.5 field pure noise) + 2 fields of destroyed sync | PAL 819/1600, NTSC 702/1358 | **all**; edge ≤ 15 samples (coasting over 2 fields with no edge at all, ~3 ppm) |
| Weak carrier for 6 fields | 0–2 of 1600 / 1358 | **all**; edge ≤ 9 samples |
| VTX gone, back at another phase | — | all after one re-lock; no double sync |
| Native AGC mask | — | no synthesized byte carries the flag |

The test also checks, after every flywheel call, that the newest completed
descriptor still holds the received bytes.

## Hardware gates

1. **CPU.** `!` → `SYNC_FW last_us/max_us/ns_per_eval/skipped`. On a clean
   carrier, expect a few µs per wake. `skipped` and `floor_skips` should stay
   near zero. Watch the task watchdog and USB.
2. **Clean carrier.**
   - `repaired`, `slots` and `rebuilt` must stay at 0, and the picture must be
     unchanged.
   - Compare with `w` off.
3. **HDZero.**
   - Walk to the range edge, and cover the antenna for 0.5–2 s.
   - With goggle logging on, there must be no `switch` lines and no loss of
     lock while `SYNC_FW repaired` grows.
   - Compare with `w` off.
4. **No-carrier behaviour.**
   - VTX off: V5 must still reach NO_CARRIER / maximum gain.
   - The idle raster must still enter after 2 s.
5. **Re-lock.** Power-cycle the VTX: one short timing jump, no rolling.
6. **Native mode with the mask.** Same as 3, plus `AGC_MASK` unchanged.

## Limits

- The repair cannot recover the picture content of a damaged line. It only
  keeps the raster valid.
- Timing when coasting through a long blackout follows the learned period: a
  slow drift of a few ppm, invisible to the goggles. The VTX picture can sit a
  fraction of a microsecond off until the PLL pulls it back.
- A real pulse that is noisy is replaced by a synthetic one at the PLL
  position. That is the intent, but it relies on the PLL tracking the real
  sync (it steers on intact-width pulses only).
- On-chip CPU cost, the 100 µs timer overhead, and TP2825 behaviour are not
  measured.
