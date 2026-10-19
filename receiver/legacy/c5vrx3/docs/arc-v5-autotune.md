# ARC V5 AUTOTUNE

ARC V5 keeps the proven ARC V3 controller as its safety floor and adds a
persistent, confidence-gated feed-forward model.

The goal is **faster reaction without faster hunting**.

```text
completed Q4/I4 window
        |
        v
persistent local gain-response model
        |
        +-- high confidence --> predicted multi-index move
        |
        +-- low confidence ---> ARC V3 search
        |
        v
VERIFY >= 500 ms
        |
        +-- wrong direction --> immediate bounded rollback
        |
        +-- coherent result ---> learn local d(Q4)/d(gain)
        |
        v
LOCK / HOLD (zero writes)
```

## What is learned

For each vendor gain region V5 stores the measured short-horizon response to a
+1 gain step:

- dP/dG
- dQ/dG
- dOrigin/dG
- dClip/dG
- observed settling time
- sample count / confidence

The learned values are **not a distance table**. They describe the receiver's
local actuator response. Transitions that look dominated by an RF fade,
blocker, no-carrier state, or severe clipping are rejected from learning.
Because vendor gain states are not linear in dB, only local moves of four
indices or fewer are allowed to update the persistent per-edge curve; larger
predictive jumps are verification-only evidence.

## Persistence

The model is stored in NVS under the `arc_v5/model` blob with:

- version
- vendor gain-table fingerprint
- generation counter
- CRC

Writes are rate limited to at most once per minute and only after multiple
accepted learning updates. A changed vendor gain table invalidates the stored
model automatically.

## Safety rules

1. NO_CARRIER never trains the model.
2. Diagnostics/manual gain own the actuator because V5 only ticks while AGC is ACTIVE.
3. Wrong-direction predictive moves can roll back during VERIFY, but accepted learning waits for at least the hardware-proven 500 ms settle interval.
4. Fusion CLEAN is an authoritative zero-write hold, even when V3's narrower classifier would prefer a trim.
5. Predictive confidence is required on the exact gain edge being extrapolated; neighboring confidence is not enough.
6. Low-confidence regions fall back to the existing ARC V3 behavior.
7. The live 40 MS/s path remains hardware paced; V5 runs only on completed control windows.

## Factory calibration

The measured ladder from ARC V3 hardware work remains a low-confidence prior:

```text
G16 -> G40 -> G54 -> G70 -> G78 -> G81
```

It helps recovery before the receiver has learned its own local response, but
only real hardware transitions build confidence for larger predictive jumps.
