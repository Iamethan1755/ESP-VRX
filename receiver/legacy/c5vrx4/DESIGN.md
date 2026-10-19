# Integrated C5VRX-4 architecture

Extends C5VRX by Twotoz and contributors. The authoritative integration contract
is README.md; INTEGRATION_SOURCES.json records exact donor revisions.

The sample path is MODEM_DIAG Q4/I4 → positive-edge PARLIO RX → raw cyclic ring
→ one TX BitScrambler → original 6-bit resistor DAC. IQ and physical DAC rates
are 40 MS/s. Phase endpoints span 75 ns; two intervening raw sign pairs classify
winding. The three-bundle schedule outputs `[D,D,D]` with 13.333 MS/s unique
video. The 524,386,048-case oracle assumes each adjacent phase step is within
63/256 turn; it does not recover missing ADC bits or prove RF continuity.

Final voltage is mapped after winding resolution and saturates. Detector span,
RF amplitude and loaded DAC voltage are separate quantities. STATIC/HISTORY
and fixed/legacy voltage choices are boot-selected programs; default has no running LUT
reload temporarily changes its width. Unknown trajectory maps to blank reference.

The supervisor owns gain, channel/BW/offset and diagnostics. Direct Gain V5
operates on fresh descriptor regions and shares serialized PHY actuator ownership
with retunes. Mailbox epoch/age checks prevent old strong-input observations from
cutting gain on a new weak signal. Clean IQ performs zero gain writes; no carrier
returns to the vendor table's maximum listening state. Severe coarse clipping
uses the controller's G20 floor, with the existing post-drop freshness check.

Adaptive lane upgrades require two overlap observations and one step at a time.
Recovery is immediate and observations are excluded for 210 us after routing.
Six sequential magnitude-selector writes preserve sign, output enable and
inversion, but cannot provide an atomic switch or sample transition tag. Decoder
history is not reset at physical DMA boundaries or lane changes.

Slow sync scoring decodes a frozen stride-3 trajectory/transfer estimate. It is
not a byte-exact live alignment/history trace or carrier/field lock. AFC V2 uses
all captured adjacent raw phase pairs for an independent burst/timing reference,
excludes color burst from porch CFO, requires stationary/fresh epochs, and has
its own sticky video TRACK. Gain HOLD alone neither proves video lock nor blocks
AFC acquisition. AUTO is opt-in and at most four bounded steps are allowed.
AFC actuator ownership is acquired before a second epoch check and decision.

Native AGC is never mixed with firmware gain ownership. Its pacing and explicit
BB-hold lab are optional, reversible comparisons; there is no automatic native
PAL/NTSC reacquisition controller. Pinned PHY experiments restore vendor state;
unverified libraries cannot use recovered write masks.

All firmware source dependencies are inside this directory. Repository C5VRX-3
main, root configuration, website and workflow files are unchanged. The existing
alpha workflow invokes this project; configure-time verification enforces local
host regressions and generated consistency without adding a shared workflow.

The opt-in `u` level lab adds a bounded sync/black servo after winding, using
DAC-only LUT16 writes. See CVBS_LEVEL.md; live arbitration is not yet proven.
