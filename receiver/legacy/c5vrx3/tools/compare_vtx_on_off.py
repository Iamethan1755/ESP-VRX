#!/usr/bin/env python3
"""Compare MODEM_DIAG lane activity between VTX ON and VTX OFF."""

import re
import sys
from pathlib import Path

TRACE_RE = re.compile(
    r"PHY_TAP config=(\d+) lanes=(IQ_REF/)?(\d+)\.\.(\d+) "
    r"samples=(\d+).*?vary=0x([0-9a-fA-F]+) transitions=([0-9,]+)"
)


def parse_log(path: Path):
    # returns dict[config, dict[(first, is_ref), (vary, [trans])]]
    res = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = TRACE_RE.search(line)
        if not m:
            continue
        config, is_ref, first, last, samples, vary, trans = m.groups()
        config = int(config)
        first = int(first)
        is_ref = bool(is_ref)
        vary = int(vary, 16)
        transitions = [int(x) for x in trans.split(",")]
        res.setdefault(config, {})[(first, is_ref)] = (vary, transitions)
    return res


def main():
    if len(sys.argv) < 3:
        print("Usage: compare_vtx_on_off.py on.log off.log")
        return

    on_data = parse_log(Path(sys.argv[1]))
    off_data = parse_log(Path(sys.argv[2]))

    print("=" * 70)
    print(" C5VRX-3 MODEM_DIAG LANE SENSITIVITY: VTX ON vs VTX OFF")
    print("=" * 70)

    for config in sorted(on_data.keys()):
        if config not in off_data:
            continue
        print(f"\n--- SELECTOR CONFIG {config} ---")
        print(f"{'Lanes':<15} | {'Bit':<4} | {'VTX ON Trans':<14} | {'VTX OFF Trans':<14} | {'Sensitivity (Delta)':<20}")
        print("-" * 70)

        for (first, is_ref) in sorted(on_data[config].keys()):
            if (first, is_ref) not in off_data[config]:
                continue
            name = f"IQ_REF" if is_ref else f"DIAG[{first}..{first+7}]"
            _, on_trans = on_data[config][(first, is_ref)]
            _, off_trans = off_data[config][(first, is_ref)]

            for bit in range(len(on_trans)):
                lane_num = f"bit{bit}" if is_ref else f"lane{first+bit}"
                ont = on_trans[bit]
                offt = off_trans[bit]
                delta = ont - offt
                ratio = (ont / offt) if offt > 0 else float("inf") if ont > 0 else 1.0
                sens = f"+{delta} ({ratio:.1f}x)" if delta >= 0 else f"{delta} ({ratio:.1f}x)"
                print(f"{name:<15} | {lane_num:<4} | {ont:<14} | {offt:<14} | {sens:<20}")


if __name__ == "__main__":
    main()
