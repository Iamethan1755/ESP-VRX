# Where can C5VRX-5 firmware still gain range?

Simulation study, 2026-10-11. `sim.py` models the whole C5VRX-5 chain with
the **bit-exact U85Q LUT** (two LUT16 lookups per 50 ns, `u85q.bsasm`), the
C5 analog channel (5th-order, ~20 MHz), unfiltered 80 -> 40 MS/s decimation,
4-bit ultrafine quantisation at the Direct Gain V5 radius, the resistor DAC,
a 5 MHz goggle input filter and a goggle sync/H-PLL model (ideal and
peak-clamp AGC). `range_study.py` runs the experiments; raw rows and tables
are in `data/`. Synthetic numbers, not measured range.

Each configuration gets two thresholds over a C/N sweep (4 cases per point,
PAL and NTSC, C/N per 20 MHz channel):

- **lock**: the goggle still shows 90 % of the lines correctly;
- **usable**: the luma SINAD reaches 0 dB (below that the picture is noise).

The range limit is the worse of the two. Free-space range scales with
10^(dB/20): 3 dB is 1.41x the distance.

## 1. Sync is not the bottleneck

| configuration (deviation 1.0) | lock | usable |
| --- | ---: | ---: |
| 20 MHz channel | 1.80 dB | 3.55 dB |
| 14 MHz filter | 0.85 dB | 2.77 dB |

The goggle keeps lock 1.5-4 dB below the point where the picture is still
usable. Perfect sync (true timing pasted in) moves lock below -3 dB, but a
realistic regenerator (a slow line PLL on the received video, writing clean
syncs where it predicts them) gains nothing: it sees the same noisy syncs
the goggle already flywheels over. A sync flywheel is not a range lever.

## 2. The picture limit depends on the VTX deviation

Usable threshold, then the combined range limit (worse of lock/usable):

| deviation | v5.0 (14 MHz, kp 0.85) | best | setting | gain |
| --- | ---: | ---: | --- | ---: |
| 0.69 | 4.86 dB | 1.66 dB | 10 MHz filter + kp 0.5 | **3.2 dB** |
| 1.0 | 2.77 dB | 1.10 dB | 10 MHz filter + kp 0.6 | **1.7 dB** |
| 1.4 | 2.31 dB | 2.08 dB | 12 MHz filter + kp 0.85 | 0.2 dB |

A narrower pre-detection filter lowers the noise bandwidth; a lower tracker
loop gain narrows the tracking bandwidth (classic FM threshold extension).
Both suit low-deviation VTXs and hurt wide ones: the low-deviation setting
on a deviation-1.4 VTX loses the picture (0.71 at 4 dB) and sharpness
(SINAD 6.9 vs 14.6 dB at 25 dB C/N). The full grid is in
`data/matched.txt`.

## 3. Strong signal and carrier error (`data/strong.txt`)

- 12 MHz + kp 0.85 (the new default) is at least as good as 14 MHz for
  every deviation at weak signal and loses <= 0.8 dB SINAD close in.
- The matched settings cost 0.9-1.7 dB SINAD at 25 dB C/N (still 10-12 dB,
  a good picture) and win from 12 dB C/N downward.
- A residual carrier error of -0.5 MHz hurts the narrow settings at the
  edge; the AFC holds +-50 kHz, so this is margin, not the operating point.

## 4. Tracker gain alone (`data/tracker.txt`)

kp 0.7 / 0.6 / 0.5 on the 14 MHz filter: up to 2.4 dB better usable
threshold at deviation 0.69, about equal at 1.0, worse at 1.4. Only useful
together with a deviation measurement.

## 5. What C5VRX-5 does with it

- Default filter class >= 12 MHz (was 14 MHz): safe for every VTX.
- **VTX-matched mode**: the AFC already measures the sync tip and the
  burst-free porch of every line; their distance is deviation x 1.914 MHz.
  After 32 tight estimates on a usable carrier the receiver picks
  10 MHz + kp 0.5 (deviation <= 0.85) or 10 MHz + kp 0.6 (<= 1.15), else
  keeps the default. Once per channel session; a retune or reboot starts
  over. The kp variants are the same untrained formula compiled with the
  original generator (kp 0.85 reproduces `u85q.bsasm` bit-exactly).

## 6. Open field: AFC re-acquisition at weak signal (`afc_bias.py`)

Operator report (2026-10-11): good video indoors through walls, poor range
in an open field. One firmware mechanism acts exactly at long range. After
4 windows without confirmed sync (200 ms) the AFC drops its lock and gets a
fresh budget of 4 retunes of up to 250 kHz. Losing the lock alone changes
nothing: the offset stays and U85Q keeps demodulating. The risk is the
retunes made after the lock is lost.

The porch estimate is a mean of wrapped phase steps. Below the FM
threshold, clicks pull it toward the receiver centre, and the porch target
is -1.0 MHz. Model results (true error 0, median of 16 windows, mean of 40
decisions):

| C/N (20 MHz) | 20 MHz chan | 12 MHz class | 10 MHz class | MAD gate passes (12 MHz) |
| ---: | ---: | ---: | ---: | ---: |
| 0 dB | +254 kHz | +177 kHz | +138 kHz | 52 % |
| 2 dB | +120 kHz | +77 kHz | +65 kHz | 100 % |
| 4 dB | +58 kHz | +27 kHz | +14 kHz | 100 % |
| 6 dB | +20 kHz | +5 kHz | +2 kHz | 100 % |

The deadband is 50 kHz. Around the usable-picture threshold (~1-5 dB) the
AFC can therefore step the wrong way: up to 4 x 250 kHz per lost lock, in
a fade-heavy field. Section 3 shows that a -0.5 MHz error already hurts the
narrow settings at the edge.

