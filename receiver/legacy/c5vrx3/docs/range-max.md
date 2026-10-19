# Range max: from physics to picture

Goal: the weakest RF input at which the goggle still shows a usable picture.
Every 6 dB is roughly 2x the distance.

## dB budget

| Link | Sets | State | Achievable |
|---|---|---|---|
| RX antenna | signal | linear WiFi antenna (~3 dB loss on RHCP by definition) | RHCP omni +3-6 dB, patch 8-13 dBic, tracker |
| Diversity | fades | one receiver | two C5, vertical-blank switch: +5-10 dB effective in flight |
| Noise figure | noise floor | C5 internal LNA (WiFi class) | external ~1 dB NF LNA: +3-5 dB |
| Noise bandwidth | noise floor | 40 MHz | BW20 at the edge: +2-3 dB (built) |
| Quantization | information | coarse Q4 at the edge | noise-referenced lanes (built) |
| Demodulator | FM threshold | endpoint Phase8 | see "Demodulator limits" |
| Sync keeping | usable vs lost lock | raw sync | sync flywheel + colour killer (built) |

Rough numbers: kTB over 40 MHz = -98 dBm; with a ~5 dB NF the floor is ~-93 dBm,
and a ~10 dB FM threshold puts the picture threshold near -83 dBm.

## Measured: receiver noise vs quantizer

With the VTX off at maximum gain (G81), ultrafine lanes read P50 7, 34 %
origin. That is sigma ~2.2 ultrafine steps, i.e. ~0.56 coarse step. Widrow:
a uniform quantizer is nearly linear once sigma >= ~0.5 step, so coarse Q4
already loses little at the edge. Finer lanes help by ~1-3 dB, not 12.

## Built

### Range lanes with a noise cap (`direct_gain_v3.c`, `rf.c`)
Fine {9,7,6,5} and ultrafine {9,6,5,4} are exact 2x/4x rescales inside their
windows. They are entered only at the table's maximum analog gain; the analog
gain trims between the 6 dB steps. The receiver noise r^2 (P50 - 1, exact 4^k
per lane) is learned from quiet windows. A carrier is held at the first lane
where noise reaches sigma ~0.95 step (fine on this board); listening uses the
finest lane. Fold guard: hard saturation drops at once; soft evidence (rail
codes, wide incoherent junk) must persist 2 windows (hardware showed ~9
single-window bursts/s); 5 ms re-entry hold-off.

### Bandwidth gear (`video.c`, BW mode AUTO, default)
BW20 only at maximum gain, on the lane cap, with a present but starved or
incoherent carrier for 1 s; back to BW40 after 1 s of clear recovery. BW20
was rejected as a fixed mode (chroma/detail); at the edge it trades a little
colour for ~3 dB of CNR, as analog receivers narrow their IF.

### Sync flywheel + colour killer (parked on `feat/sync-flywheel`)
The BitScrambler demodulates on the fly from the ring TX reads ~409 us after
RX writes it. The flywheel mirrors the Phase8 code formula on a small window
per predicted line, tracks line phase and period with a PLL (phase 1/4,
frequency 1/64, period within 0.3 %), and coasts up to 300 lines. For lines
whose pulse is missing or malformed it rewrites the raw IQ bytes of the pulse
ahead of the TX read so Phase8 itself emits a clean sync at the learned sync
level (constant phase rotation per endpoint, strong cells r 4..6.5). Clean
lines are never modified; the first 20 and last 8 lines of each field are
never written (vertical interval). Above ~25 % repaired lines (EMA ~65 ms)
the burst window is written at blank level so the goggle switches to
monochrome instead of rainbow colour; released below 5 %. The tracked period
also drives the PAL/NTSC detection. Console: `B` repair, `M` colour killer.
**Not in main (2026-09-30):** parked on branch `feat/sync-flywheel`. On the chip it measured
~0.5 us per demodulated code even at -O2; at a 25 % CPU share that covers
only ~1 line in 6, and the first on-by-default builds starved IDLE (task
watchdog) until it was budgeted. It is self-paced now (learned ns/code,
50 us per 200 us wake, streaming acquisition, skip-ahead, fast path) and
safe to enable with `B`, but it needs a large speed-up before it is useful.
The operator saw no difference on a strong signal, as designed (no broken
pulses to repair). Earlier plan: Both are on by default; AGENTS.md has an explicit exception for the
flywheel (operator decision 2026-09-30). A detected real pulse is never
rewritten, even when it sits off the prediction after coasting; the search
window widens with coasted lines (+-16 up to +-40). The field counter coasts
too (313/312 or 263/262 lines, up to 1 s), so a deep fade that also hides the
vertical sync keeps both repair and vertical-interval protection.

Host test (`tools/test_sync_flywheel.c`): synthetic PAL CVBS -> FM -> Q4
cells in the live ring layout with the hardware RX/TX lag, decoded with the
exact BitScrambler formula: 745/745 broken lines repaired (61,020 samples at
sync level, 0 wrong), 0 bytes touched on clean or vertical-interval lines,
colour killer on during a 500-line fade and released afterwards.

### Telemetry and logging
`p` prints `DG3_OBS` (lane, lane_cap, noise_r2_q4, receiver DC, bw40,
bw_switches, fold_drops) and `SFW` (lock, standard, repaired, missed, levels,
colour killer). `tools/range_logger.py PORT out.csv` logs it continuously;
typed lines become marker rows ("picture lost", "30 dB").

## Demodulator: what fits two bundles

