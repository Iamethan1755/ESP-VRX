#!/usr/bin/env python3
"""Host verification and cycle-accurate simulation for bs_addctia_probe.

Simulates the BitScrambler 2-bundle unrolled pipeline:
- CONTROLLER: ADDCTIAL adds C (preserved in O24..O31) to Counter A
- WORKER: LDCTIAL loads -P = (256 - P) & 255 from in[0..7],
          preserves C (in[8..15]) into O24..O31,
          emits Counter A delta (A0..A4) to [delta, delta],
          reads next 16-bit input word, and writes 16-bit output.
- Validates that all 127 verified pairs match (C - P) mod 32 and
  compact 32-entry Golden DAC output with zero mismatches.
"""

import sys

DAC32 = [
    20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50,
    0, 0, 0, 0, 0, 0, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18
]


def golden_dac(delta):
    signed = ((delta + 16) & 31) - 16
    return max(0, min(63, int(round(20 + 2 * signed))))


def simulate_addctia_loopback():
    # 128 input pairs
    num_pairs = 128
    s_input = bytearray(num_pairs * 2)
    p_vals = []
    c_vals = []

    for pair in range(num_pairs):
        # Full 5-bit phase range 0..31
        p = (pair * 7 + 3) & 31
        c = (pair * 13 + 19) & 31
        p_vals.append(p)
        c_vals.append(c)
        s_input[2 * pair] = (256 - p) & 255
        s_input[2 * pair + 1] = c & 255

    # Cycle-accurate simulation of the 8-slot BitScrambler circular ring
    counter_a = 0
    o_reg = 0
    read_ptr = 0
    s_output = bytearray()

    for pair_step in range(num_pairs):
        slot = (pair_step * 2) % 8

        # --- CONTROLLER ---
        # set 16..23 o24..o31
        controller_in_byte = (o_reg >> 24) & 0xff
        # addctial
        counter_a = (counter_a + controller_in_byte) & 0xff
        o_reg = (controller_in_byte << 16)

        # --- WORKER ---
        delta = counter_a & 0x1f
        # set 0..4 a0..a4, set 8..12 a0..a4
        out_w16 = delta | (delta << 8)
        s_output.append(out_w16 & 0xff)
        s_output.append((out_w16 >> 8) & 0xff)

        # set 16..23 0..7 (new -P)
        # set 24..31 8..15 (new C)
        in_p = s_input[read_ptr]
        in_c = s_input[read_ptr + 1]
        read_ptr += 2

        out_high16 = (in_p << 16) | (in_c << 24)
        o_reg = out_w16 | out_high16

        # ldctial: loads low 8 bits of counter A from out[16..23]
        counter_a = in_p

    # Verification against expected values
    mismatches = 0
    dac_mismatches = 0

    for pair in range(1, num_pairs):
        src_pair = pair - 1
        p = p_vals[src_pair]
        c = c_vals[src_pair]
        expected_delta = (c - p + 32) % 32
        expected_dac = golden_dac(expected_delta)

        hw_delta_0 = s_output[2 * pair]
        hw_delta_1 = s_output[2 * pair + 1]

        if hw_delta_0 != expected_delta or hw_delta_1 != expected_delta:
            mismatches += 1

        dac_from_hw = DAC32[hw_delta_0]
        if dac_from_hw != expected_dac:
            dac_mismatches += 1

    print(f"ADDCTIA Loopback Simulation: {num_pairs} pairs ({len(s_output)} bytes)")
    print(f"Verified pairs: {num_pairs - 1} (pairs 1..{num_pairs - 1})")
    print(f"Delta mismatches: {mismatches}")
    print(f"DAC mismatches:   {dac_mismatches}")

    assert mismatches == 0, f"Delta mismatches: {mismatches}"
    assert dac_mismatches == 0, f"DAC mismatches: {dac_mismatches}"
    print("ALL CHECKS PASSED: 100% bit-exact match to hardware subtraction and 32-entry DAC!")
    return True


if __name__ == "__main__":
    if simulate_addctia_loopback():
        sys.exit(0)
    else:
        sys.exit(1)
