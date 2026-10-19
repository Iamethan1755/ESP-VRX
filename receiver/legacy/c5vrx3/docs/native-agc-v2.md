> **Status (2026-09-30):** findings from PR #122, carried to `main` with PR
> #126. Their conclusion is that native AGC is not the default; Direct Gain
> V4 is. Code named below (AGC policy/offset/pacing keys, P8 FINE/ULTRAFINE,
> sweep tables) lives on branch `feat/native-agc-v2`, not on `main`.

# Native AGC V2: Phase8 image quality, AFC reference, range (#121, #115, #118)

Status: research, measurement tooling and opt-in controls. Default
behaviour is unchanged: native hardware AGC stays the only gain owner (#121),
AFC stays OFF, the start-gain override stays at the vendor value and the
demodulator stays Phase8. Production RF behaviour differs from `main` only by
releasing FFT scale again after a channel change in native mode.

## Native AGC register semantics

Sources: the pinned C5 `libphy.a` (disassembled) and
[ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr), which implements
hardware-AGC and manual gain on C5/C6/C61. Items marked *bench* were measured
on this board.

| Register / routine | Meaning | Evidence |
|---|---|---|
| `0x600A702C` [31:24] / [23] | forced gain index / force enable (`phy_force_rx_gain`) | disassembly |
| `0x600A702C` [14:8] | highest calibrated gain index (83 here) | esp-sdr `gain_max()`, `phy_agc_max_gain_set` |
| `0x600A7094` [8:2] | AGC **initial gain**: each acquisition starts here (vendor: max-1 = 82) | esp-sdr C61, `phy_agc_max_gain_set`; *bench*: 7078 re-enters 82 at every restart |
| `0x600A713C` [24:18] | AGC gain threshold (vendor: max-1 = 82) | esp-sdr C61, `phy_agc_max_gain_set` |
| `0x600A7030` bit 29 | baseband AGC disable (`phy_disable_agc` / `phy_enable_agc`, reversible) | disassembly |
| `0x600A705C` | RF-side AGC / saturation intervention; `phy_rfagc_disable` writes 0 (plus clears bits in `0x600A284C`, already 0 in native mode) | disassembly, esp-sdr disables RF saturation intervention for stable gain |
| `0x600A706C` [15:8] | signed signal RSSI (`phy_get_sigrssi`) | disassembly |
| `0x600A706C` [7:0], `0x600A7078` [7:0] | undecoded AGC state; 7078 walks 82 -> 60 -> 58 at each re-acquisition | *bench* |
| `0x600A7064`, `0x600A7114` | four ascending saturation bytes each (`phy_wifi_agc_sat_gain`) | *bench*: +4 A/B had no measurable effect |
| `0x600A7068` | packet-detect config (`phy_rx_pkdet_num_set` = 0x808) | disassembly |
| `0x600A7010/7014` [31:23], `0x600A7044` [7:0] | rx-sense detection thresholds (`phy_rx_sense_set`) | disassembly |
| `0x600A8004..807C` | written only by `bb_agc_reg_update()`; `8028 = 0xC0403020` looks like stacked level thresholds | disassembly; untested |
| sample-dump word [27:20] / [31:28] | per-sample gain index / AGC FSM state (esp-sdr dump format; Q in [9:0], I in [19:10] as routed to MODEM_DIAG) | esp-sdr; not yet read on C5VRX |

## Bench findings (A1 5865 MHz, VTX close)

- Native AGC re-acquires constantly with a carrier present: the state byte
  changes 137-319 times per ~20 ms. `7078` re-enters 82 roughly 1-4 times per
  ms, i.e. 0.064-0.256 re-entries per 64 us line on average. With no signal
  it can sit still. These bytes are state proxies, not validated per-sample
  gain metadata; re-entry is not proof of a hardware restart.
- Pinning the gain (force bit) gave a perfectly steady Q4 circle and zero phase
  jumps, but the picture got noisier at the AGC's own low median gain. Firmware
  holding is also ruled out by #121 (zero CPU gain decisions).
- **Field rounds 1-3 (`7128`, `7034`, `7158`, `71B0`, `7068`, `7010`,
  `7014`, `7044`) are invalid**: a BOOT short-click had moved the receiver to
  A2, so every reading shows no carrier (hard-jump rate >= 928 per mille,
  coherence ~0). P8ENV now carries `ch=`/`mhz=`, and `p8env_sweep.py` warns on
  carrier-less rows.

## Blue blacks / constant static: Q4 quantisation in the colour band

`tools/sim_phase8_false_colour.py` models the live path exactly (Q4 cells ->
256-code LUT -> 50 ns endpoint delta, as in `fm_phase8_hr_live.bsasm`) and
measures the energy the DAC output puts in the PAL/NTSC chroma band on flat
picture areas ("false colour"):

| Q4 radius (cells) | false colour, IRE rms (PAL, sigma 0.25) |
|---|---|
| 1.5 | ~6.5 |
| 2.5 (native AGC today) | ~3.9 |
| 4.0 | ~2.4 |
| 5.5 | ~1.8 |

With little noise, specific picture levels produce deterministic tones up to
~4.8 IRE at radius 2.5: a dark tone at such a level decodes as a colour cast.
An arc-midpoint LUT does not help (4.38 -> 4.28), and the live path already
uses Golden's 50 ns delta. **The fix is a larger Q4 radius, i.e. the native
AGC target level, if that control is identified and measured.** Larger
radius with fixed post-gain noise is not a constant-SNR RF range comparison.

## AFC

- **Hygiene (#115 items 2, 4, 5)**: `main/afc_state.h`. CFO pairs need both
  endpoints valid; no sample is accepted across a settle period or gain/PHY
  transition; state resets on mode, channel, profile, BW, offset or RF
  generation change; a correction needs 8 fresh samples. Test:
  `tools/test_afc_state.c`.
- **V2 reference (#115 items 1, 3, 7-10)**: `main/afc_v2.h` measures
  instantaneous frequency only on the sync tip and on the back porch after the
  colour burst. Sync is detected on the demodulated frequency, independent of
  FM polarity, bracketed by matching porches and confirmed by the colour burst,
  which also reports PAL/NTSC. Smoothing is a 20+6 sample cascade that nulls
  the VTX audio subcarriers (6.0 / 6.5 MHz). Reported in P8ENV (`afc2_*`).
  Test: `tools/test_afc_v2.c`, 48 synthetic PAL/NTSC cases (both polarities,
  colour burst, random picture blocks, audio subcarriers) through the real
  quantiser.
- **V2 correction**: `main/afc_v2_ctrl.h`. AUTO AFC (default OFF, acquisition
  only) now corrects only from that reference: 16 burst-confirmed estimates,
  same polarity/standard, MAD <= 80 kHz, 50 kHz deadband, steps <= 250 kHz,
  at most 4 corrections per acquisition. The porch is the default centre
  reference; #115 section 9 still requires confirming the centre level and
  the sign on hardware before AUTO AFC is recommended. Test:
  `tools/test_afc_v2_ctrl.c`.

## VTX facts that matter here (RTC6705 datasheet)

Almost every analog FPV VTX uses the Richwave RTC6705: 1 Vpp video input and
FM audio subcarriers at **6.0 and 6.5 MHz** at -25..-30 dBc (modulation index
~0.06-0.11, i.e. roughly 0.4-0.7 MHz peak carrier deviation each; 12 kHz audio
pre-emphasis). A normal FPV receiver low-pass filters the demodulated video,
removing them. C5VRX puts the demodulated frequency straight on the 20 MS/s
DAC, which attenuates 6.5 MHz only ~16 %, so a tone of roughly 10 IRE at
6-6.5 MHz is probably present in the CVBS output as fine static patterns. This
is unconfirmed on this VTX: the short `Q` captures were too noisy to resolve it.
A full-window capture (`z` + `tools/analyze_q4_window.py`) can expose spectral
peaks but cannot identify their cause. The review reproduced 22 dB / 20 dB
peaks in these bands from constant-envelope FM with NO audio, due to Q4
harmonics. Change picture level / CFO and compare the tone's motion before
attributing it to audio. No sound trap or AGC register change is enabled by
these observations. The loaded DAC resistor network and 470 pF capacitor
must also be included in any measured filter response.

## Lab tools in this PR (all read-only unless stated)

| Key | Function |
|---|---|
| `E` | P8ENV row: channel, Q4 envelope, state-byte changes and start-byte re-entry proxy, start gain, AFC V2 reference |
| `h` | fast poll of the native AGC state bytes (histogram, switch rate) |
| `Q` / `z` | raw Q4: four 64-sample runs / one contiguous 4092-sample window |
| `T` | AGC register dump (`0x600A7000..71FC`, `0x600A8000..807C`) |
| `P` `M` `J` `B` | *writes*: select / step / restore candidate AGC fields (reversible, reboot restores) |
| `w` | *writes, persisted*: native AGC start gain vendor -> 74 -> 66 -> vendor |
| `u` | *persisted, reboots*: Golden <-> Phase8 VIDEO32 (bounded video-transfer candidate) |
| `d` | *persisted, reboots*: original Phase8 FULL (8-bit endpoint arithmetic) |
| `i` | *persisted, reboots*: PLL-tracking A/B (only in a lab build with `CONFIG_ESP_PHY_DISABLE_PLL_TRACK=n`) |
| `N` | *persisted, reboots*: native AGC <-> firmware gain |

## Next bench session (script, not exploration)

1. Verify `ch=A1`, carrier present (coherence > 0) before anything else.
2. Start gain A/B: hold each of vendor / 74 / 66 for about 30 s, operator
   answers "blue / not blue, noisy / clean" per setting (no timing race).
3. Range: same spot at the edge, Phase8 vs Golden (`u`) and start gain vendor
   vs best candidate; compare with `main` if doubts remain.
4. Several `z` windows with different picture levels / CFO: investigate the
   6.0/6.5 MHz peaks and candidate envelope edges. Neither proves audio or AGC.
5. AFC V2 on hardware: step the offset (`,` / `.`) and check `afc2_porch_khz`
   follows 1:1 with the expected sign; only then try AUTO AFC.
6. PLL tracking A/B for the white-screen / stuck-low state (lab build).

## Review fixes and bounded video candidate

The VIDEO OUTPUT menu now cycles PHASE5 -> PHASE8 FULL -> P8 VIDEO32 with a long
press; the choice is persisted and applied when leaving the menu. Serial
`u` / `d` selects and reboots. Fresh installations still default to P8 FULL;
existing `demod_golden` choices migrate without changing their meaning.

P8 FULL maps the entire signed endpoint range onto 64 DAC codes, pedestal
32. It has much less video swing than Golden at the same RF. P8 VIDEO32
uses Phase8 cell-centre angles to design a 32-state codebook and a bounded
pair LUT: pedestal 20, gain 0.75 code per Phase8 bin, saturation before
conversion, tapered far tail at 96..112 bins (135..157.5 degrees). The far
tail is a video prior, not proof that a transition is impossible. Both use
two bundles, 20 MS/s unique DAC output duplicated at 40 MHz, and continuous
state across DMA boundaries. VIDEO32 does NOT preserve eight bits of
endpoint state; it is an explicit precision/transfer tradeoff. A full-precision
nonlinear transfer has not been proven at two-bundle throughput.

`tools/test_phase8_video.py` exhausts all 65,536 raw endpoint pairs through
the emitted assembly. `sim_phase8_false_colour.py` also models VIDEO32 and
reports brightness error instead of returning a placeholder zero. At sigma
0.25, PAL dark maximum at radius 2.5 is approximately 4.38 IRE for FULL versus
4.79 for VIDEO32, and at radius 5.5 2.31 versus 2.78. Thus this candidate is
NOT advertised as a fine-grain cure and is opt-in. It fixes output range and
unsafe amplified tails; loaded voltage, colour, sharpness and range need A/B.

Grain is already present in noiseless Q4 simulations. Eliminating it needs
better input information / suitable filtering, not simply more phase bits or
digital gain. Raising the native target, a finer pre-Q4 tap with explicit
overflow handling, or a proven realtime filter are still hardware research;
no guessed native AGC target register is written by this change.

AFC now uses individually burst-confirmed runs with sufficient valid tip and
porch pairs. Invalid estimates discard ALL consecutive history. Context is
checked across the snapshot; a bounded DMA-copy deadline prevents accepting
a copy after the ring could have lapped. Settling ticks advance before any
native/manual/profile branch. Native mode skips firmware gain state machines;
its AFC gate no longer requires firmware-controller power >=18. A conservative
IQ-envelope gate rejects suspected transitions, but cannot prove autonomous
gain stayed fixed without per-sample metadata. The 20+6 smoother group delay
is 12 samples (0.3 us), not 26 samples. AFC remains OFF by default pending
centre/sign validation.
Native acquisition lock is sticky: CFO drift alone does not authorize an
in-flight retune. Four invalid windows re-arm acquisition and its correction
budget; changing the receive context also resets the lock.

The analyzer requires complete ordered 4092-byte windows, validates both
frequency endpoints, and uses Cartesian-cell radius bounds before reporting
envelope candidates. Rotating a noiseless radius-1.2 vector no longer creates
hundreds of fake AGC events. State fields are labelled as proxies; start-byte
re-entry uses the configured start (including 66/74), not a hard-coded >=80.


## Phase5/Phase8 switching and remaining range work

Golden is the original **Phase5** endpoint discriminator. On VIDEO OUTPUT,
long press cycles **PHASE5 -> PHASE8 FULL -> P8 VIDEO32**. The first
two are the reference A/B; VIDEO32 remains an explicitly experimental,
32-state bounded-transfer candidate. Exit the menu to apply the saved choice.
Native hardware AGC owns RF gain in all three selections. The recovery
hold now also restores and persists the actual Golden/Phase5 program, rather
than only resetting a legacy enum while Phase8 remained selected.

The native sync/standard observer now uses the Phase8 raw-IQ detector in
`afc_v2.h` rather than Golden's DAC low-code threshold. Accepted burst-bearing
runs report their measured width and, when two agreeing runs fit inside the
window, their measured spacing. Standard votes require matching PAL/NTSC burst
and horizontal spacing; a lone burst earns only partial sync confidence.
This observer is independent of the selected DAC pedestal/gain.

Acquisition lock and AFC writes share the 16-estimate median/MAD stability
test. AUTO must additionally be centred; OFF/HOLD can lock a stable receive
state with a nonzero reference offset, so experimental BW AUTO cannot keep
switching solely because centering is disabled. Valid TRACK remains sticky.
Invalid DMA copies also age the native lock, clear stale CFO and discard
consecutive evidence. All slow IQ consumers use the bounded descriptor copy;
the returned ring offset belongs to that same copy, preserving endpoint parity.
The copy deadline sums the actual intervening descriptor lengths, including
the short tail node, rather than assuming a uniform ring.

For #118, compare exactly the same frozen samples through the three actual
assembly LUTs, without RF/AGC variations between modes:

```sh
python3 tools/compare_live_demods.py capture.log
python3 tools/compare_live_demods.py capture.bin --binary --parity 1
```

The JSON contains a per-window input hash, endpoint parity, sample count and
per-mode DAC swing/rail/jump statistics. Separate `z` windows are never joined
across a capture gap. Raw DAC statistics have different gains/pedestals and
are not a quality ranking or proof of RF sensitivity. Use a controlled
attenuation sweep and external CVBS picture/sync rating to establish range.
The exhaustive regression checks all 65,536 raw pairs and both byte parities.

Register semantics were checked against Espressif's pinned v6.0.2
[`ahb_dma_struct.h`](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/register/soc/ahb_dma_struct.h):
`in_dscr_bf0` identifies fetched descriptor x; `in_dscr` already points at
x+1. We therefore retain the previous-buffer selection and validate its
copy lifetime. This is software snapshot hygiene, not a new proof of gapless RF.

Remaining #121 hardware work is still finding and validating native target,
hysteresis and retrigger controls, then measuring annulus/near-far recovery.
The failed target-register sweeps do not justify promoting guessed writes.
No CPU gain controller or guessed native policy patch is enabled here.

## Why native AGC restarts, and what that rules out (2026-09-30)

**Measured.** The raw 80 MS/s RF dump words carry the RX gain index in bits
20..27 (ESPARGOS esp-sdr word format; bits 28..31 are the AGC state
machine). In one 102 us boot capture the gain index (bits 20..25) shows a
new acquisition every ~25-50 us, i.e. one to two per 64 us video line, each
a ~2.4 us walk in 0.6 us steps (e.g. 16 -> 18 -> 58 -> 34 -> 10) that ends on
a *different* trapped gain each time (10, 28, 19). The old `7078` re-entry
proxy (1-4/ms) undercounts this by an order of magnitude.

**Mechanism.** This is an 802.11 packet AGC: detection -> coarse acquisition
from the start gain -> gain trapped for the "packet" -> reset on packet end
or abort (WARP SISO AGC, US 7212798). A 25-50 us cycle matches preamble
processing (STF/LTF/SIG ~20 us) followed by an abort and immediate re-detect
on a carrier that never stops. The C5 ROM/libphy expose only start gain
(`7094`), gain threshold (`713C`), saturation bytes (`7064`/`7114`), RF
saturation intervention (`705C`), CCA (`701C`, `0x600A4C5C`) and rx-sense /
packet-detect thresholds (`7010`/`7014`/`7044`/`70CC`/`7124`, `7068`,
I2C pkdet); `phy_enable_agc` is `7030[29]` clear plus a `702C[23]` strobe.
No in-packet tracking control exists: re-acquisition *is* the tracking.
Suppressing detection leaves a fixed gain; keeping it keeps the restarts.

**Picture.** The 2.4 us acquisitions (saturated, starting high) are the thin
black / rainbow dashes; the different trapped gain per ~40 us segment is the
line-to-line contrast/texture change. About 6% of samples are acquisition,
which matches the 52 pm of Q4 samples outside |x| < 256 under native AGC.

**Rejected.** Pinning the gain from firmware (the former HOLD/HOLD123
profiles) is clean while the VTX is still but lags when it moves; it hides
the cause behind a CPU control loop and was removed. Start-gain following
(INIT) raised the re-entry rate and was removed too.

**Kept / next.**
- `P8 FINE` (VIDEO OUTPUT menu, serial `y`): the PHASE8_FULL program on the
  #123 fine lanes I/Q {9,7,6,5}. DIAG[14]/[15] (I4/I5) are proven bit-exact
  with six-bit reference alignment (probe passes `CAND_123`, `I_BUS_4_9`;
  the old 63-91% came from pass 1 locking fast I bits on Q[8:9] only).
  Trapped samples fit the window; only acquisition samples fold, and those
  are saturated garbage on the coarse lanes as well.
- Probe builds print three full-word windows (`AGC_WORDS`) after the sweep;
  `tools/analyze_agc_words.py` reports acquisition interval/duration/share,
  gain paths, which state bits separate acquisition from trapped, and IQ
  amplitude against the fine window. That state bit is the candidate hold
  flag for an AGC-gated demodulator.
- A per-sample guard with reseed fits two bundles only in the pair-LUT
  architecture (`fm_golden_hard_guard.bsasm` pattern); PHASE8_FULL uses
  counter arithmetic, which cannot share a bundle with a branch.


## Native AGC register sweep (2026-09-30, A1, VTX static)

`tools/agc_tune_sweep.py` on the probe build: one field changed per boot,
three full 80 MS/s dump windows each (`AGC_WORDS`). Vendor reference (same
setup, earlier boot): 35.8 acquisitions/ms, 13.4 % of time in acquisition,
3.4 us per acquisition, trapped radius ~135 codes, radius spread 225.

| candidate | acq/ms | acq % | acq us | radius | spread | out +-256 pm |
|---|---|---|---|---|---|---|
| 7128[31:24] -40 / -34 / -52 (vendor -46) | 36 / 62 / 52 | 10 / 20 / 18 | 2.4 / 3.4 / 2.3 | 184 / 178 / 146 | 263 / 285 / 313 | 123 / 37 / 167 |
| 702C[7:0] comp -24 / -36 (vendor -30) | 29 / 20 | 11 / 6 | 2.8 / 2.8 | **140 / 92** | 176 / 226 | 27 / 22 |
| 70A0[31:24] comp -24 / -36 (vendor -30) | 78 / 33 | 25 / 11 | 3.0 / 3.3 | **156 / 46** | 320 / 381 | 83 / 41 |
| 7034[30:24] 5 / 20 (vendor 10) | 42 / 68 | 14 / 19 | **1.8** / 3.4 | 170 / 152 | 228 / 357 | 73 / 69 |
| 71B0[27:21] 15 / 60 (vendor 30) | 68 / 55 | 21 / 16 | 2.4 / 3.1 | 203 / 82 | 285 / 439 | 143 / 40 |
| 7158[6:0] 6 / 26 (vendor 13) | 39 / 59 | 11 / 23 | 2.1 / 2.8 | 80 / 183 | 217 / 334 | 18 / 249 |
| 8028 halved | 52 | 11 | 2.5 | 176 | 410 | 106 |

(8028 doubled: no complete capture.) Three 102 us windows per candidate
are few, so small differences are noise.

- **The rx-compensation fields written by `phy_set_rx_comp_new()`
  (`702C[7:0]`, `70A0[31:24]`) move the level the native AGC settles on**,
  consistently in both fields: -36 dB lowers the trapped radius (92 / 46),
  -24 dB raises it (140 / 156). These are the level offset for
  `agc_offset.h`; `70A0` has the larger effect. `7128[31:24]` shows no
  consistent direction.
- None of the fine-stage candidates makes the trapped gain consistent: the
  per-acquisition radius spread stays 180-440 codes. `7034 = 5` shortens
  acquisitions (1.8 us) without narrowing the spread. The varying trapped
  gain is a property of this AGC; finer IQ (P8 FINE) is the answer to it.
- The DAC is not a retrigger source: with the carrier present, native AGC
  state switching is the same with the DAC/PARLIO output electrically quiet
  (`S`: 14-16/ms active, 17/ms quiet); without a carrier the AGC idles at 83.
- Baseband packet AGC off (`9`, `7030[29]`) freezes the gain (clean IQ,
  coherence ~1.0) and the RF saturation block (`705C`) then does nothing,
  even at 41 % clip: it is an intervention into the packet AGC, not an
  independent loop.
- A lower start gain (`7094` 74) looked restart-free in one boot capture
  but live coherence and switching were unchanged; not a fix.

Sweep candidates apply only in probe builds (`CONFIG_C5VRX_PHY_PHASE_TAP_PROBE`);
production images always run the vendor AGC plus, when enabled, the offset.

**Live check of the offset (same day, A1).** With the self-calibrating
offset on `70A0[31:24]` the live P50 stayed at 7 across -4..+12 dB while
the calibrator hunted and flipped polarity; its clip input (20-28 pm) is
dominated by the saturated re-acquisitions, not by a high trapped level.
The sweep's radius differences were not confirmed by the live check; three
short windows per candidate are insufficient to establish a level effect.
Conclusion: the tested `70A0` offset did not measurably move the live settled
level in this setup; the offset is kept as a lab tool, off by default.

**Range interpretation and pending test (2026-09-30).** The displayed
`+12 dB` is a +12 change to the signed compensation field relative to its
vendor value, not a measurement of 12 dB extra RF gain or sensitivity. Its
effect on sensitivity has not been established. An unchanged level with a
strong carrier does not rule out a weak-signal benefit: hardware AGC could
keep the output level constant while a setting affects reception near the
range edge. This is a hypothesis, not a measured result.

To test it, compare fixed offsets 0 and +12 on the same field, with the
self-calibrator disabled, the same demodulator, channel, VTX power and RF
geometry. Alternate the settings at each controlled attenuation step and
record picture quality plus the attenuation at the same usable-picture
threshold. Verify the applied register value and repeat the comparison to
separate a reproducible effect from fading or AGC variability. Reception at
that threshold with 12 dB more attenuation would demonstrate a 12 dB link
margin improvement under those conditions; the offset label alone does not.
No weak-signal range comparison has been performed yet.

## Proposed suppression of AGC switching artifacts (2026-09-30)

**Operator observation:** Phase5 does not suppress the visible recurring
artifact either. Changing phase precision alone is therefore not an
established remedy for this artifact. This observation does not establish
that all visible grain comes from AGC: noiseless Q4 quantization already
produces colour-band error, as described above.

**Preferred research direction:** compensate repeatable gain-switch errors
before FM discrimination, then conceal only intervals whose phase cannot be
recovered. Native hardware AGC remains the gain owner. This is a design
proposal, not an implemented or hardware-validated fix.

For an ideal positive gain change, `z[n] = a[n] * exp(j * phi[n])`, the FM
discriminator `arg(z[n] * conj(z[n-1]))` is independent of `a[n]`. Actual
artifacts can come from a gain-dependent phase response, DC displacement,
settling, analog/ADC saturation, or quantization. Model these separately:

- If measurements establish a repeatable phase offset for each gain state,
  correct the phase difference by the calibrated offset change at the actual
  transition. Gain-dependent IQ centering or imbalance needs a corresponding
  calibrated IQ correction. A repeatable settling response may also be
  correctable, but its dependence on the input waveform must be checked.
- An event's timing being predictable does not prove that its error waveform
  is predictable. Do not subtract one fixed periodic waveform or apply a
  notch at the average acquisition rate: gain paths and intervals vary, and
  short disturbances have broadband energy overlapping wanted video.
- Fine-lane overflow outside +/-256 is not proof that the signed 10-bit ADC
  sample has saturated. Determine which errors are introduced by selecting
  or quantizing lanes before declaring the underlying phase unrecoverable.
- For genuinely unreliable intervals, suppress the faulty discriminator
  output and re-prime phase history from reliable adjacent samples before
  resuming. A minimal causal hold is a useful baseline, but holding through
  every 2-3 us acquisition erases many colour cycles and can damage sync.
  Buffered interpolation is another concealment candidate; neither approach
  can recover arbitrary detail from a multi-microsecond information gap.

General precedents, not C5 hardware evidence:
[gain-transition phase compensation](https://patents.google.com/patent/US7889820B2/en)
and [FM impulse detection with interpolation](https://patents.google.com/patent/US10404301B2/en).
The latter addresses broadcast FM audio/MPX; its successful interpolation
assumptions cannot be transferred directly to MHz-bandwidth composite video.

### Evidence required before selecting the live implementation

1. Capture complete, ordered Q10/I10 plus gain/state words over many native
   acquisitions with a known RF stimulus. Compare high-resolution phase
   behavior with the actual coarse/fine lane mappings on the same samples.
   Keep capture windows separate. Measure transition timing, settling,
   gain-conditioned IQ centres and phase errors, and distinguish ADC clipping
   from lane overflow and coarse quantization.
2. Determine how much of the error repeats for the same gain transition on
   independent captures and different video content/RF levels. Calibrate on
   a known stimulus; avoid learning wanted picture edges as a noise template.
   Compare selective correction and minimal concealment against the existing
   uncorrected demodulators, including sync, burst, colour and fine detail.
3. Prove that the required gain/state information can be captured alongside
   live IQ with the correct sample alignment. Dump-word metadata and static
   status-bit matches alone do not prove this. An IQ-only fallback detector
   must distinguish quantized FM rotation and real sync/colour transitions
   from AGC disturbances; amplitude or phase-jump thresholds alone are not
   sufficient evidence.
4. Fit the selected operation into the sustained two-bundle hardware path
   with continuous state and full output cadence. PHASE8_FULL currently uses
   arithmetic that conflicts with adding a branch; compensation, buffering
   and interpolation are not claimed to fit. Redesign the representation if
   useful, but demonstrate throughput and useful precision before enabling.

The available `probe_boot.log` has packed lane-sweep captures, not complete
`AGC_WORDS` windows. It cannot establish a transition-error model. A new
full-word capture is required before claiming a calibrated cancellation or
an optimal concealment interval.

## Initial implementation: rail HOLD/RESEED and calibration tools

`AGC GUARD` is an opt-in lab demodulator, selected via VIDEO OUTPUT or serial
`!` (persist and reboot). It uses coarse Q4/I4 and the same codebook and
transfer as VIDEO32, so VIDEO32 is its unguarded comparison. VIDEO32 itself
was reported by the operator to provide no useful filtering of this grain.
The new operation is selective HOLD/RESEED; no filtering benefit is inferred
from the reused codebook. Default remains PHASE8 FULL (`d` to restore).

The raw-IQ LUT flags an endpoint if either signed nibble is -8 or +7. These
are rail-risk cells, not proven ADC clipping or an acquisition flag. Other
cells, including central cells, are not rejected. A flagged endpoint holds
the last emitted DAC value. The first subsequent unflagged endpoint also
holds the DAC while seeding phase history; the next reliable pair resumes
normal discrimination. Both middle samples and rejected endpoints continue
to be consumed at the existing rate; this does not implement adjacent-40 FM.
Every branch takes exactly two bundles per output pair. The program uses
five instruction slots, a 1024-entry 16-bit LUT, duplicated 20 MS/s unique
DAC values and continuous state across DMA boundaries. Startup values are
untrusted until the pipeline has acquired reliable endpoints.

This detector can reject legitimate outer-cell samples, and can miss AGC
errors that do not reach a rail cell. Sustained flagged reception holds the
output indefinitely and can lose sync. It cannot detect fine-lane folding,
so this mode explicitly selects coarse lanes. It is not calibrated phase
compensation, does not change AGC gain/timing, and has no proven picture
improvement or sustained-hardware throughput validation yet.

Serial `@` explicitly arms one full-word boot capture and reboots. The NVS
request is cleared before capture, the offset is zeroed, and eight separate
8192-word IQ/gain/state windows are printed before normal video resumes.
The existing probe capture is reused; the critical SRAM-ownership interval
is in IRAM and the stop pointer is read after disabling the writer. Dump
control and SRAM ownership are restored. USB never paces live video; this
measurement intentionally runs before starting live DMA. The RF dump clock
is still assumed to be 80 MS/s, consistent with previous measurements.

```sh
python tools/agc_transition.py capture --port COM10 --log tone-train.log
python tools/agc_transition.py calibrate tone-train.log --tone-khz 0 --output tone.json
python tools/agc_transition.py capture --port COM10 --log tone-eval.log
python tools/agc_transition.py evaluate tone-eval.log --model tone.json
```

**Calibration requires an unmodulated tone of known baseband frequency.**
A constant video image still contains sync and colour burst and does not
satisfy that requirement. Do not use an ordinary VTX video waveform with
`--tone-khz 0`. Known waveform/reference alignment for a video test-pattern
calibration is not implemented. A connected board alone cannot supply the
required calibration stimulus.

The offline model learns a phase-error response per ordered gain transition
over -4..+32 samples, keeps only entries observed at least eight times with
small circular dispersion, and excludes intervals crossing another switch.
It distinguishes signed 10-bit endpoint rail risk from the +/-256 fine-lane
limit. Parsing requires complete ended windows with ordered unique chunks;
windows are never concatenated. Duplicate training windows are removed,
and evaluation rejects training hashes. Tone residual RMS is not a video
quality score. Models are not loaded into the live firmware: aligned gain
metadata and a demonstrated realtime correction path are still required.

Build status at implementation: the pinned ESP-IDF v6.0.2 C5 assembler
accepted the five-instruction guard. Full firmware compilation and flashing
remain pending because this session cannot access Docker/WSL. No firmware
image or bench validation is claimed from assembly alone.

## Native acquisition acceleration experiment

The current priority is to shorten the native hardware gain walk itself for
continuous analog FM. Native AGC stays enabled and makes every gain decision.
The 0.6 us observation is gain-step spacing within an acquisition, not the
period of an entire acquisition. Improving that spacing must also reduce the
total time with bad IQ; a shorter walk with more restarts is not automatically
an improvement. Settling, false acquisitions, trapped IQ quality and picture
must be measured as well.

The pinned ESP-IDF [PHY library submodule](https://github.com/espressif/esp-phy-lib/tree/59c1234e929212aec0fdda75769b759951235536)
provides C5 `libphy.a`; the gain walk is a hardware state machine rather than
a public C loop with a configurable software delay. The public
[C5 PHY initialization data](https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_phy/esp32c5/phy_init_data.c)
does not expose a named 0.6 us acquisition-step setting. Vendor register
initialization can be overridden after calibration without replacing native
gain ownership. This implementation uses that route; it does not patch or
redistribute a modified vendor binary.

The native acquisition profiles change one seven-bit field per boot:

| Index | Profile | Earlier acquisition-duration evidence |
|---|---|---|
| 0 | vendor | baseline ~3.4 us in the original short sweep |
| 1 / 2 / 3 | `7034[30:24]` = 5 / 2 / 1 | 5 gave ~1.8 us; 2 and 1 unmeasured |
| 4 / 5 / 6 | `7158[6:0]` = 6 / 2 / 1 | 6 gave ~2.1 us; 2 and 1 unmeasured |
| 7 / 8 / 9 | `71B0[27:21]` = 15 / 5 / 1 | 15 gave ~2.4 us; 5 and 1 unmeasured |
| 10 | `7034[30:24]` = 127 | maximum encoded value; timing unmeasured |

These are acquisition-policy candidates, not proven time constants. Smaller
values are a hypothesis for faster acquisition; no value is labelled 0.1 us.
Zero is excluded because an undocumented field's zero behavior is unknown.
The earlier captures are too short to establish a repeatable improvement.

Serial `{` advances to the next profile, atomically clears the older sweep,
start-gain and offset overrides, requests eight boot captures, and reboots.
Serial `}` does the same with vendor profile 0. The profile is persisted for
live picture comparison, applies after vendor initialization and on retunes,
and defaults to vendor when no profile was explicitly selected. Older probe
sweep selection takes precedence rather than silently combining candidates.
`AGC_ACQ` records index, field, register before/after, channel and frequency.
Default demodulation is unaffected; compare with the same demodulator and RF
setup throughout. Restore vendor after each unsuccessful candidate.

```sh
python tools/agc_transition.py capture --timing vendor --log vendor.log
python tools/agc_transition.py capture --timing next --log 7034-5.log
python tools/agc_transition.py report vendor.log 7034-5.log
```

The timing report measures gain-step intervals *within* each change cluster,
cluster duration and rate, occupied-sample share, 10-bit endpoint rails and
fine-lane overflow. Capture windows are never joined to create fictitious
intervals. Grouping changes separated by at most 4 us is an event proxy, not
a decoded acquisition state. Last-change-minus-first-change omits final
settling, which still needs phase/state correlation. Unlike phase calibration,
this timing comparison can use a normal VTX kept on one fixed channel.

No fast profile has yet been measured on hardware or enabled by default.

### Full build and flash (2026-09-30)

After restoring interactive approvals, ESP-IDF v6.0.2 compiled and linked
the normal firmware successfully. `c5vrx3.bin` is 1,165,168 bytes (0x11c770),
4,704 bytes larger than the previous image; the app partition has 63% free.
Implementation commit `faf9388` was pushed to PR #122 and flashed on COM10.
Esptool verified the bootloader, partition table and application hashes.

Post-flash `E` telemetry confirmed A1/5865 MHz, `native=1`,
`fw_gain_epochs=0`, `agc_tune=0`, `agc_off_en=0`, `agc_off_db=0`,
`demod=PHASE8_FULL`, and zero reported RX/TX/GDMA errors. This sample had
`class=COLLAPSE`, `p50=1`, `coh_pm=0`, `agc_state=82`: no usable video
carrier was established. This confirms firmware operation, not a noise
improvement. Carrier-present timing comparisons and AGC GUARD validation
remain pending.

### Capture memory failure and standalone meter

The first carrier-present normal-build capture crashed in `esp_vfs_write`
after the dump overwrote live BSS. Saving only the printed 32 KiB window
also failed: the interrupt stack above that window was corrupted. The
legacy `c5vrx2/main/rf_dump.c` explicitly records that `MAC_DUMP_ALLOC`
disconnects **both** 64 KiB banks, through 0x40850000, from the CPU. A
temporary heap/RTC backup is not a valid replacement for reserving those
banks, especially when executing stacks and driver buffers live there.
Those temporary backup attempts were discarded.

Raw AGC capture is now refused in normal builds (`@` returns
`ESP_ERR_NOT_SUPPORTED`). `{` / `}` still select native profiles and reboot
for live picture comparison, but no longer request a raw dump. A separate
`CONFIG_C5VRX_NATIVE_AGC_CAPTURE_ONLY` image excludes video and reserves
0x4082ffc0..0x40850040 before heap initialization, checks static BSS ends
below that region, and sanitizes stale dump ownership before Wi-Fi starts.
It deliberately produces no analog video. Restore the normal image for
picture comparison; NVS retains the selected profile.

```sh
idf.py -B build_agc_meter -D SDKCONFIG=sdkconfig.agc-meter \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.agc-meter.defaults' build
python tools/flash.py COM10 --build-dir build_agc_meter
python tools/agc_transition.py capture --timing vendor --log vendor.log
python tools/agc_transition.py capture --timing next --log 7034-5.log
python tools/flash.py COM10
```

Both images compile with ESP-IDF v6.0.2. Normal image: 0x11c350 bytes;
meter before the export pacing adjustment: 0xd4300 bytes. The meter was
flashed and ran capture without panic. Initial USB export lost a chunk;
strict parsing rejected the session rather than silently accepting it.
Export now flushes and yields between chunks, entirely after sampling.
Disable clears the hardware stop pointer, so it is read immediately before
disable again; a few bus cycles of boundary uncertainty remain. No precise
gain-to-phase calibration or noise improvement is established by these
transport checks. Carrier-present profile comparison remains pending.

The user requested direct live assessment instead of further meter work.
Normal firmware was restored and its flash hashes verified. Native profile
1 (`7034_5`) was selected with `{`, without capture, and the receiver was
returned from saved A3 to A1/5865 MHz. Live `T` readback confirmed
`0x600A7034 = 0x850187a4`, i.e. `[30:24] = 5`. `E` confirmed
`native=1`, `fw_gain_epochs=0`, `demod=PHASE8_FULL`, `agc_off_en=0`,
and no reported RX/TX/GDMA errors. VTX was requested on only after this
verification. Picture improvement and actual step-duration reduction are
still unproven. `}` restores vendor in the normal firmware without a dump.

**Live result:** the user reported "nope niks beter" for `7034_5` on
2026-09-30. No visible grain/noise improvement was observed with native AGC,
P8 FULL and offset disabled. This session has no valid matched full-word
timing comparison, so it does not establish the actual gain-step spacing or
reject every faster-AGC candidate. It does reject promoting `7034_5` based
on the earlier short acquisition-duration sweep alone. Vendor profile was
restored immediately with `}`; readback `0x600A7034 = 0x8a0187a4`
confirms `[30:24] = 10` again. The independent watchdog/reset audit in
`libphy-native-agc-audit.md` was merged without altering its findings.

### AGC GUARD live picture comparison (2026-09-30)

Serial `!` selected guard mode on A1 with a carrier. `E` confirmed
`demod=AGC_RAIL_GUARD`, coarse IQ, native AGC, vendor acquisition index 0,
offset disabled, and zero reported RX/TX/GDMA errors. The user reported:
"het is schoner, maar wel veel meer glitches en static". Cleaner appearance
does **not** make this version a usable improvement: the added artifacts
outweigh it. Serial `d` restored P8 FULL immediately.

This supports investigating selective rejection, but does not prove all
grain comes from clipped samples or that the guard detects AGC events.
Its rail-cell test is coarse and can reject valid FM/video samples; repeated
rail flags hold output indefinitely, then reseeding discards another pair.
Removing enough composite waveform can disturb sync/chroma. This is a
plausible explanation for the added glitches, not a demonstrated diagnosis.
The comparison also changes the demodulator/codebook from P8 FULL to
VIDEO32, so the perceived cleanliness cannot be attributed solely to hold.
An unguarded VIDEO32 comparison and bounded-duration rejection would be
needed to separate those effects. Do not enable this guard by default or
claim a noise fix. Hardware runtime observation is not sample-gapless proof.

### Lowest nonzero 7034 candidate selected

At the user's request to try the fastest available candidate, normal live
profile 3 (`7034_1`) was selected. Intermediate profiles 5 and 2 were used
only to reach the persisted index; no new picture assessment of 2 is claimed.
USB console recovery required a hardware reset, after which `T` confirmed
`0x600A7034 = 0x810187a4` (`[30:24] = 1`), profile 3, A1/5865 MHz.
`E` confirmed native AGC, P8 FULL, coarse IQ, offset disabled, and zero
reported RX/TX/GDMA errors. The initial snapshot had strength 0 and no
usable carrier, so carrier-present picture assessment remains pending.
Value 1 is the lowest nonzero setting in this field, **not a demonstrated
maximum hardware acquisition speed**; no 0.1 us timing is claimed. Other
AGC policy fields remain vendor, and `}` restores profile 0.

### Maximum 7034 candidate

The user requested the highest setting after selecting 1. Appended native
profile 10 is `7034_127`, the maximum value of the seven-bit `[30:24]`
field. Appending preserves all previous NVS indices. Serial `*` selects it
directly, clears older sweep/start-gain/offset overrides and reboots. Native
AGC remains enabled; `}` restores vendor. The update compiled successfully
with ESP-IDF v6.0.2 (normal image 0x11c8a0 bytes), and all 214 repository
architecture checks passed. Initial COM10 flash attempt timed out; readback
and picture assessment are pending. Maximum encoded value must not be
called maximum speed: the field's time/control units remain unknown.

After the user put the board back into download mode, the normal image was
flashed on COM10 and all flash hashes verified. Serial `*` armed maximum
profile 10 successfully. Live `T` then confirmed `0x600A7034=0xff0187a4`,
`[30:24]=127`, `name=7034_127`, A1/5865 MHz. `E` confirmed native AGC,
P8 FULL, offset disabled, coarse IQ and zero reported RX/TX/GDMA errors.
The readback snapshot had strength 0/no usable carrier; VTX was requested
on for direct picture assessment. No timing or image improvement is yet
claimed for the maximum candidate.

The user subsequently reported apparently very fast response and possibly
slightly less noise, then explicitly requested 127 as the default from now
on. Normal builds now default to native profile 10 (`7034_127`) when the NVS
selection is missing or invalid. An explicit persisted selection remains
respected, including `}` for vendor comparison. The board already stores
profile 10; it survives reboot and retune. Old probe-only sweep NVS no
longer suppresses this default in a normal image. The standalone meter
keeps a vendor default to support independent comparisons.

This is a user-selected live preference, not a calibrated speed/range proof.
Five subsequent live telemetry snapshots reported native state 83 without
observed changes, P50 3, coherence 6..11 per mille and strength 2. They did
not provide a valid sub-microsecond timing measurement. Register polling
also includes a 2 us delay per sample, so it cannot resolve the proposed
0.6 us versus 0.1 us gain-step spacing. Do not derive an acquisition time
from those counters or equate the register value 127 with a speed.