The live program reads one 16-bit pair per two bundles and does exactly one
LUT lookup per bundle (address = bundle word bits 16..25, 1024 x 16 bit).

- The live Phase8 already spans 50 ns endpoint-to-endpoint (the test feeds
  pairs `(0, endpoint)`); there is no "+6 dB from an unused half".
- With **8-bit phase**, Sigma (`wrap(d01) + wrap(d12)`), confidence-based
  click suppression and MMSE tables all need more lookups per pair than the
  two available. That narrower statement holds.
- With **5-bit phase states** the Golden Phase5 two-stage shape *does* give a
  pair-joint LUT: raw byte (+2 bits) -> 5-bit state, then (previous state,
  current state) -> DAC. Golden already maps near-origin samples to an invalid
  state -> pedestal. So confidence- or history-conditioned estimation fits at
  5-bit precision. The strongest candidate is the history-conditioned phase
  estimator in `long-range-two-bundle-research.md` (PR #122 worktree): the
  decoder is addressed by raw IQ plus the retained phase quadrant. It must beat
  Phase5 and Phase8 at matched RF input before it can replace anything.

### History-conditioned demodulator (built, opt-in: `P`)

`main/fm_hc.bsasm` is Golden's two-bundle program with one change: the decoder
lookup bank is the retained state's quadrant (`set 24 O29, set 25 O30`).
`LUT[(quadrant << 8) | raw].bits[12:8]` is the MAP 5-bit state given the raw
cell and the previous quadrant (172/1024 entries differ from Golden's static
state, all near-origin cells); `LUT[(prev << 5) | cur].bits[5:0]` is the
nominal Golden P20/G2 code for the state step. Generated by
`tools/gen_fm_hc.py`, verified bit-exact against the reference recursion by
`tools/test_fm_hc.py` (256 random streams, all four banks).

Offline bench (`tools/hc_estimator_bench.py`, synthetic PAL, receiver noise
sigma 0.56 step; trained on one picture/seed/CFO set, scored on another).
IRE rms luma / chroma-band, clicks per mille (|err| > 40 IRE), sync intact %:

| C/N (40 MHz) | Phase8 FULL | Golden Phase5 | HC |
|---|---|---|---|
| 5.5 dB | 14.4 / 13.3 / 426 / 59 | 16.3 / 10.9 / 386 / 58 | 10.3 / 9.3 / 244 / 60 |
| 8.0 dB | 7.7 / 9.1 / 283 / 63 | 11.0 / 8.7 / 259 / 63 | 7.5 / 7.6 / 157 / 68 |
| 10.0 dB | 5.8 / 7.1 / 176 / 68 | 8.1 / 7.1 / 161 / 69 | 6.1 / 6.4 / 100 / 73 |
| 11.6 dB | 4.9 / 5.8 / 101 / 71 | 6.6 / 6.0 / 98 / 72 | 5.2 / 5.5 / 61 / 75 |
| 16.8 dB | 3.0 / 3.1 / 2.6 / 85 | 4.0 / 3.9 / 8.9 / 83 | 3.4 / 3.5 / 3.7 / 85 |

HC beats Golden everywhere and has ~40 % fewer clicks than Phase8 near the
edge (HC at 10 dB ~ Phase8 at 11.6 dB: ~1.5-2 dB threshold). On strong signals
Phase8's 8-bit phase is slightly finer (luma 3.0 vs 3.4). An MMSE pair table
was rejected: it learns the training picture's content and biases sync and
luminance (sync intact 34-38 %). Golden/HC map about -27..+57 Phase8 bins to
the DAC (Phase8 maps +-128): the real VTX deviation must fit that window,
which the flywheel's learned levels will show on hardware.

The sync flywheel mirrors either demodulator (a forward code cursor with an
8-endpoint HC warm-up; HC synthesis alternates state steps to hit the learned
level, cells chosen per decoder bank). Both modes pass the same host test.
Only normal and broad pulses steer its PLL: HC maps ~half of random cells to
low codes, and short noise runs had drifted it during fades.

## Gain-policy corrections (from the same review)

- `emergency_drop()` now removes BB gain before the RF stage: the RF stage sets
  the noise figure and an outer-cell reading cannot tell front-end
  compression from BB/ADC overdrive. A still-saturated next window takes the
  RF stage.
- Saturation no longer waits for a settling write: after an upward write, or
  once the 300 us freshness floor has passed after a downward one (a stale
  pre-write window must not cause a second drop).
- The outer-cell "clip" flag (codes -8/+7, which includes raw 448..511)
  overstates real ADC-limit exceedance; the model in that review shows only
  ~0.9 dB chroma-noise benefit from radius 6.5 vs 5.5 at C/N 20 and almost
  none at C/N 12. This matches the hardware L0/L1/L2 A/B (no visible difference).
- BW20's effect at the MODEM_DIAG tap is unproven. First measurement: VTX off,
  `noise_r2_q4` at BW40 vs BW20 (`W`/menu), before trusting the gear.

## Hardware plan (not firmware)

1. Measurement bench: VTX -> SMA attenuators (10/20/30 dB) -> C5, and the same
   into the goggle's own receiver as reference. Walk tests vary +-10 dB.
2. RHCP antenna, then a patch.
3. 5.8 GHz LNA (~1 dB NF, 15-20 dB gain) with a band filter; V5 and the lanes
   keep the extra gain in range close by.
4. Diversity: two C5s; switch their DAC outputs with an analog video switch
   during vertical blanking, driven by the better `SFW`/coherence score.
