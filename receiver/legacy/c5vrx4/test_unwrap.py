#!/usr/bin/env python3
"""Issue #144: test generated routing, DAC bounds and noisy Q4 trajectories."""
import math
from pathlib import Path
import random
import re
import struct
import generate_phase8 as gen


def expand(spec):
    if '..' not in spec:
        return [spec]
    lo, hi = spec.split('..')
    prefix = ''.join(c for c in lo if c.isalpha())
    a, b = int(lo[len(prefix):]), int(hi[len(prefix):])
    return [prefix + str(i) for i in range(a, b + 1)]


def program(history):
    text = gen.build(history)
    instructions = []
    for name in ('accumulate', 'map_delta', 'decode_next'):
        body = text.split(name + ':', 1)[1].split('\n\n', 1)[0]
        routes = []
        for dst, src in re.findall(r'^\s*set (\S+) (\S+?)[,\s]*$', body, re.M):
            routes.extend(zip(map(int, expand(dst)), expand(src)))
        instructions.append((routes, 2 if name == 'accumulate' else
                             1 if name == 'map_delta' else 0,
                             1 if name == 'accumulate' else
                             2 if name == 'map_delta' else 0))
    return instructions


def source(bit, samples, pos, last, lut, counter):
    if bit == 'L': return 0
    if bit == 'H': return 1
    if bit.startswith('O'): return (last >> int(bit[1:])) & 1
    if bit.startswith('L'): return (lut >> int(bit[1:])) & 1
    if bit.startswith('A'): return (counter >> int(bit[1:])) & 1
    n = int(bit)
    return (samples[(pos + n // 8) % len(samples)] >> (n % 8)) & 1


def signs_index(raws):
    return sum((((raw >> 7) & 1) | (((raw >> 3) & 1) << 1)) << (2*k)
               for k, raw in enumerate(raws))


def tests():
    rng = random.Random(144)
    noisy_total = noisy_valid = ambiguous = 0
    for history in (False, True):
        phases, words = gen.decoder(history), gen.words_for(history)
        assert len(words) == 1024 and max(words) <= 65535
        compiled = Path(__file__).resolve().parent / 'build/esp-idf/main' / (
            'c5vrx4_phase8_history.bsbin' if history else
            'c5vrx4_phase8_static.bsbin')
        if compiled.exists():
            data = compiled.read_bytes()
            header = struct.unpack('<BBBBHBBHBB', data[:12])
            assert header[2:7] == (3, 3, 512, 1, 1)
            assert len(data) == 12 + 3*36 + 2048
            assert list(struct.unpack('<1024H', data[120:])) == words
        assert words[:256] == words[512:768]
        for bank in range(2):
            for raw, p in enumerate(phases[bank]):
                assert (p >> 6) == gen.quadrant(((raw >> 7) & 1) |
                                                (((raw >> 3) & 1) << 1))
        # Every DAC plane duplicates across parity banks. Its selected physical
        # target is monotone and saturating, including wrapped outer branches.
        voltage = [sum(1/r for bit,r in enumerate((8200,3900,2000,1000,470,240))
                       if code & (1 << bit)) for code in range(64)]
        pairs = sorted((gen.transfer_delta(i), voltage[words[i] & 63])
                       for i in range(192))
        assert all(a[1] <= b[1] for a,b in zip(pairs,pairs[1:]))
        # Fixed volts/MHz deliberately saturates outer frequency bins. Keep
        # at least 25 physical levels in the principal branch, not full-span
        # DAC coverage at the expense of video/sync amplitude.
        assert len(set(words[i] & 63 for i in range(64))) >= 25
        # Near-origin and noisy Q4: compare the exact decoded adjacent reference
        # only within its stated bound; report ambiguous paths separately.
        for radius in (.5, 1, 2, 4, 7):
            for sigma in (0, .25, .8, 2):
                for _ in range(500):
                    angle = rng.random()*2*math.pi
                    step = rng.uniform(-1.4, 1.4)
                    raw = []
                    for k in range(4):
                        i = max(-8, min(7, math.floor(radius*math.cos(angle+k*step)
                                                     + rng.gauss(0,sigma))))
                        q = max(-8, min(7, math.floor(radius*math.sin(angle+k*step)
                                                     + rng.gauss(0,sigma))))
                        raw.append(((i & 15) << 4) | (q & 15))
                    bank = rng.randrange(2)
                    p = [phases[bank][r] for r in raw]
                    cls = (words[signs_index(raw)] >> 6) & 3
                    steps = [gen.wrap(b-a) for a,b in zip(p,p[1:])]
                    noisy_total += 1
                    ambiguous += cls == 3
                    if all(abs(d) < 64 for d in steps):
                        e = (((128+p[-1]) & 254) + ((-p[0]) & 254)) & 255
                        d = gen.transfer_delta((e >> 2) | (cls << 6))
                        # Final transfer discards e's low 2 bits and both
                        # endpoint parity bits. Midpoint endpoint error <=2 bins.
                        assert cls != 3 and abs(sum(steps)-d) <= 2
                        noisy_valid += 1
        # Simulate the actual generated bit-routing, counter-high operations,
        # LUT bank overlap and repeated writes over >2 physical DMA wraps.
        samples = [rng.randrange(256) for _ in range(32768)]
        instructions = program(history)
        pos = last = lut = counter = 0
        previous_phase = None
        pending_dac = None
        for iteration in range(24000):
            raws = [samples[(pos+k) % len(samples)] for k in (1,2,3,4)]
            p_current = (lut & 255) - 128
            p_current &= 255
            if previous_phase is not None:
                cls = gen.trajectory_class(signs_index(raws))
                e = (((128+p_current) & 254) + ((-previous_phase) & 254)) & 255
                expected = words[(e >> 2) | (cls << 6)] & 63
            else:
                expected = None
            written = []
            for stage,(routes,read,write) in enumerate(instructions):
                output = sum(source(bit,samples,pos,last,lut,counter) << dst
                             for dst,bit in routes)
                if stage == 0:
                    counter = (counter & 255) | (
                        (((counter >> 8) + (output >> 24)) & 255) << 8)
                    if previous_phase is not None:
                        assert ((output >> 6) & 1) == (previous_phase & 1)
                        assert ((output >> 7) & 1) == (p_current & 1)
                        adjacent = [previous_phase, gen.phase8(raws[1]),
                                    gen.phase8(raws[2]), p_current]
                        steps = [gen.wrap(b-a) for a,b in
                                 zip(adjacent,adjacent[1:])]
                        if all(abs(d) < 64 for d in steps):
                            exact = (counter >> 8) - 128
                            if cls == 1 and exact < 0: exact += 256
                            if cls == 2 and exact >= 0: exact -= 256
                            exact += (output >> 6) & 1
                            exact += (output >> 7) & 1
                            assert exact == sum(steps)
                elif stage == 1:
                    counter = (counter & 255) | ((output >> 24) << 8)
                written.extend((output >> (8*k)) & 63 for k in range(write))
                last = output
                lut = words[(output >> 16) & 1023]
                pos = (pos + read) % len(samples)
            if pending_dac is not None:
                assert written == [pending_dac]*3, (iteration, written, pending_dac)
            if expected is not None:
                assert (last & 63) == expected
            pending_dac = last & 63
            previous_phase = p_current
        print(f'PASS generated routing history={int(history)} iterations=24000 '
              'input_bytes=72000 output_bytes=72000 dma_wraps=2')
    print(f'PASS noisy_Q4 cases={noisy_total} within_bound={noisy_valid} '
          f'ambiguous={ambiguous} wrong_within_bound=0')


if __name__ == '__main__':
    tests()