The firmware now retunes only when the C/N estimate is >= 6 dB
(`main/cn_estimate.h`). A weak window still feeds the estimate history. The
VTX carrier does not move during a fade, so the offset found on a strong
signal stays. `[AFC]` logs every retune; `afc_weak_skips` in `T` counts the
retunes that were blocked.

This model omits burst detection and sync search at that C/N. Whether such
steps happen on the board shows up in the logs (`[AFC]` lines,
`afc_step=1` in FIELD_DIAG).

### Coherence is not C/N

The observer coherence of receiver noise alone depends on the analog
filter: about 42 % through the ~20 MHz chip channel, 58 % through the 12 MHz
class and 61 % through the 10 MHz class. Fixed coherence thresholds
therefore mean a different C/N per filter class. `afc_bias.py --coherence`
gives the tables behind `cn_estimate.h`. Profile gating, the diagnostics
and the AFC gate use this estimate.

## 7. Open field: propagation, the larger effect

These are textbook two-ray numbers (ground reflection coefficient -1), not
C5VRX measurements. They show why a test with both antennas low can lose
range in an open field, while short indoor paths through walls work.

| heights TX / RX | breakpoint | extra loss vs free space at 300 / 500 / 1000 m |
| --- | ---: | --- |
| 1 m / 1 m (walking test, VTX in hand or on the ground) | 77 m | +2 / +6 / +12 dB |
| 1.5 m / 1.5 m | 174 m | -4 / 0 / +5 dB |
| 30 m / 1.5 m (flying) | 3.5 km | within +-6 dB nulls and peaks |

The first Fresnel zone has a radius of 2.0 m at the middle of a 300 m path,
and 3.6 m at 1 km. With antennas at 1 m, the ground fills part of it.

Free-space budget, for scale: 25 mW, 2 dBi each side, 3 dB polarisation
loss (linear XIAO antenna vs RHCP VTX), 3 dB C/N per 20 MHz at the
threshold. That gives ~1.0 / 0.7 / 0.46 km for a receiver noise figure of
5 / 8 / 12 dB. Indoors, distances are ~10x shorter (20 dB less path loss),
which covers a few walls. A good indoor picture and a short open-field
range are therefore not contradictory. Neither proves anything about the
demodulator.

`docs/FIELD_TEST.md` separates propagation (height and position A/B),
antenna/polarisation and receiver sensitivity (conducted attenuation).

## 8. Open field: Direct Gain through fast nulls (`test/test_gain_fade.c`)

A moving drone passes multipath and ground-reflection nulls lasting about
λ/2 ÷ speed (2.6 cm, ~3 ms at 10 m/s), after which the carrier is back at
its old level. Direct Gain V5 jumped to the table maximum on the first
empty window. From G60 that is about +22 dB, but the ultrafine window has
only ~4 dB of headroom above the healthy radius, so the returning carrier
folded. The 1 ms burst gate plus a staged recovery added time on top.

The real `dg_tick()` was run on a rough receiver model (gain from the
decoded tuples, noise at P50 ~9 at the maximum, fold above 7.5 cells):

| event (carrier healthy at G60) | before | fade hold 10 ms |
| --- | --- | --- |
| null 1-3 ms, -40 dB | +1.8 ms after the carrier returns, 4 writes | 0 ms, 0 writes |
| null 1-3 ms, -12 dB | 0-1 ms, 4-9 writes | 0 ms, 0 writes |
| null >= 10 ms | +1.8 ms, 4 writes | unchanged |
| carrier stays 12 / 22 dB weaker | 1.2 / 10.6 ms | 11 / 20 ms |
| VTX off | table maximum | table maximum (after <= 10 ms) |
| +20 dB jump | cut at once | unchanged |

The price is up to ~10 ms longer recovery when a carrier stays weaker, for
example behind an obstacle. A 100 ms hold cost 81 ms there, so it was
rejected. These are model numbers; every gain write is also a short PHY
transient on the board. `fade_holds` in `T` counts the windows held.

## 9. Open field: what gives the most range (`field_antenna.py`)

This section uses textbook physics and data from datasheets and listings,
not C5VRX measurements.

**The receiver chip.** The ESP32-C5 802.11a 6 Mbps sensitivity is
-92.5 dBm (module datasheet). That puts its noise figure at about 5 dB,
the same class as an RX5808 (-90 dBm datasheet). At the threshold, Direct
Gain holds a radius of ~4 cells, so the 4-bit capture costs ~0.1 dB there.
The chip is not what limits open-field range.

**The antenna.** Retail listings give the FPC antenna bundled with the XIAO
ESP32-C5 as a 2.4 GHz part (2.9 dBi). Its 5.8 GHz performance is unknown
and could be poor; this needs an A/B test.

**Polarisation against the ground bounce.** At FPV grazing angles
(0.4-4 degrees), the ground reflection of the RHCP VTX signal returns
almost entirely left-handed. Over average ground it is -0.2..-2 dB in the
opposite hand and -16..-34 dB in the same hand. An RHCP receive antenna
therefore rejects the bounce, limited by its axial ratio. A linear antenna
loses 3 dB on the direct wave and still gets the full bounce, which causes
the two-ray nulls.

Model ranges (25 mW, 2 dBi RHCP VTX, -93 dBm, 95 % of positions):

| receive antenna | VTX 1 m / RX 1 m | VTX 30 m / RX 1 m | VTX 30 m / RX 3 m |
| --- | ---: | ---: | ---: |
| linear, 0 dBi | 400 m | 550 m | 400 m |
| linear, 6 dB worse (mismatched antenna) | 300 m | 250 m | 250 m |
| RHCP omni 3 dBi (13 dB cross-pol) | 1250 m | 1150 m | 1150 m |
| RHCP patch 8 dBi (aimed) | 2600 m | 3150 m | 2850 m |

**Other levers.**

