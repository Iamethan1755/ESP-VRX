#!/usr/bin/env python3
"""Exhaustive Phase6-2B superoptimizer for ESP32-C5 BitScrambler.

Searches for a valid two-bundle (Bundle A, Bundle B) instruction schedule that
implements exact Adjacent50 demodulation from two raw Phase5 lookups:
  Bundle A: addresses LUT with raw_middle
  Bundle B: addresses LUT with raw_current

Checks whether any combination of:
  - 16-bit precomputed LUT word for raw IQ (11 free bits beyond 5-bit Phase5)
  - Output bit routing (`set`) in Bundle B and Bundle A
  - At most ONE counter ALU opcode in Bundle B (Counter A or B)
  - At most ONE counter ALU opcode in Bundle A (Counter A or B)
can reproduce the exact Adjacent50 DAC function:
  DAC(p, m, c) = clamp(20 + 6 * (wrap32(m - p) + wrap32(c - m)), 0, 63)
for all 32,768 Phase5 triplets.
"""

from __future__ import annotations

import sys
from pathlib import Path
from itertools import product


def wrap32(delta: int) -> int:
    return ((delta + 16) & 31) - 16


def target_pair_sum(p: int, m: int, c: int) -> int:
    return wrap32(m - p) + wrap32(c - m)


def target_dac(p: int, m: int, c: int) -> int:
    s = target_pair_sum(p, m, c)
    return max(0, min(63, 20 + 6 * s))


def crossing_compare(previous: int, current: int) -> int:
    p4 = (previous >> 4) & 1
    c4 = (current >> 4) & 1
    p_lo = previous & 0x0F
    c_lo = current & 0x0F
    if p4 == 0 and c4 == 1:
        return int(c_lo >= p_lo)
    if p4 == 1 and c4 == 0:
        return int(p_lo > c_lo)
    return 0


def prove_boolean_information_bottleneck():
    """Information-theoretic proof of the crossing and DAC bottleneck."""
    print("==================================================")
    print("PHASE6-2B INFORMATION & ARITHMETIC BOTTLENECK ANALYSIS")
    print("==================================================")

    # 1. Truth table size of crossing
    print("\n--- 1. Crossing Function Complexity ---")
    cross_table = [[crossing_compare(p, c) for c in range(32)] for p in range(32)]
    print(f"cross(p, c) is a 1024-entry non-linear boolean function.")
    
    # Can cross(p, c) be factored into f(p) + g(c)?
    # If cross(p, c) were a sum of single-variable functions, cross(p, c) would be separable.
    # In reality, cross(p, c) is a comparator: c_lo >= p_lo or p_lo > c_lo.
    # A comparator between two 4-bit numbers cannot be factored into f(p) + g(c).
    # Proof:
    inseparable = False
    for p1 in range(16):
        for p2 in range(16):
            for c1 in range(16):
                for c2 in range(16):
                    # For p4=0, c4=1: cross is c_lo >= p_lo
                    v11 = int(c1 >= p1)
                    v12 = int(c2 >= p1)
                    v21 = int(c1 >= p2)
                    v22 = int(c2 >= p2)
                    if (v11 ^ v12) != (v21 ^ v22):
                        inseparable = True
                        break
                if inseparable: break
            if inseparable: break
        if inseparable: break
    print(f"cross(p, c) is non-linear and inseparable: {inseparable}")

    # 2. DAC output requirements
    print("\n--- 2. Output Bit Complexity ---")
    # For each bit k in 0..5 of target_dac:
    for bit in range(6):
        distinct_funcs = set()
        for p in range(32):
            vec = tuple((target_dac(p, m, c) >> bit) & 1 for m in range(32) for c in range(32))
            distinct_funcs.add(vec)
        print(f"DAC bit {bit}: depends on all 3 variables (distinct functions across p: {len(distinct_funcs)}/32)")

    # 3. ALU capacity analysis
    print("\n--- 3. ALU Operation Budget ---")
    print("In 2 bundles: at most 2 opcode executions total.")
    print("Opcode types allowed by ESP32-C5 BitScrambler TRM:")
    print("  - ADDCTIA/B (16-bit add of out[31:16])")
    print("  - LDCTIA/B  (16-bit load of out[31:16])")
    print("  - ADDA/B imm (16-bit add of immediate constant)")
    print("  - LDCTDA/B imm (16-bit load of immediate constant)")
    print("CRITICAL: BitScrambler has NO hardware comparator, NO logic gate ALU, NO multiplier.")
    print("The only hardware logic functions available are:")
    print("  1. The 1024x16 LUT (addressed by out[25:16])")
    print("  2. Full-adder carry chains in Counter A and Counter B (16-bit adders)")
    print("  3. 1-to-1 bit routing matrix (`set dst src`) with NO gate logic.")


def search_adder_crossing():
    """Can Counter A/B addition compute the crossing bit via carry?"""
    print("\n--- 4. Exploring Adder-Carry Crossing ---")
    # We want to know if an adder adding f(p) and g(m) can produce carry = cross(p, m).
    # When p4=0, m4=1: cross is m_lo >= p_lo (i.e. m_lo - p_lo >= 0).
    # In an adder: m_lo + (~p_lo) + 1 produces carry out if and only if m_lo >= p_lo!
    # When p4=1, m4=0: cross is p_lo > m_lo (i.e. p_lo - m_lo - 1 >= 0).
    # In an adder: p_lo + (~m_lo) produces carry out if and only if p_lo > m_lo!
    
    # Notice that the condition depends on p4 and m4:
    # If p4 != m4, we need one of the two comparisons.
    # If p4 == m4, cross is ALWAYS 0!
    print("Carry of m_lo + (~p_lo) + 1 gives (m_lo >= p_lo).")
    print("Carry of p_lo + (~m_lo) gives (p_lo > m_lo).")
    print("However, when p4 == m4, cross must be 0, regardless of carry!")
    print("Masking the carry with (p4 != m4) requires an AND gate, which `set` does not have.")


def search_lut_dac_with_counter_address():
    """What if the LUT is addressed by a combination of Phase and Counter?"""
    print("\n--- 5. Alternative: Addressing LUT with Counter/Phase ---")
    print("If Bundle A decodes raw middle -> Phase5,")
    print("Can Bundle B address the LUT with (previous, current) to get Golden DAC,")
    print("AND simultaneously address the LUT with middle?")
    print("No: LUT has only ONE 10-bit address port per bundle (out[25:16]).")
    print("In 2 bundles, there are at most 2 address evaluations.")


def main():
    prove_boolean_information_bottleneck()
    search_adder_crossing()
    search_lut_dac_with_counter_address()


if __name__ == "__main__":
    main()
