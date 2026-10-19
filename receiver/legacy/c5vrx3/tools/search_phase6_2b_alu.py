#!/usr/bin/env python3
"""Exhaustive search for Phase6-2B ALU/LUT decomposition.

Investigates whether:
1. Two raw LUT lookups (one for middle IQ, one for current IQ)
   each producing a 16-bit word,
2. Combined with Counter A and Counter B operations across 2 bundles,
3. Can compute exact Adjacent50 DAC output.
"""

from __future__ import annotations

def wrap32(d: int) -> int:
    return ((d + 16) & 31) - 16

def target_dac(p: int, m: int, c: int) -> int:
    s = wrap32(m - p) + wrap32(c - m)
    return max(0, min(63, 20 + 6 * s))

def check_counter_algebra():
    """Explore what Counter A/B can compute when fed LUT outputs."""
    print("Testing Counter ALU capability:")
    # Suppose Counter A holds a representation of p.
    # Bundle A feeds middle raw to LUT -> L has 16 bits depending on m.
    # What can Counter A compute?
    # Counter A can add an operand formed by selecting bits from L and O.
    
    # Can Counter A compute wrap32(m - p)?
    # Notice: m - p mod 32 is (m - p) & 31.
    # wrap32(m - p) = ((m - p + 16) & 31) - 16.
    
    # If Counter A computes m - p mod 32:
    # Let's test if (m - p) & 31 gives the wrapped delta directly.
    # For small deltas:
    # m - p = 0 -> 0
    # m - p = 1 -> 1
    # m - p = -1 -> 31
    # m - p = -2 -> 30
    
    # Now in bundle B:
    # Counter A has (m - p).
    # Can Counter A add (c - m)?
    # Notice: (m - p) + (c - m) = c - p !
    # THE MIDDLE SAMPLE M CANCELS OUT COMPLETELY IN ORDINARY ADDITION!
    print("Crucial mathematical identity:")
    print("In linear modular addition: (m - p) + (c - m) = c - p (mod 2^N).")
    print("m completely vanishes unless winding / wrap carry is preserved!")
    
    # Why does m matter?
    # Because wrap32 is NON-LINEAR:
    # wrap32(m - p) + wrap32(c - m) != wrap32(c - p) when winding occurs!
    # The difference is:
    # (wrap32(m - p) + wrap32(c - m)) - wrap32(c - p) = 32 * k, where k in {-1, 0, +1}!
    print("The non-linear winding difference is exactly: k = (pair_sum - wrap32(c - p)) // 32 in {-1, 0, 1}.")

if __name__ == "__main__":
    check_counter_algebra()