- VTX power: 25 to 200 mW is +9 dB, about 2.8x range.
- A low-noise LNA right at the antenna (NF ~1 dB, e.g. ADI CN0534: 23 dB
  gain, 2 dB NF) adds ~3-4 dB over a ~5 dB chip NF, minus coax loss. A
  ~5 dB NF LNA adds little (VK4GHZ build).
- Spatial or polarisation diversity needs a second receive path (a second
  C5 plus a video switch).
- Firmware cannot turn a linear antenna into a circular one.

## 10. Wide VTXs: the default 12 MHz class clips them (`range_study.py wide`)

On the board's own VTX the sync tip sat at -5.0 MHz and white at
+6.25 MHz (docs/FINDINGS.md). By the firmware's formula, that 11.25 MHz
swing is a deviation of ~1.7, wider than the 0.69-1.4 studied above.

Range limit (worse of lock and usable), bit-exact U85Q, kp 0.85, C/N per
20 MHz (`data/wide.txt`):

| deviation | 12 MHz (default) | 14 MHz | 16 MHz | 20 MHz chan |
| ---: | ---: | ---: | ---: | ---: |
| 1.4 | **2.08** | 2.31 | 2.54 | 3.06 |
| 1.7 | 3.64 | 3.28 | **3.26** | 3.63 |
| 2.0 | 8.03 | 5.98 | 5.27 | **5.18** |

Picture quality at 8 dB C/N (SINAD) for deviation 1.7 is 5.9 dB at 12 MHz
and 7.9 dB at 16 MHz. A calmer tracker (kp 0.70) is worse for every wide
VTX. The firmware adds a WIDE profile: the same tracker (kp 0.85) with a
filter class >= 16 MHz from the same sweep, chosen at a measured deviation
>= 1.6. The break-even is ~1.55, so a wrong WIDE on a narrow VTX costs only
~0.5-0.8 dB. The host test sends no VTX <= 1.45 into WIDE.

The filter blob gets a new version, so the receiver runs one new filter
sweep after the update (startup, as before).

## 11. Where it is good and where it is bad (`situations.py`)

This stress matrix runs the bit-exact U85Q with the profile the firmware
picks per deviation, at 2 seeds and C/N 4 / 8 / 20 dB (`data/situations.txt`;
the focused comparisons used 4 seeds). Picture is the share of lines the
goggle shows correctly.

**Good** (picture ~1.0 from 4 dB): clean signal, residual carrier error of
+-0.5 MHz, motion fades (3 ms nulls, picture kept, SINAD lower), DC 0.5
cell, I/Q imbalance 1.5 dB / 10 degrees, a CW spur at -10 dBc, and a strong
signal at the top of the gain band.

**Bad, and what was done about it:**

- **Negative carrier error at weak signal.** At 4 dB C/N, -0.75 MHz drops
  the picture to 0.86-0.89 and -1.0 MHz to 0.5-0.75 (dev 1.7), and
  -1.5 MHz loses it (0.03-0.58, all profiles). The sync tip then runs into
  the low edge of the filter and the U85Q window. Positive errors keep the
  picture but cost SINAD.
  - Firmware flaw found in the AFC (`test/test_afc.c`): the correction
    budget (4 x 250 kHz) only came back after a lost lock. A VTX more than
    1 MHz off therefore never got centred and never locked. It kept
    0.2-0.45 MHz residual for the whole session.
  - Fix: while still unlocked, a spent budget re-arms after 3 s. The
    offset clamp is widened from +-1.5 to +-2.5 MHz (channels are >= 19 MHz
    apart). VTXs up to 2.4 MHz off are now centred within 20 s; a centred
    carrier with 80 kHz estimate noise is never retuned. The +-2.5 MHz
    range of the vendor offset helper is not yet hardware-tested.
- **Long multipath echoes** (-6 dB at 0.3 us, -3 dB at 1 us) hurt every
  profile alike. A wide VTX loses the picture with either echo, and every
  VTX loses it with the 1 us echo up to 8 dB. This is frequency-selective
  fading. The BitScrambler cannot run an equaliser, so this is not a
  profile or tuning choice. Open-field ground reflections have nanosecond
  delays (flat fading, sections 7 and 9), so these echoes come from
  buildings and trees.
- **DC of 1 cell** hurts wide VTXs at 4 dB (picture 0.36). The measured
  uncorrected DC is ~0.13 cell, and the per-gain DC hold covers it.
- **WIDE vs DEFAULT on a deviation-1.7 VTX:** equal or better in every
  situation (+2 dB SINAD at 8 dB), so the WIDE profile stays.

## 12. Post-detection low-pass in the TX BitScrambler (`u85q_lp.py`)

FM detection noise rises with the video frequency, and the colour
subcarrier sits where it is largest. An ideal 3 MHz low-pass after the
detector (a soft black-and-white picture) moves the usable point by ~2 dB.
The question: does any such filter fit next to U85Q in the TX BitScrambler?

Constraints (ESP-IDF 6.0.2 `bsasm.py`, esp32c5 target): two bundles per
50 ns, both LUT16 lookups used by U85Q, one opcode per bundle (U85Q uses
`nop`, so both are free). In LUT16 mode counter B overlaps the LUT mux
region, so only counter A can be routed next to LUT bits. The counter
operand is output[31:16], shared with the LUT address output[25:16].

- A sliding box filter needs subtraction or a delay line: neither fits.
- Integrate-and-dump boxes need two readable counters, so they do not fit
  either. A box of 4 at 5 MS/s aliases.
- **What fits: a one-pole IIR with pole 1/2**, y <- (y >> 1) + (x >> 1), on
  the U85Q DAC code x. The controller does `LDCTIA`, the worker does
  `ADDCTIAH`, y lives in A[15:10], and the address bits below act as dither.
  - Same LUT, same two bundles, same 20 MS/s [y, y] output, one span of
    latency.
  - The controller carries the phase and x >> 1 to the worker in output
    bits 6..15. Bits 6 and 7 of the written byte drive no pin on the XIAO
    DAC; an 8-bit custom-board output would see them.

