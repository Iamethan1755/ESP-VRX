# Native C5 AGC for continuous PAL/NTSC: binary audit and patch prototype

C5VRX by Twotoz and the contributors; extends Twotoz's prior receiver
research and the existing #139 sample-and-hold proposal. This work belongs to
PR #154. It is a software/binary investigation and bounded lab implementation,
not a hardware-proven replacement analog AGC or a measured range improvement.

## Evidence boundaries

Audited ESP-IDF v6.0.2, esp-phy-lib revision
`59c1234e929212aec0fdda75769b759951235536`, C5 `libphy.a` SHA-256
`dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb`.
The reproducible extractor now includes the native AGC initializers, gates,
force controls, reset and calibrated-table functions in
[phy-rx-evidence.txt](phy-rx-evidence.txt).

Prior hardware evidence takes precedence over register-name guesses. Read
[native-agc-v2.md](native-agc-v2.md), especially its native target sweep and live
offset check. Native gain metadata repeatedly walked during continuous-carrier
captures; frozen BB AGC produced stable IQ in that setup. The live compensation
offset experiment did **not** reliably improve the settled level. A signed
register change is not a sensitivity change in dB.

## Recovered controls

| Function / state | Exact C5 operation | Interpretation / limit |
| --- | --- | --- |
| `phy_disable_agc()` | Set `7030[29]`, return | BB acquisition gate only; no force-index command, table or RF-disable writes |
| `phy_enable_agc()` | Clear `7030[29]`; set then clear `702C[23]` | Resume includes a strobe, not just clearing the hold bit |
| `phy_force_rx_gain(force,index)` | Write index to `702C[31:24]`, force boolean to bit23 | `702C` is configuration, **not the current native gain** |
| `phy_rx_gain_force()` | Same force fields on 702C or 2840 depending on upper selector | Separate BB/RF force path; not used by the prototype |
| `phy_rfagc_disable()` | Zero `705C`; clear masks `0x07080000` and `0xf0000000` in `284C` | Separate destructive RF configuration; resume above does not restore it |
| `phy_agc_max_gain_set(max,arg1)` | `713C[24:18]=max-1`, `7094[8:2]=max-1`, `702C[14:8]=max`; 08BC[11:4]=max, [19:12]=(arg1+90)&0xff; set0444[11] | Coupled maximum/start/threshold and front-end-side setup; changing max is not a clean analog target patch |
| `phy_agc_reg_init_new(max,arg1)` | 702C top byte50 and strobe; 7128 top byte `0xd2`; call max helper; 71B0[27:21]=30, 7034[30:24]=10, 7158[6:0]=13 | Initialization fields, not proof of their physical units or an independent steady analog target |
| `phy_set_rx_comp_new()` | Selector `phy_param[0x2a]` chooses signed -30/-32 in 702C low byte and70A0 top byte; writes7064/7114 saturation words | Both signed fields are recalculated on channel setup; prior live offset test was negative |
| `phy_wifi_agc_sat_gain(word)` | Copy one whole word to7064 and7114 | Multiple saturation thresholds; not an independent gain decision loop |
| `phy_bb_fsm_rst()` | Pulse `7C28[1]` | BB reset control, distinct from AGC resume; not suppressed by this patch |
| `phy_rx_sense_set(value)` | Set7010/7014 upper9 bits and7044 low byte; toggle7108[9] depending on zero/nonzero | Multi-register detection prerequisite; not a verified analog tracking mode |
| `phy_rx_pkdet_num_set()` | I2C block67 reg29[6:4]=4; clear0C38[2:0], set[29], set7068 to0x808 | Mixed analog/digital detection configuration, not just one threshold |

`bb_agc_reg_update()` installs many fixed baseband words, including 8020=0x180,
8028=0xc0403020, 8010=0x852a1 and8018=0x600030. Their units and FSM meaning
are **not decoded**. Calling them durations or a ready-made PAL/NTSC mode would
be speculation. Its writes also reach 7044/7048/7104/7124, 9C18 and other fields;
replaying the whole initializer during video is not an isolated patch.

Espressif's [official CSI gain-control v0.1.5 API](https://components.espressif.com/components/espressif/esp_csi_gain_ctrl/versions/0.1.5/readme?language=en)
extracts gain from packet RX metadata. Its `RX_GAIN_READY` means baseline
calculation finished, not hardware acquisition completed. This packet API does
not supply the needed continuous analog acquisition-complete witness.

Two promising-looking getter names are also not live witnesses:
`phy_bb_gain_index(arg)` counts set bits in the low seven input bits and returns
max(popcount-1,0); `phy_rfrx_gain_index(arg)` compares its supplied code with one
of two nine-entry static tables selected by `phy_param[0x2a]`. Neither reads
live RF/AGC MMIO. We do not call these conversions acquisition-complete getters.

The gain-table generator/setter combines calibrated vendor state and writes
receive gain memory. Reshaping native landing states therefore needs a separate
ordered physical tuple / IQ/DC / transition comparison. We preserve this table.

