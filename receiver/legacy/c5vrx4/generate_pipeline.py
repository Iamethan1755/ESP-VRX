#!/usr/bin/env python3
"""Generate the C5VRX-4 three-bundle program; no model/hardware test is run."""
from pathlib import Path
import math

HERE = Path(__file__).resolve().parent

def phase6(raw):
    i, q = raw >> 4, raw & 15
    i = (i if i < 8 else i - 16) * 64 + 31.5
    q = (q if q < 8 else q - 16) * 64 + 31.5
    return round(math.atan2(q, i) * 32 / math.pi) & 63

def build():
    # GPIO bit order and network remain those of the existing 6-bit DAC.
    # Nominal resistor inversion; this is not measured board calibration.
    resistance = (8200, 3900, 2000, 1000, 470, 240)
    voltage = [sum((1 / r) for bit, r in enumerate(resistance) if code & (1 << bit))
               for code in range(64)]
    words = [0] * 1024
    for raw in range(256):
        phase = phase6(raw)
        words[raw] = ((32 + phase) & 63) | (((-phase) & 63) << 8)
    for biased_delta in range(64):
        target = voltage[63] * biased_delta / 63
        words[256 + biased_delta] = min(range(64), key=lambda code: abs(voltage[code] - target))
    return """# C5VRX-4: Q4/I4 -> Phase6 span75 -> nominal DAC transfer.
# Three bundles / three IQ bytes / three DAC bytes. Continuous [D,D,D].
# 40 MHz transport, 13.333 MS/s unique video. Wrap at +/-6.667 MHz.
# LUT bank 0: phase terms; bank 1: biased-delta DAC mapping.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 0
cfg lut_width_bits 16
lut """ + " ".join(map(str, words)) + """

decode:
    # L is the preceding DAC result. Preserve it twice in output history.
    set 0..5 L0..L5,
    set 8..13 L0..L5,
    # Address the third input byte; load -previous above the LUT address.
    set 16..23 16..23,
    set 24..25 L,
    set 26..31 O26..O31,
    read 16,
    write 8,
    ldctia

delta:
    set 0..5 O0..O5,
    set 8..13 O8..O13,
    # Retain -current outside the two DAC bytes. Low counter sum <=318.
    set 16..21 L8..L13,
    set 22..25 L,
    set 26..31 L0..L5,
    read 8,
    write 16,
    addctia

map_dac:
    set 0..5 O0..O5,
    set 8..13 O8..O13,
    # LUT uses only bits 16..25: retain -current above its address.
    set 16..21 A10..A15,
    set 22..23 L,
    set 24 H,
    set 25 L,
    set 26..31 O16..O21,
    jmp decode
"""

if __name__ == "__main__":
    (HERE / 'c5vrx4_span75.bsasm').write_text(build(), encoding='utf-8')