Evidence. `u85q_lp.py check` runs a small emulator on the ESP-IDF
assembler's own parse:
- it reproduces U85Q bit-exactly against `sim.u85q_codes`, which checks the
  emulator itself;
- it reproduces the generated `main/u85q_lp.bsasm` bit-exactly against
  the arithmetic model, with 0 mismatches.

CI assembles the program for esp32c5 and runs the check. Board timing,
FIFO throughput and the goggle colour killer are not tested.

Picture model: `data/lowpass.json`, 4 seeds, 12 MHz filter. The range
limit is the worse of goggle lock (90 % of lines) and usable (luma SINAD
0 dB).

| deviation | | U85Q | U85Q-LP | change |
| --- | --- | ---: | ---: | ---: |
| 1.0 | usable | 2.4 dB | 0.6 dB | -1.7 dB |
| 1.0 | range limit | 2.4 dB | 0.8 dB (lock) | **-1.6 dB, ~1.2x** |
| 1.4 | usable | 2.1 dB | 0.8 dB | -1.3 dB |
| 1.4 | range limit | 2.1 dB | 1.7 dB (lock) | -0.4 dB |
| 1.0 / 1.4 | SINAD at 20 dB C/N | 12.5 / 13.9 dB | 10.3 / 10.8 dB | softer, less colour |

- **Crossover:** the low-pass wins below ~12 dB C/N at deviation 1.0 and
  below ~9 dB at deviation 1.4. Above that it costs sharpness.
- **Wide VTXs** become lock-limited, so most of their gain is lost.
- **Firmware selection (superseded, see section 16).** K mode 2 (the
  default for fresh settings) selects U85Q-LP live through the guarded,
  verified pipeline restart, which briefly interrupts the picture.
- Pole 1/2 attenuates PAL chroma by only ~6 dB. Whether a goggle then drops
  colour is untested.

