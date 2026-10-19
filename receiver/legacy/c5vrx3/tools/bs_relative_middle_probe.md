# Alternating middle/endpoint relative-mux Phase5 probe

This silicon probe is the next milestone following PR #73 (which proved Counter-A
relative mux loopback) and PR #74 (which proved relative mux operation in the
live 40 MHz PARLIO video path).

PR #74 proved relative worker operation with a fixed offset ($A=8$), always
selecting the endpoint sample. This probe proves that the worker can dynamically
alternate ("om en om") between the middle sample ($A=0$) and the endpoint sample
($A=8$) without adding any extra bundle, and verifies on hardware that both
Phase5 results are available at the exact expected pipeline moment.

## Cadence and pipeline flow

The BitScrambler program (`main/bs_relative_middle_probe.bsasm`) uses four
controller/worker pairs filling all eight C5 instruction slots:

- **Slot 0 (`controller_0`)**: Latches `Phase5(endpoint_3)` into `O26..30`; executes `ldctda 0`.
- **Slot 1 (`worker_0`)**: $A=0$. Selects middle sample (FIFO bits 0..7) via `0+a..7+a` into `out[16..23]`. Emits `Phase5(endpoint_3)` as `[Phase5, Phase5]`. Executes `read 16`, `write 16`, `adda 0`.
- **Slot 2 (`controller_1`)**: `L0..4` is `Phase5(middle_0)`. Latches into `O26..30`; executes `ldctda 8`.
- **Slot 3 (`worker_1`)**: $A=8$. Selects endpoint sample (FIFO bits 8..15) via `0+a..7+a` into `out[16..23]`. Emits `Phase5(middle_0)` as `[Phase5, Phase5]`. Executes `read 16`, `write 16`, `adda -8`.
- **Slot 4 (`controller_2`)**: `L0..4` is `Phase5(endpoint_1)`. Latches into `O26..30`; executes `ldctda 0`.
- **Slot 5 (`worker_2`)**: $A=0$. Selects middle sample (FIFO bits 0..7) of Pair 2 into `out[16..23]`. Emits `Phase5(endpoint_1)`. Executes `read 16`, `write 16`, `adda 0`.
- **Slot 6 (`controller_3`)**: `L0..4` is `Phase5(middle_2)`. Latches into `O26..30`; executes `ldctda 8`.
- **Slot 7 (`worker_3`)**: $A=8$. Selects endpoint sample (FIFO bits 8..15) of Pair 3 into `out[16..23]`. Emits `Phase5(middle_2)`. Executes `read 16`, `write 16`, `adda -8`.

Slot 7 wraps to slot 0. Every 50 ns pair still executes exactly two bundles,
consumes 16 bits of IQ, and writes 16 bits of output.

## Hardware loopback oracle

`main/bs_relative_middle_probe.c` implements a finite loopback test using
`bitscrambler_loopback_run` attached to `SOC_BITSCRAMBLER_ATTACH_I2S0`.
It feeds 128 distinct pairs (256 bytes) and verifies that every output pair
matches the ground-truth Golden Phase5 code:

- Even-source pairs ($2k$): output matches `Phase5(s_input[2 * 2k])` (middle sample).
- Odd-source pairs ($2k+1$): output matches `Phase5(s_input[2 * (2k+1) + 1])` (endpoint sample).

Expected output on silicon:
```text
BS_REL_MIDDLE status=PASS written=256 mismatches=0 err=ESP_OK
```

The result is printed at boot when enabled via `CONFIG_C5VRX_BS_RELATIVE_MIDDLE_PROBE=y`,
and remains queryable through the serial `p` snapshot command.

## Silicon verification

The firmware was built locally via ESP-IDF v6.0.2 Docker container and flashed
to a Seeed Studio XIAO ESP32-C5 on `COM10` on 2026-09-25.

Upon boot, the hardware loopback oracle executed and reported:
```text
BS_REL_MIDDLE status=PASS written=256 mismatches=0 err=ESP_OK
```

The live video pipeline then started using the Static-A Relative Golden demodulator,
confirming `tx_empty=0`, `rx_ovf=0`, `tx_eof=0`, `bs_empty=0`, `bs_eof=0`.
This proves conclusively on real C5 silicon that:
1. Counter-A relative muxing dynamically alternates between middle ($A=0$) and endpoint ($A=8$) raw samples without adding any extra bundle.
2. Both Phase5 results are successfully decoded and available at the expected pipeline cadence.
3. Static-A Relative Golden runs in the live 40 MHz PARLIO TX video path without pipeline drops.