## Call graph and reset policy

Direct RISC-V call relocations in the pinned PHY archive show:

- `phy_reg_init_new` -> `bb_agc_reg_update`, `phy_agc_reg_init_new`, `phy_bb_fsm_rst`.
- `phy_bb_init`, `phy_chip_set_chan`, `phy_chip_set_chan_offset`,
  `phy_wakeup_init` -> `phy_enable_agc`.
- `phy_chip_set_chan`, `phy_chip_set_chan_offset`, `phy_xpd_rf` -> `phy_disable_agc`.
- `phy_set_chan_reg` -> `phy_set_rx_comp_new`.

Also audited C5 Wi-Fi `libcore.a`, `libpp.a` and `libnet80211.a` from pinned
esp-wifi-lib `bb69e7e609c9a9a909ddd1ed175f36df2bd13801`: no direct call relocations
to named AGC/FSM helpers appeared. This does not rule out ROM, indirect calls,
inline MMIO or hardware FSM transitions. Static calls do not establish runtime
frequency. There is no evidenced per-packet C function we can simply remove to
stop the measured acquisition loop. The safe candidate is the decoded BB gate;
unknown state-table/abort fields stay unchanged.

## Implemented native acquire/hold/release experiment

Boot native with `N` (or RF menu) first. The two new commands work in C5VRX-3
and the C5VRX-4 target in this PR:

| Command | Operation |
| --- | --- |
| `(` | Free-running baseline, BB-only HOLD, free-running restored; each stage lasts one second for visual comparison |
| `)` | Baseline plus100 BB hold/resume cycles; each HOLD and resumed stage lasts40ms for both PAL and NTSC |

Gain/BW/AFC/menu control pauses and the recursive PHY transaction owns the
entire sequence. C5VRX-4 stops its native pacing ISR before the experiment and
restores the operator's paced/continuous choice afterwards. Unknown binaries,
Direct Gain, already-held/forced states and busy receiver labs refuse the test.
A forced state is identified from bit23; no gain index is inferred from702C.

Only the vendor BB disable/enable pair is used. There is no `phy_rfagc_disable`,
`phy_force_rx_gain`, table change, compensation patch, packet suppression or
periodic tracking timer in this prototype. The100-cycle command is a deliberate
reversibility stress test, not the production AGC cadence. Readback checks the
BB gate and cleared force/strobe bit; interference taints the result, restoration
always runs, and failed restoration reboots. No settings are persisted.

Each stage measures a fresh256-byte Q4 observation from four guarded completed
DMA regions. Reported 706C/7078 words are **state proxies** before/after the
observation, not a proven gain index, acquisition-complete flag, or transition
rate. These snapshots can miss whole gain walks. Q_phase has discontinuities
between the four regions; interpret it only as a bounded input comparison.
C5VRX-4's shared Phase5 semantic diagnostics do not measure its DAC output.

A host `completed=100` proves software sequencing only. The console's hardware
counter/readback also does not prove100 clean physical acquisitions; correlate
with full Q10/I10 gain/FSM metadata and actual recovered video.

## How this becomes an analog native controller

The patch candidate is native hardware acquisition followed by event-driven
BB hold. It can keep the calibrated native tuple without introducing a second
forced-gain transition. Continuous HOLD should have zero periodic writes.
This bounded prototype first tests the reversible actuator. An automatic
controller still needs:

1. Time-align a live acquisition-complete witness with full RF-dump metadata;
   7078 alone is not validated and earlier proxy counts undercounted restarts.
2. Verify hold/resume over100+ physical cycles, including RF overload and fade.
3. Independently observe genuine overload and persistent under-range while
   held. Q4 outer occupancy/folding is not automatically ADC clipping.
4. Permit immediate overload rearm; qualify ordinary weak-signal release and
   schedule it in a **measured** blanking interval where possible.
5. Validate PAL and NTSC separately. A CPU delay of16.7/20ms or a host oracle
   is not a detected vertical blank, and the C5VRX-4 output needs its own witness.
6. Confirm equal-or-better attenuation threshold, strong-signal recovery,
   colour/detail, latency, IQ/DC stability and no background vendor interference.

Neither the existing periodic native gate nor blind compensation sweeps solve
these prerequisites. We have implemented the first directly grounded native
patch actuator/test, not declared the full analog controller hardware-proven.

## Software verification

Both ESP32-C5 firmware targets build with ESP-IDF v6.0.2. All206 architectural
checks and the host range/demod suite pass. Pinned/unpinned lab tests cover100
hold/resume operations, unrelated-bit preservation, RF-control preservation,
wrong-owner/forced/already-held/invalid-cycle refusal, observed interference,
failed hold and failed resume, and transaction release after error. C5VRX-4's
native gate isolation test also passes. All physical acceptance above remains
pending; no build or host test proves a native PAL/NTSC picture improvement.
