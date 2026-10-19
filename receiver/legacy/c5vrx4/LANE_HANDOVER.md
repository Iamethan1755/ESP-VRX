> Donor research record. Current integration defaults and scope are in README.md and INTEGRATION.md.

# Protected lane handover with persistent Phase8 history

This implements a bounded improvement to the live lane switch, not a proven
seamless transition. The current eight IQ wires carry no lane ID or transition
tag. One previous-phase semicircle cannot identify the first post-switch sample
or correct arbitrary mixed GPIO mappings. No such capability is claimed here.

## Implemented changes

- V5 still decides desired gain and lane. Finer routing requires two fresh,
  descriptor-deduplicated sample windows whose signed I/Q nibbles are all in
  [-3,2]. The adjacent finer lane accepts source cells [-4,3]; the tighter
  interval leaves one source-cell margin. Windows sample 256 bytes, not the
  complete RF stream; they are evidence, not a bound on later samples.
- Upgrade one lane step at a time. Drops to coarser lanes for overload/folding
  are never held for overlap evidence. Emergency observations cannot trigger
  an upgrade without fresh overlap samples.
- Keep controller lane state equal to the actually accepted routing choice.
- The experiment's RF router updates only six changed magnitude selectors,
  pairing Q and I for each bit. Both sign selectors remain untouched. Changes
  use the official IDF v6.0.2 GPIO output-selector layout, preserve output-enable
  and inversion fields, and run in a short CPU critical section. Upgrade order
  is low-to-high magnitude bits; recovery is high-to-low.
- Six writes are still sequential. DMA, PARLIO and the BitScrambler continue;
  no IQ replacement, stream pause, counter reset or runtime LUT update occurs.
- Ordinary V5 observation is held for 210 us after a routing change, covering
  the age and span of a completed 4092-byte descriptor at 40 MS/s. This avoids
  learning from windows spanning the old/new mappings. The fast overload
  sentinel remains active and its recovery path is not held.

## Role of phase history

The existing HC8 program keeps its previous phase across lane changes, exactly
as it already did in PR #142. Decoder geometry is in selected-lane units; the
same cell model is used on all three lane scales. Preserving angle context may
help with near-origin quantizer differences, but the bounded semicircle prior
cannot guarantee continuity. No lane-aware or tagged first-sample decoder was
added. Such a decoder needs a demonstrated way to convey lane/transition
information within the input/LUT/instruction constraints.

This experiment defaults history on via a distinct `c5vrx4/lane_hc` key; the
prior `phase8_hc` comparison key is preserved. Serial `H` toggles `lane_hc`
and reboots, so STATIC/HISTORY can still be compared under the same handover
policy. V5 remains the default gain owner; native remains optional.

## Observability and limits

`T` additionally prints `C5VRX4_LANES`: current lane, history mode, physical
switch count, last source/destination, maximum measured route-write duration
in microseconds, overlap evidence count, deferred requests, accepted upgrades,
recovery requests and skipped stale windows. It explicitly prints `atomic=0`
and `transition_tag=0`. Controller lane-change counters can count requests
which this guard deferred; the physical switch count is the routing evidence.

Stricter overlap gating can delay fine-lane entry, particularly with high
noise. Register-write duration does not measure the lifetime or content of
mixed captured samples. Hardware comparisons must assess switching artifacts,
fold recovery, weak-carrier acquisition and useful range. No seamless output,
latency improvement or additional range is established by a successful build.

Production C5VRX-3 routing and controller policy retain their original paths;
all new routing/observer hooks are guarded by `C5VRX4_EXPERIMENT`.

No tests were added or run. See the build record in the PR description.
