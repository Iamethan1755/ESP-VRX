# Analog Lock and C5 receive-state lab (#150–153)

C5VRX by Twotoz and the C5VRX contributors.
[Source](https://github.com/Twotoz/C5VRX),
[official website and Discord invite](https://twotoz.github.io/C5VRX/).

This implements the software preparation for #150–153 and fixes the concrete
Direct Gain offset ownership bug. **It does not establish a range gain, a
matched analog passband, or absence of short RX interruptions.** No C5/VTX/
attenuator was connected to the implementation environment.

## Production fix

The pinned `phy_chip_set_chan_offset()` tail calls `phy_enable_agc()`.
Re-forcing an RX gain afterwards did not disable BB AGC. Every supported
startup, channel, frequency-offset, front-end bandwidth and FFT-lab transaction
now uses one `analog_phy_restore_lock()`:

1. Save pre-overlay vendor register state for comparison.
2. Reassert continuous MODEM_DIAG and all five receive-only TX queue gates.
3. For Direct Gain, disable BB and RF AGC, restore the selected front-end width,
   and force the current generated gain-table index.
4. For native AGC, retain vendor ownership and release forced gain; never disable
   either AGC loop or select an index.
5. Publish a receive-policy generation after restoration. Invalidate software
   sync/AFC history and reject stale fast-observer actions across generations.

Startup calibration, ADC/filter coupling, IQ/DC correction, calibrated analog
filter codes and gain tables remain vendor-owned. There is no blanket packetless
production override. Repeated identical offsets are no-ops. A lab override is
restored **before** every vendor transaction, so a saved mask cannot migrate to
another frequency. A failed public tune verification also restores policy.

## Reproducible static evidence and corrections

ESP-IDF v6.0.2 uses esp-phy-lib
`59c1234e929212aec0fdda75769b759951235536`. The exact C5 archive hash is
`dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb`.
`tools/extract_phy_rx_evidence.py` verifies that hash and uses the Espressif
RISC-V objdump to produce [symbol excerpts](phy-rx-evidence.txt):

```sh
python tools/extract_phy_rx_evidence.py \
  "$IDF_PATH/components/esp_phy/lib/esp32c5/libphy.a" \
  --objdump riscv32-esp-elf-objdump --output /tmp/phy-rx-evidence.txt
```

| Control | Recovered ABI/effect | Implementation boundary |
| --- | --- | --- |
| `phy_chip_set_chan_offset(int)` | Compound correction/LO transaction, unconditional AGC-enable tail | Restore Direct Gain owner afterwards |
| `phy_disable_agc()` | Set `600A7030[29]` | Established ownership gate |
| `phy_wifi_fbw_sel(uint32_t)` | **`600A0874`**, clears masks `00090000`, `00060000`, `00300000`; nonzero sets `00120000` | Issue #150's `600A0814` address is incorrect for this function; snapshot both addresses |
| `phy_bb_bss_cbw40_dig(uint32_t)` | `600A9C18`: clears **both** bits 2/3, sets bit 2 from argument bit 0 | Issue #151's single-bit description is incomplete |
| `phy_bb_bss_cbw40(mode)` | Mode 0: digital 0/front 0; mode 1: digital 1/front 0; mode 4: digital 0/front 1; other nonzero: digital 1/front 1 | Not a boolean BW40 ABI; do not invent a private "full BW20" call |
| `phy_bb_cbw_chan_cfg(code)` | Packed input decodes fields in `600A4400`, `600A7CE0`, `600A7CE4` | Read-only here; public Wi-Fi API owns complete width transitions |
| `phy_rfpll_set_adc_rate()` | Normal 5 GHz branch changes filter mode at 5830 MHz, ADC selector remains 1 (re-read 2026-10-04: selector 0 up to 5830 MHz, 1 above; see `PREDEMOD_LAB.md`) | Record coupled tuple; no forced mode-4/8 production change |
| `phy_chan_filt_set(a,b)` | A=0 sets bit22 and clears low3 at `600A7904`; A!=0 clears bit22. B=0 sets bit13 at `600A7074`, B!=0 clears it | Isolate each final state; neither polarity means proven bypass |
| `phy_noise_floor_auto_set()` | Set `600A7018[23,28]`, `600A7C44[0]`, `600A7C50[0]` | Independent temporary mask experiment |
| `phy_pkdadc_set(a,b)` | Includes `600A0C38[31]`, threshold and secondary controls | Test enable bit only, retain thresholds |
| `phy_spur_reg_write(index,code)` | Changes slot control **and coefficients**, not a one-register setter | Only slot0 enable-bit experiment; no arbitrary indices or zero-coefficient replay |
| `phy_i2c_readReg(block,host,reg)` | Three arguments; chip wrapper obtains host/mask internally and forwards register from a2 | Explicit bounded block-67 read, never periodic polling |
| `phy_filter_dcap_set()` / `phy_rc_cal()` | Calibrated analog register bank, source bytes `phy_param[F5..FC]` | Snapshot, do not infer MHz or write guessed capacitor codes |
| `phy_rx_table_track()` / `phy_get_adc_rand()` | Empty returns in this archive | No disable patch needed |

The vendor chain is:
`phy_chip_set_chan` → `phy_rfpll_set_adc_rate` → ADC/filter setters;
`phy_set_chan_misc_new` → `phy_set_chan_reg` → `phy_bb_bss_cbw40` → digital and
front-end helpers, plus RX compensation; and a separate
`phy_bb_cbw_chan_cfg` call. A front-end toggle demonstrably does **not write the
same set of registers**. Whether extra stages affect MODEM_DIAG or usable
analog range remains a hardware question.

The local final ELF built with tracking disabled has no linked definitions for
`phy_param_track_tot`, `phy_cal_param_track`, or `phy_track_pll_timer`. The
force-off helper is linked because tuning needs it. This rules out these
particular archive definitions in that build, **not** other ROM/driver paths,
short hardware gating, or future library versions. OSI counters observe calls
through the copied Wi-Fi table only; they are not a universal PHY-call trace.

## Console workflow

| Key | Action |
| --- | --- |
| `H` | Existing ARC report plus MMIO vendor-before-overlay / LOCK / current values, change count, timestamp, ownership bit and OSI counts |
| `{` | Enter quiet MANUAL/BW40/AFC-off baseline and take one explicit analog-I2C/calibration-byte snapshot; refuses native AGC/menu/gain-sweep |
| `}` | Toggle opt-in 50-ms MMIO monitoring and baseline current state |
| `L` / `l` | Existing glitch marker plus timestamp and last 64 captured PHY events |
| `[` | Enter quiet baseline, apply next **single isolated** profile for at most 10 seconds |
| `]` | Immediately restore stock owned fields |
| `W` | Existing fixed-gain front-end-only BW40/BW20 A/B, now also dumps complete register state |
| `B` | Fixed-gain **public vendor BW40/BW20 + retune** A/B, dump pre-overlay/full state, restore original public width, front-end width and offset |
| `p`, `Q`, `t`, `K` | Existing video/Q4 row, raw IQ, vendor timers, fresh-calibration-on-next-boot |

`B` uses `esp_wifi_get/set_bandwidths()` and a normal channel transaction rather
than calling private helpers with guessed tuples. A failure still runs
restoration; failure to restore the public width aborts via ESP_ERROR_CHECK
rather than resuming a mixed production state. It does not assume the public
setter changes every filter; compare the captured registers to establish that.
Automatic A/B rows characterize only the fixed RF level connected at that
moment. Perform separate attenuator steps for a sensitivity threshold.

The monitor has no extra task, ISR, UART stream or per-sample processing. It
polls from the existing slow controller, buffers **changes only**, and dumps
only on an explicit marker/report. The bounded ring overwrites old entries;
`events` is the total sequence count. Event address 1/2 means OSI enable/disable,
3 transaction begin/end, 4 lab profile/timeout, 5 user glitch marker. A transaction
rebaselines intentional register changes, retains lifetime change counts and
publishes a generation. `vendor` means state immediately before C5VRX overlay,
not necessarily an untouched cold calibration state at every later transaction.

**Limits:** minimum 50-ms polling can miss a microsecond pulse, and is delayed
while a console probe occupies the same task. Unchanged registers cannot rule
out such interruptions. Call counts survive between polls, but do not identify
internal calibration calls. Snapshots are sequential MMIO reads, not simultaneous
hardware latches. Raw gain/status fields can change normally and fill the event
ring; correlate specific gates/watchdog bits rather than calling every change a
reset. Explicit analog I2C reads may perturb control timing; exclude that interval
from any clean range/video measurement. No capture data has been fabricated.

## Isolated lab profiles

The cycle is PKDADC bit31 clear → NF-auto freeze → BB-CCA bit30 clear → front BW
argument0 → digital BW argument0 → channel-A argument0/1 → channel-B argument0/1
→ spur slot0 bit13 clear. Every next profile first restores the previous one.
Reboot, retune, offset, bandwidth, FFT lab, entering the menu or another modifying
console command ends the override. Timeout works even with monitoring disabled.

Only the saved **owned mask** is replayed, preserving unrelated bits/status that
changed concurrently. Full-register replay could retrigger shared fields and
would be less exact. No coefficients, ADC rate, filter-mode tuple, RFAGC policy,
calibration loop or watchdog are swept. No combination is automatically promoted.
The current mask values are dumped by `H`; check readback before recording a row.
Writes and private I2C reads are compile-time enabled only when CMake verifies
this exact archive hash. Unknown archives keep read-only MMIO/OSI diagnostics.
The public vendor-width experiment uses the documented API and is separate.

## Completion ledger

| Issue | Completed in software/static analysis | Still requires hardware or additional ABI work |
| --- | --- | --- |
| #150 | Effective local config checked; pinned evidence extractor/map; stock/current filter/spur/IQ/scaling/ADC snapshots; safe calibrated defaults; reversible lab masks | Live baselines, passband/blocker/overload tests, drift, attenuation/range measurements; actual DC/CFO tap ordering |
| #151 | Vendor chain documented; corrected BW register/masks/mode ABI; front/digital/channel/spur isolation; bounded analog snapshot; public vendor BW+retune comparison | Full measured transition/state captures, sufficiency at tap, ADC/filter modes4/8 and capacitor response, packet-control classification, PAL/NTSC regression |
| #152 | OSI enable/disable counts/timestamps; all requested gate/clock/watchdog/RX-sense/BW/gain registers; after-policy LOCK baseline; change-only bounded events and glitch correlation | Several-minute edge recording, deliberate retune positive control, reproduce spontaneous transitions; optional internal/ROM call interposers |
| #153 | Offset ownership bug fixed; central receive-policy restore and generation invalidation; individual packet/NF/CCA experiments with stock fallback; no unmeasured production override | Attenuation table, strong-signal/overload/selectivity/sync comparison and proving runtime quietness |

Effective local IDF config: `ESP_PHY_DISABLE_PLL_TRACK=y`; station-disconnected
PM, general PM, IEEE802154 and software coexistence disabled. Runtime NVS still
selects native AGC versus Direct Gain; read `native` in the hardware report.

Validation includes normal ESP32-C5 firmware build, architecture validation,
existing host range/DSP checks, and pinned/unverified-library host lab tests for
all masks, unrelated concurrent bits, profile switching, timeout, retune,
nested transaction generations, native-owner refusal, monitor off by default,
OSI counters and bounded event-buffer wrap. Host lifecycle tests do not establish
physical register semantics or video quality.

## Hardware acceptance record

Fix gain tuple, IQ lanes, demodulator, offset, VTX/camera and scene. Match RF input
across A4/A3/A2/A1 (5805/5825/5845/5865 MHz), especially across 5830 MHz. Capture
`H`, `Q`, `p` before/after each profile. Run near/medium/edge, blockers and overload;
measure attenuation at equivalent full PAL/NTSC picture and sync-loss threshold.
For monitoring use `}` then several minutes at the edge, `L` at each glitch,
then deliberately retune for a positive control and use `H`/`L` again.

| RF setup / standard | Frequency | Profile | Measured input/attenuation | P50/P90/P95 / origin / fold | Sync/chroma/detail | PHY events / temperature | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Pending hardware | — | stock/fix-only/candidate | — | — | — | — | No dB claim |

Keep #150–153 open until their measurement criteria are actually met.

## Issue #155: reversible 11p A/B

Press `:` in Direct Gain mode to compare BASELINE, ENABLED and RESTORED at
one fixed channel, gain, bandwidth, offset and IQ lane. Gain/BW/AFC controllers
pause for the complete experiment; the live hardware video path continues.
Each stage settles for one second and measures four fresh 64-byte regions from
a completed DMA descriptor. These bounded Q4 statistics are not a measurement
of continuous phase quality, sync, chroma, range or received power in dBm.
Compare the picture physically and repeat with controlled attenuation.

The original reported improvement around 5.75–5.99 GHz is credited to
**SushiDude (@Ready4Sushi on X)**. The pinned helper contains no frequency test;
the reported frequency range is an observation, not a proven validity boundary.

For the pinned `libphy.a` SHA-256 above, `phy_11p_set(1,0)` writes:

| State | Owned mask / registers | Enabled value |
| --- | --- | --- |
| `phy_param` | offsets `0x26`, `0x27` | `1`, `0` |
| `0x600A7CE4` | `0x0000001c` | `0x00000010` |
| `0x600A7030` | bit 5 | clear |
| `0x600A7048` | `0x00007f00` | `0x00004000` |
| `0x600A71C4` | `0x00fe0000` | `0x00440000` |
| analog I2C block `0x67`, host `1` | registers 6–13 | all `60` |

The helper itself does not retune the PLL, rebuild the gain table or write the
ADC/filter tuple. The normal vendor channel helper can replay 11p from the
stored flags. A task-owned recursive PHY mutex therefore excludes other RF
transactions across all three stages. Vendor I2C calls run outside spinlocks.

Rollback restores the two saved flags, eight full saved analog bytes and four
saved MMIO fields, preserving unrelated bits. Calling `phy_11p_set(0,0)` would
install vendor defaults, which may differ from the previous calibration/state,
and is deliberately not used. Application and rollback have readback checks;
a failed application skips the candidate measurement, and failed rollback
reboots rather than resuming the controller over unknown PHY state. Native AGC
and unknown library revisions refuse the experiment. No NVS state is changed.

Host tests cover saved non-default flags/analog state, unrelated-bit changes,
failed rollback, native/unpinned refusal and competing-task exclusion. Hardware
quality, stability, attenuation and PAL/NTSC acceptance remain pending. Keep
#155 open until these measurements are recorded.

## C5VRX-4 integration

The isolated `experiments/c5vrx-4` target shares this entire PHY lab, RF restore
policy and controller generation handling. Direct Gain V5 is now its default;
`N` / RF-menu gain-owner changes persist only in namespace `c5vrx4` and reboot.
The optional native pacing gate is inert under Direct Gain, including suspend,
resume and `~`, so it cannot reopen native AGC during a profile or 11p A/B.
All keys above apply; native mode retains read-only diagnostics and refuses
Direct Gain lab writes. The span75 detector/output stays unchanged. Input Q4
metrics are bounded observations; shared Phase5 sync/chroma diagnostics are
not evidence of this detector's physical output. C5VRX-4 acceptance needs its
own hardware comparison, separate from C5VRX-3.

## Native analog patch prototype (#139)

Explicit native-only `(`/`)` commands test the reversible BB gate in both
firmware targets. This is a scoped exception to ordinary native zero-disable
operation; defaults and Direct Gain labs retain their original policy. No RF
AGC disable or forced gain is allowed in this prototype. See
[native-agc-analog-patch.md](native-agc-analog-patch.md) for the binary audit,
prior negative results, exact test sequence and pending PAL/NTSC acceptance.


## Range-edge control races fixed after the native audit

This extends C5VRX/Twotoz/contributors' existing Direct Gain V5 and PHY-owner
work, not a new demodulator or a measured sensitivity improvement.

Two concrete software paths could reduce usable reception around a retune:

1. The sentinel's overload mailbox stored only Q4 metrics. A strong-signal
   observation from the prior channel, lane or gain could survive a controller
   reset and issue an emergency gain cut on a newly selected weak channel.
   It now carries profile/PHY/gain epochs, rejects changed epochs, and expires
   after 1 ms. Fresh same-state overload still takes the immediate path. The
   inactive consumer no longer clears a mailbox while its producer owns it.
2. `apply_rx_gain_tracked()` advanced the software index, timestamp and transition
   count before `rf_set_rx_gain()` could refuse a busy PHY write. The controller
   could therefore measure a different physical gain than the one it believed
   it had applied. RF gain writes now acquire the same recursive mutex as vendor
   retunes, without waiting or changing tune generation, and check the expected
   generation while holding it. Tracked state advances only after acceptance.
   Direct Gain V3/V5 holds this ownership over its lane-and-gain application;
   V2 also passes its pre-observation PHY generation to the actuator.

Host tests exercise stale strong input against a maximum-gain listener, fresh
emergency response, age bounds, same-task busy refusal, competing-task refusal,
post-retune stale-generation refusal, and unchanged generation for an accepted
actuator. Both firmware targets and the shared host suites must pass. The native
AGC gain-write refusal remains intact. These fixes do not change AGC targets,
gain tables, bandwidth defaults or demodulation, and provide no numeric dB claim.
Physical weak/strong/channel/lab transitions still need hardware acceptance.
