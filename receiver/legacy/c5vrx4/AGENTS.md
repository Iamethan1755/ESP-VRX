# Isolated C5VRX-4 integration ledger

Inherit the repository attribution, hardware and evidence instructions. This
directory extends C5VRX by Twotoz and contributors; README.md and INTEGRATION.md
define its scoped defaults, donor lineage and pending physical acceptance.

Only this directory may change for this integration. Do not edit shared main,
root configuration, workflows or website to make the isolated build work.
Its main/ snapshot is intentional isolation; C5VRX-3 remains untouched. Keep
subsequent donor updates explicit in INTEGRATION_SOURCES.json.

Record source format, acquired rate, CPU/hardware ownership, raw ring, three
bundles, unique 13.333M output and physical DAC40M separately. No CPU per-sample
output, transformed ring, concurrent RX/TX BitScrambler or boundary DSP resets.

Fixed fine lanes {9,7,6,5} are default (Leon, 2026-10-04): never switch lanes
at runtime. Fixed ultrafine and protected adaptive V5 lanes are Z comparisons.
Native is opt-in and owns gain exclusively. Under native the IQ tap is the
coarse lane set for the whole session and the restart patch 71C4[25:23]=7
is applied in every PHY restore (NVS native_patch, default on; board
measurements 2026-10-06 in the native AGC lab commits). Severe coarse or fixed-lane clipping
may cut active Direct Gain to its established G20 floor; fresh epochs and settling
refusal still apply. Manual/native never use this automatic gain response.

Sync scoring is a stride-3 semantic estimate, not tagged physical DAC output.
AFC uses adjacent raw Phase8 burst/clean-porch evidence and separate sticky video
TRACK; AUTO remains off until explicitly selected, with a bounded correction
budget. Do not use gain HOLD alone as a video lock or a CFO-write permission.

Leon explicitly authorized default-on sync-referenced CVBS level regulation on
2026-10-04 in PR164. The adaptive 5/20-ms observer uses completed IQ snapshots and bounded
DAC-only LUT16 updates after a stopped-engine addressing probe. Preserve the
explicit u opt-out, noise/context/settle refusal, loss hold and fault latch. RF SETTLE and lane-history guards must precede
qualification; recovery is bounded to 100 ms, slew to 32 mV in the loaded table.
Live RAM arbitration, waveform seams and FIFO continuity remain physical gates;
do not claim their proof from default-on authorization or host tests. H/V
regeneration and pre-Q4 (PHY) DC correction remain absent outside the reversible `#`
lab. Leon authorized default-on digital DC recentring of the static decoder and
a first-lock sampling-phase check on 2026-10-04; keep their opt-outs, refusal
conditions and verified LUT path. Leon also authorized a fixed, measured analog
RX filter width (target 24 MHz) replacing the BW20/BW40 gear on 2026-10-04.
It extends ESPARGOS esp-sdr's C5 BANDWIDTH control (absolute regs 6/7 code,
noise-FFT curves, commit ac627b0b) and zerowidth PR #3's 11p noise result;
keep that credit, keep regs 8..13 and upper bits calibrated, calibrate only
without a carrier, never go below the 24 MHz floor that C5VRX's BW20 test
motivated, and keep the `^` opt-out. Where the chip cannot reach 24 MHz at
all (first board, 2026-10-06: 19.4 MHz at every RX0 code), Leon decided the
same day that the second stage may narrow the normal profile by at most 7 %
of the delivered width for >= 0.3 dB lower noise bandwidth, and that the
range-edge gear uses the second stage (>= 14 MHz, >= 0.3 dB). Leon asked on 2026-10-04 for the native AGC
acquisition mask (NATIVE_AGC_MASK.md): native mode only, witness bit measured
on the board (never guessed), STATIC decode, six BitScrambler slots, three
bundles per span on every path; keep the `|` opt-out, the per-boot latch, the
pacing exclusion and the DC-recentring refusal (bank 3 is the hold plane). The
no-carrier idle raster (HDZERO.md, 2026-10-04) only replaces demodulated
receiver noise: keep it out of any state with a carrier or sync, exit at the
first one, keep V5 at its no-carrier maximum while it owns TX, and keep the `_`
opt-out. It is not sync regeneration of a received picture. The V5
radius boost (RADIUS_BOOST.md) only moves the healthy P50 band on a strong,
tight, rail-free ring; keep the immediate exit on rail codes/P95/jumps, the
doubling hold-off, the normal-band constants and `y` as an opt-in. The
level servo runs under native AGC too (HDZERO.md): keep its phase-domain
validity gates, and keep CPU snapshots on the Q3 mask decode while masking. Live LUT
writers must stay rare (HDZero): keep the level servo's settled hold (24 mV /
250 ms), DC recentring's 10 s gap and the idle raster's exit hysteresis and
re-entry hold-off. Run verify.py and exact-head
IDF CI before requesting merge approval; merge only after Leon approves.