**Negative result: a filter that switches itself inside one program.** The
idea was to have one program with an LP loop and a bypass loop, and switch
between them on a mode bit in the LUT (taking x's LSB) that the CPU rewrites
live.
- The LP loop needs both opcodes, so it can only test the mode bit in a
  check span every 4 spans (8-instruction limit).
- In that span the IIR cannot update: it can only hold y or reset y = x.
- Arithmetic model, deviation 1.0, luma SINAD at 4 dB C/N (U85Q 2.40,
  U85Q-LP 5.09):
  - hold: 3.72 at 4 dB, and 7.86 instead of 12.5 at 20 dB;
  - reset: 3.10 at 4 dB.
- Both lose most of the gain; the 5 MHz check pattern injects noise.
- A 5-bit x on its own costs nothing (2.37 vs 2.40).

A dynamic low-pass therefore means a CPU-side program switch on the C/N
estimate, with hysteresis, or switching in the analog domain.

## 13. Live kp (`kp_morph.py`, `main/kp_ladder.h`), verified restart

**Board correction (2026-10-12):** the live entry-by-entry rewrite below
failed LUT verification on the physical C5 under RUN and PAUSE. The
firmware now loads and verifies the complete target while the core is
halted and restarts the pipeline (a few ms of interrupted video). The
seamless-rewrite reasoning and study below are kept as the host record;
they are not the firmware path (`docs/FINDINGS.md`).

The U85Q loop gain kp is a dynamic setting already. `u85q.bsasm`,
`u85q_kp060` and `u85q_kp050` are the same program with different LUTs.
- The token bits of every word (the raw -> phase observation) are
  identical at every kp, and the 32 phase states mean the same angles. Only
  the (phase, token) -> (next phase, DAC) half differs: 352 to 824 of 1024
  entries.
- So every entry is valid in either table. A table in the middle of a
  rewrite is a valid tracker with a mixed kp, and the tracker state carries
  over. Nothing stops, flushes or resyncs.
- The C5 LUT port writes one entry per register write in the execution
  width (`lut_mode` 1, `lut_idx` = entry). `bitscrambler_load_lut()` is
  avoided: it switches the running program to a 32-bit LUT while it
  writes.

Evidence:
- `kp_morph.py check`: the generator (the C5VRX-4 untrained formula of
  PR #190) reproduces `data/tracker_luts.json` and all three `.bsasm` LUTs
  bit-exactly. Every ladder level (kp 0.85 .. 0.50 in 0.05 steps,
  `main/kp_tables.h`) keeps the token bits.
- `kp_morph.py study` (`data/kp_morph.json`, 4 seeds, C/N 4 / 8 / 20 dB)
  starts a live change mid-field: 0.85 -> 0.6, 0.6 -> 0.85, 0.85 -> 0.7 and
  0.7 -> 0.6, at 1 entry per us and 1 per 10 us.
  - The video error within 0.2 ms of the change stays at the error of the
    worse steady table, 0.70-1.04x on average.
  - The worst case is 1.16x, a full 0.6 -> 0.85 jump at 20 dB with the fast
    writer.
  - Goggle sync is never worse than steady.
  - The ladder target moves one 0.05 level at a time; one guarded restart
    then applies the whole accumulated target (possibly several levels plus
    LP).

Firmware (fresh settings default to AUTO kp+LP; a saved OFF is respected):
- `K` switches the ladder to AUTO (NVS `kp_auto`). The ladder follows a kp
  curve per deviation class (section 14).
  - It takes one level per step: after 1 s of agreement and 0.5 s apart
    toward a calmer kp, after 0.3 s and 0.2 s apart toward a sharper one.
  - A noise-like window (C/N estimate unknown, coherence at plain noise) is
    the floor and targets the curve's 0 dB point. A collapsing carrier
    therefore climbs back to the lock-safe kp (deviation 0.69: 0.50 -> 0.70
    in 0.9 s) instead of freezing deep. Only unusable windows (retune, gain
    write, menu) hold the level.
  - `test/test_kp_ladder.c` checks this, including no hopping under +-3 dB
    of estimate noise.
- `transport_kp_tick` applies a complete, verified LUT16 target through a
  brief halted pipeline restart (shared 5 s cooldown, 2 s AUTO
  confirmation). `k`/`j` step one level by hand for the bench test.
- Active on every profile: MATCHED is the narrow filter plus a start level
  of the same program (section 17). Wide or unknown deviation: kp 0.85 near
  the floor, 0.80 from ~4 dB.

Expected effect: see section 14. Simulation only.

Bench gate before AUTO: run `k` steps under live video and watch `T`.
`kp_verify_fail` and `tx_empty` / `bs_overload` must stay 0 and the
picture must not flicker. Whether a CPU LUT write can collide with a
BitScrambler read is unknown until then.

## 14. The best combination (`best_combo.py`)

Setup: `data/best_combo.json`, 8 seeds (4 PAL, 4 NTSC), 12 MHz filter.
Every kp 0.85 .. 0.50 was run on U85Q and on U85Q-LP, at deviation 0.69,
1.0 and 1.4 and C/N 0..30 dB. The range limit is the worse of usable (luma
SINAD 0 dB) and goggle lock (90 % of lines).

| deviation | U85Q kp 0.85 | best fixed kp | seamless kp curve | U85Q / LP switch |
| --- | ---: | ---: | ---: | ---: |
| 0.69 | 4.50 dB | 2.17 dB (0.50) | 2.38 dB | 1.15 dB |
| 1.0 | 2.36 dB | 1.63 dB (0.55) | 1.70 dB | 0.65 dB |
| 1.4 | 2.08 dB | 2.08 dB (0.85) | 2.09 dB | 1.63 dB |

**kp 0.85 is not the best setting anywhere.**
- Close in, kp 0.75-0.80 is better for every deviation: +0.25..0.9 dB
  SINAD at 20 dB C/N (deviation 1.0: 12.95 vs 12.49).
- At the edge, a calmer tracker wins.
- Near the floor (0-2 dB), a very calm tracker loses goggle lock first:
  deviation 1.0 with kp 0.50 keeps 0.05 of the lines at 0 dB, against 0.85
  with kp 0.85.
- The best kp therefore follows a curve: sharp-ish close in, calm at
  4-12 dB, and back up near the floor.

**Best seamless combination.** U85Q with the live kp curve per deviation
class (`main/kp_ladder.h`), kp at C/N 0 / 2 / 4 / 8 / 12 / 20 dB:

| class | 0 dB | 2 dB | 4 dB | 8 dB | 12 dB | 20 dB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| deviation <= 0.80 | 0.70 | 0.55 | 0.50 | 0.50 | 0.50 | 0.75 |
| <= 1.05 | 0.75 | 0.60 | 0.55 | 0.55 | 0.65 | 0.75 |
| <= 1.15 (interpolated) | 0.80 | 0.70 | 0.65 | 0.65 | 0.70 | 0.80 |
| wider or unknown | 0.85 | 0.85 | 0.80 | 0.80 | 0.80 | 0.80 |

- Gain over today: +2.1 dB at deviation 0.69, +0.66 dB at deviation 1.0,
  0 dB at deviation 1.4 (+0.25 dB SINAD close in).
- The picture is better at every distance, not traded off.
- The mid row applied to a deviation-1.4 VTX costs 0.23 dB, so a wide VTX
  must not be classed as mid.

**Best with a program switch.** U85Q close in plus U85Q-LP at 1-6 dB
(kp 0.50-0.85 by class) is about 1 dB better again at the edge. It needs a
program change in flight, which is not seamless (section 12).

**LP plus kp together** adds only 0.1-0.2 dB over LP alone: both are
low-passes and they overlap.

## 15. Measured kp instead of the curve (`adaptive_kp.py`), negative

The question: can the receiver measure the best kp from the received IQ,
instead of using the section 14 curve?
- The CPU runs a bit-exact software U85Q on 20 control windows (one second)
  for every ladder kp.
- Per kp it measures the sync-tip noise and the colour-burst amplitude, and
  picks a kp by one of three rules.
- Data: `data/adaptive_kp.json`. Deviation 0.69 / 1.0 / 1.4, C/N 1-20 dB,
  6 seeds: the rule weight was fitted on 3 seeds and tested on the other 3.

Mean true luma SINAD of the chosen kp on the held-out seeds:

| oracle (perfect choice) | kp 0.85 | table curve | min noise | burst^2/noise | burst^1.75/noise |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4.97 dB | 3.71 dB | **4.84 dB** | 3.94 dB | 4.37 dB | 4.30 dB |

- The curve is already within 0.13 dB of a perfect per-case choice.
- Measuring does worse:
  - Minimum noise is optimal at weak signal for narrow and normal VTXs, but
    picks far too calm a kp on a wide VTX or a strong carrier.
  - The burst amplitude is too noisy in one second of windows to judge
    sharpness.
- What matters is knowing the VTX deviation, not measuring kp. The curve
  only goes deep for a deviation measured this session.

## 16. U85Q close in, U85Q-LP at the edge: one ladder (`kp_ladder.h`, K mode 2)

Section 14's best combination needs U85Q-LP at the range edge, so it now
switches live.
- **The switch.** Board correction (2026-10-12): the paused instruction
  rewrite described in the original design gave persistent black video.
  `transport_set_lp()` now uses the stop/load/reset/start restart with the
  verified kp LUT restored and one checked rollback. The emulator results
  below describe the instruction schedules, not a seamless handover.
  - Both programs read 16 bits per span, share the LUT and have their
    controller/worker slots in the same places.
  - The bsasm emulator (`u85q_lp.py check`, in CI) switches at every slot,
    both directions. The output equals the new program's steady output
    within 16 bytes (400 ns).
  - A switch just before a worker moves the DAC stream by one byte (25 ns):
    the programs split their 16 bits per span differently.
- **The policy.** The U85Q curve applies above the switch point and an LP
  curve below it, with C/N smoothed over ~0.4 s.
  - LP below 6.5 dB C/N (7 dB for the mid class, 8.5 dB wide or unknown),
    back to U85Q 1.5 dB higher. The wanted program must hold 1 s, with at
    least 3 s between switches.
  - A wide VTX goes back to U85Q kp 0.85 at the floor (below 1.5 dB), where
    it keeps lock better. At the same kp, LP keeps lock as well as U85Q.
  - A switch lands on the new program's curve level at that C/N. The curves
    cross there (deviation 1.0, 6-8 dB: U85Q kp 0.50-0.55 = LP kp 0.85 to
    within 0.1-0.3 dB SINAD), so the picture does not jump.
  - `test/test_kp_ladder.c` checks switch points, hysteresis, the wide
    floor and the 3 s gap.

Range limit (`data/best_combo.json`, 8 seeds):

| deviation | U85Q kp 0.85 | kp ladder | kp + LP ladder |
| --- | ---: | ---: | ---: |
| 0.69 | 4.50 dB | 2.38 dB | **1.15 dB (+3.35)** |
| 1.0 | 2.36 dB | 1.70 dB | **0.75 dB (+1.6)** |
| 1.4 | 2.08 dB | 2.09 dB | **1.72 dB (+0.36)** |

Close in it stays U85Q kp 0.75-0.80 (deviation 1.0: 12.95 vs 12.49 dB SINAD
at 20 dB).

Bench gate before K mode 2:
- Under live video, toggle the program with `P` a few times.
- `prog_restart_fail`, `prog_verify_fail`, `tx_empty` and `bs_overload`
  must stay 0 and the picture must not visibly break.

## 17. The ladder on the MATCHED filter (`best_combo.py`, FILTER_MHZ=10)

C5VRX by Twotoz and the C5VRX contributors. Until now a MATCHED profile
(10 MHz class, kp 0.5 or 0.6) fixed its tracker and switched AUTO off, so
the U85Q-LP rescue never ran for exactly the narrow VTXs where it helps
most. `u85q_kp060/kp050.bsasm` have the U85Q instructions and their LUTs
are ladder levels 5 and 7 bit-exactly, so the firmware now keeps one
topology: a profile sets the filter and the start level, the ladder owns
kp and LP. `data/best_combo_10mhz.json`, 8 seeds, 10 MHz filter:

| deviation | old MATCHED (fixed) | best fixed kp | kp ladder | kp + LP ladder |
| --- | ---: | ---: | ---: | ---: |
| 0.69 | 1.62 dB (kp 0.50) | 1.62 dB (0.50) | 1.71 dB | **0.59 dB (+1.03)** |
| 1.0 | 1.10 dB (kp 0.60) | 1.05 dB (0.55) | 1.09 dB | **0.59 dB (+0.51)** |

- Close in the ladder goes back to kp 0.80 (deviation 0.69: 10.36 vs
  9.79 dB SINAD at 20 dB C/N) and 0.85 (deviation 1.0: 12.45 vs 11.98).
- The narrow curves (`kp_ladder.h`, low and normal classes only) are the
  per-C/N picks of this run; the LP switch points stay at 6.5 dB (the
  crossovers are ~6.9 and ~6.5 dB).
- With AUTO OFF the profile's start kp stays, as before.

The board's own sweep put the narrow class on the last code of the old grid
(code 60, 11.9 MHz noise bandwidth), not on the modelled 10 MHz. The sweep
now also tries codes 52, 62 and 63 (filter blob v4, one new sweep after the
update). Whether the chip gets narrower is a board measurement.

Simulation only. Bench gate as section 16; then a MATCHED VTX at the edge.

## 18. The ladder froze at the range edge (`ladder_gate.py`)

C5VRX by Twotoz and the C5VRX contributors. Sections 14-17 give the kp + LP
ladder its range gain, but only if the control loop lets it move there. Two
things in `control.c` stopped it, both in the regime it was built for.

**The window gate.** The ladder and its two-second switch confirmation used
the AFC's window gate, which includes `afc2_envelope_stationary()`: every
32-sample block power within 2.5x of its neighbour. Near the FM threshold
receiver noise alone fails that test. Share of 4092-byte control windows
that pass (`data/ladder_gate_rates.json`, sim.py IQ, 4 seeds):

| C/N (20 MHz) | 0 dB | 1 dB | 2 dB | 3 dB | 4 dB | 5 dB | 6 dB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 12 MHz class, deviation 1.0 | 0.46 | 0.60 | 0.74 | 0.85 | 0.94 | 0.99 | 1.00 |
| 10 MHz class, deviation 0.69 | 0.30 | 0.45 | 0.65 | 0.80 | 0.91 | 0.97 | 0.99 |

A failed window held the ladder and broke the confirmation. Entering LP
needs 1 s of uninterrupted usable windows, the confirmation 2 s more: at
3 dB that is 0.85^60, below 0.01 %. The ladder's own rule was already that
only a retune, gain write or menu holds it, and its C/N comes from the gain
observer, not from this window. `kp_ladder_window_usable()` now keeps only
those (settle, gain epoch, 100 ms after a gain write, context); the AFC
still needs the stationary envelope.

**The exact-target confirmation.** `kp_switch_confirm()` wanted the same
(level, program) pair for 2 s, while the ladder steps one level every
0.2-1 s as the C/N drifts, so a fade restarted it on every step. AUTO now
uses `kp_switch_confirm_toward()`: the candidate is the program plus the side
of the running level, and the restart applies the latest level. A target
back on the running pair or on the other side still restarts it, so
threshold oscillation never switches (`test_kp_switch_guard.c`). The shared
5 s cooldown is unchanged.

**Model.** `ladder_gate_trace.c` runs the real `kp_ladder.h` and
`kp_switch_guard.h` in control.c's order (50 ms windows, +-3 dB estimate
jitter, 500 ms settle and a new context after each restart), with the pass
rates above. The running program and kp are scored with the best_combo
means at the true C/N (`data/ladder_gate.json`, 16 seeds). Arriving at the
edge (14 dB to X in 3 s) and staying there, the mean C/N where half of the
windows still show a picture (lock >= 0.9 and SINAD >= 0 dB):

| case | fade | before | after | oracle |
| --- | --- | ---: | ---: | ---: |
| 12 MHz, deviation 0.69 | none / +-2 dB 2 s / +-4 dB 4 s | 2.50 / 3.53 / 2.83 | 1.50 / 1.40 / 1.48 | 1.50 / 1.23 / 1.31 |
| 12 MHz, deviation 1.0 | same | 1.50 / 1.94 / 1.91 | 0.51 / 0.83 / 0.82 | 0.50 / 0.90 / 0.98 |
| 12 MHz, deviation 1.4 | same | 2.50 / 2.23 / 2.09 | 1.50 / 1.96 / 2.12 | 1.50 / 1.92 / 1.99 |
| 10 MHz MATCHED, deviation 0.69 | same | 2.33 / 2.09 / 2.00 | 0.52 / 0.52 / 0.61 | 0.50 / 0.56 / 0.64 |
| 10 MHz MATCHED, deviation 1.0 | same | 1.50 / 1.25 / 1.32 | 0.50 / 0.91 / 0.99 | 0.50 / 0.90 / 0.98 |

- Arriving at the edge, the fixed loop gains 1.0-2.1 dB (1.12-1.27x free-
  space distance) in 10 of the 15 cases, 0.3 dB for MATCHED deviation 1.0
  under fades and 0..0.3 dB for deviation 1.4 under fades. Every case lands
  within 0.2 dB of the oracle. Before, the loop stayed in the close-in
  setting it had (U85Q, kp 0.65-0.85) and often never reached LP.
- Removing the gate gives most of it. The directional confirmation adds
  0.6-0.9 dB for deviation 0.69 on the 12 MHz class under fades and
  ~0-0.1 dB elsewhere.
- A slow fly-out (0.25 dB/s) changes little (-0.2..+0.2 dB): LP is entered
  near 5.5 dB, where ~99 % of the windows still pass. That is also why a
  steady walk-away test can hide this.
- A wide VTX (1.4) under strong fades is lock-limited either way.

Simulation of the control loop on the best_combo picture model, not a board
measurement. On the board, `kp_steps`, `lp_switches` and the `[AFC]`/
FIELD_DIAG records at the edge show whether the ladder now follows the C/N.
Gain writes (100 ms each) still hold the ladder; how often Direct Gain writes
at the edge is not modelled here.

## 19. Sampling phase, wide-VTX rows and narrow-row selection (2026-10-13)

C5VRX by Twotoz and the C5VRX contributors.

**What a mid-transition read costs** (`sphase_study.py`). PARLIO RX samples
the ~80 MS/s MODEM_DIAG bus at 40 MS/s with a phase fixed at reset (three
positions per bus word). On a bad position some bits of a read come from
the next word. The model runs sim.py's chain at 80 MS/s, quantises both
words with one gain and mixes their bits. Range limit (worse of lock 0.9
and luma SINAD 0 dB), bit-exact U85Q kp 0.85, 12 MHz, 4 seeds:

| bits from the next word | deviation 0.69 | 1.0 | 1.4 | SINAD at 20 dB (dev 1.0) |
| --- | ---: | ---: | ---: | ---: |
| none (clean) | 4.50 dB | 2.33 dB | 2.07 dB | 12.9 dB |
| Q nibble (12.5 ns I/Q skew) | 4.60 | 2.50 | 2.44 | 11.0 |
| I LSB only | | 2.62 | | 10.8 |
| I and Q LSB | | 2.94 | | 9.2 |
| I sign only | | 3.43 | | 7.4 |
| two low bits of I and Q | 6.39 | 3.81 | 4.29 | 6.8 |
| every bit at random | 6.26 | 3.72 | 4.27 | 6.4 |
| half of the bits | 7.19 | 4.65 | 7.09 | 4.7 |

The clean row reproduces section 14's kp 0.85 baseline (4.50 / 2.36 /
2.08 dB), which checks the model. One mixing LSB costs ~0.3-0.6 dB, several
bits 1.4-5 dB and a grainy strong picture. On the probe (dsp.h) a clean
carrier reads 2-60 milli-cell^2 in the model (board: -70..+42), a mixing
LSB lane ~500 and multi-bit mixing 2400-7500.

**Why the boot check was not enough.** The startup scan repairs a boot that
lands on the transition. But with the VTX off it runs on receiver noise at
the table maximum, where a clean position already reads ~725 (board,
intrinsic broadband noise), and a phase that drifts there in flight was
only reported until the next boot.

**Firmware (`main/sphase_logic.h`, `calibration.c`, `test/test_sphase.c`).**
On the carrier (burst-confirmed sync, Direct Gain >= 6 steps below the
table maximum):
- *Choose:* the first carrier window of a boot (or the final startup step
  when the VTX is on) scans the three positions and settles within 30
  milli-cell^2 of the cleanest one, so every boot ends on the position
  farthest from the transition, not just one below the limit.
- *Watch:* a passive probe every 10 s against 150 (half the absolute
  limit), so one LSB lane that mixes in a third of the reads is caught
  before the multi-bit cliff. Contrast on the carrier uses the carrier
  margin (2 x 30) instead of the noise floor of 200.
- *Repair:* two consecutive excursions (2 s apart) run the same scan. No
  per-boot cap: a scan without contrast (false alarm) doubles the gap from
  60 s up to 16 min, a scan that moved the phase resets it.
- A slip moves the RX write point by a few samples once, much less than the
  verified kp/program restart, and the control loop pauses ~0.4 s.

What this does not do: remove the cause. A source-synchronous MODEM_DIAG
clock would, but its existence is not established (legacy C5VRX-3 PR #15).
Board check: `F`/`T` SPHASE lines (`carrier_qualified`, `carrier_scans`,
`moved`, `false_alarms`) and `SPHASE ... carrier_scan` logs.

**Wide VTX rows** (`best_combo.py`, FILTER_MHZ=16, deviation 1.4 / 1.7 /
2.0, `data/best_combo_16mhz.json`). The existing LP rule already reaches
the best range limit in that data (2.75 / 2.86 / 3.96 dB vs U85Q kp 0.85
2.73 / 3.26 / 5.27 dB); no candidate does better, so a wide VTX gets no more
range from this demodulator. The 12 MHz wide row's kp 0.80 is wrong on the
16 MHz filter: kp 0.85 is 0.5 dB SINAD sharper at 12 dB C/N for deviation
1.7 and 2.2-2.8 dB at 12-30 dB for deviation 2.0, and LP still wins up to
~9.5-13 dB. The WIDE profile now has its own rows (`kp_ladder.h`
`kp_curve_wide16_x100`, LP below 9.5 dB). Simulation only.

**Narrow rows by measured width.** The narrow curves come from a 10 MHz
model; the board's narrow entry measured 11.9 MHz. On the 12 MHz data the
12 MHz rows are 0.13 dB better at deviation 0.69 (equal at 1.0) and
0.3-0.4 dB sharper close in, so a MATCHED profile uses the narrow rows only
when its filter measured <= 11 MHz (`kp_ladder_narrow_rows`). The narrow
filter itself not reaching 10 MHz costs the ladder ~0.65 dB at deviation
0.69 and ~0.2 dB at 1.0 against the 10 MHz model.

**Ladder evidence on the board.** FIELD_DIAG records carry `kp_x100` and
`lp`; `tools/field_diag_compare.py` reports `edge_s`, `edge_lp_pct`,
`edge_kp_median` (C/N below 6 dB) and `strong_kp_median` (>= 12 dB).

## Not firmware levers

- The second IQ40 sample of every span (+0.5..1.1 dB in the C5VRX-4 ledger)
  needs four lookups per 50 ns; the TX BitScrambler does two.
- Antenna diversity, LNA and filters in front of the C5: hardware.

## Reproduce

```
python range_study.py filter  data/filter.json
python range_study.py sync    data/sync.json
python range_study.py tracker data/tracker.json
python range_study.py matched data/matched.json
CNRS=0,2,4,8,12,16,20,25 python range_study.py strong data/strong.json
python u85q_lp.py study data/lowpass.json      # U85Q vs U85Q-LP (section 12)
python best_combo.py data/best_combo.json      # best kp / program per C/N (section 14)
FILTER_MHZ=10 DEVS=0.69,1.0 python best_combo.py data/best_combo_10mhz.json   # section 17
IDF_PATH=<esp-idf> python u85q_lp.py check     # emulator: U85Q and U85Q-LP bit-exact
python ladder_gate.py rates data/ladder_gate_rates.json                      # section 18: window gate pass rates
python ladder_gate.py study data/ladder_gate_rates.json data/ladder_gate.json # section 18: ladder at the edge
python sphase_study.py data/sphase_study.json                                # section 19: mid-transition reads
FILTER_MHZ=16 DEVS=1.4,1.7,2.0 python best_combo.py data/best_combo_16mhz.json  # section 19: WIDE rows
```

Needs numpy, scipy and numba. `data/tracker_luts.json` holds the kp variant
LUTs (generated with the C5VRX-4 untrained generator of PR #190).

## Diagnostic cliff follow-up (PR #196, 2026-10-11)

CVT follow-up: [candidate results and remaining work](CVT_STATUS.md),
[two-bundle hardware constraints](CVT_HARDWARE.md). `u85_cvt.py search`
compares immutable analytical laws; `check` verifies their complete LUT16
programs against the ESP-IDF instruction emulator. `cvt_validation.py`
uses held-out seeds and the same Q4 for all fixed/CVT/floating candidates.
CVT is experimental and disabled in normal firmware builds.

Operator clarification: the image becomes static abruptly; the analog feed
remains, with no goggles NO SIGNAL indication. This does not establish whether
RF/FM threshold, tracking, filter/CFO or CVBS/goggle decoding is responsible.
The passive diagnostic fix separates all-window AFC sync evidence from
measurement coverage and conditional sync. It adds bounded timestamped pre/post
onset records at the existing nominal 50-ms control cadence, not extra live DSP.
These metrics cannot measure recognisable picture/luma quality or resolve every
sub-50-ms fade. Unknown evidence stays UNKNOWN; FAST_EVIDENCE_LOSS is not a
propagation diagnosis. C/N, noise figure and distance from the simulations are
model assumptions, not independently measured RF sensitivity. Identify the
actual supplied antenna before assigning it a band or loss.

See `docs/FIELD_TEST.md` for repeated 0.5–1-dB conducted attenuation sweeps,
independent IQ/video/output thresholds, real-IQ floating/bit-exact replay and
outdoor antenna/height A/B/A. RF offset readback, analog BW/passband and DC
validity across profiles still require bench measurements. No measured range
gain or physical picture-cliff fix is claimed by the diagnostic code.
