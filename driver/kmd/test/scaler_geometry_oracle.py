#!/usr/bin/env python3
"""Compare the C geometry shim with exact rational arithmetic and boundary cases."""
import argparse
from fractions import Fraction as F
import itertools
import random
import subprocess

Q = 1 << 19


def floor(value):
    return value.numerator // value.denominator


def truncate19(value):
    return F(floor(value * Q), Q)


def round32(value):
    return F(floor(value * (1 << 32) + F(1, 2)), 1 << 32)


def oracle(case):
    surface, active, sb, se, db, de, cb, ce, taps, mirror = case
    zero = (0,) * 6
    src, dst = se - sb, de - db
    if (not 1 <= surface <= 16383 or not 1 <= active <= 16383 or sb < 0
            or src <= 0 or se > surface or dst <= 0 or ce < cb
            or not 1 <= taps <= 8 or mirror not in (0, 1)):
        return (2, *zero)
    if dst > 16383 or src > 8 * dst:
        return (3, *zero)
    first, stop = max(db, cb, 0), min(de, ce, active)
    if first >= stop:
        return (1, *zero)
    # AMD rounds to 31.32 first, then truncates to 19 fractional bits.
    rounded32 = round32(F(src, dst))
    ratio = truncate19(rounded32)
    if ratio == 8:
        ratio -= F(1, 1 << 32)
    position = ratio * (first - db)
    tap_origin = floor(position)
    # div_int uses from_fraction too: the exact-8 adjustment leaves an odd
    # 31.32 value, whose half rounds up before the 19-bit truncation.
    phase = truncate19(round32((ratio + taps + 1) / 2) + position - tap_origin)
    extension = min(tap_origin, max(0, taps - floor(phase)))
    origin = tap_origin - extension
    phase += extension
    end = min(src, origin + floor(phase + ratio * (stop - first - 1)))
    width = end - origin
    if width <= 0 or origin < 0 or end > src or ratio <= 0 or floor(phase) > 15:
        return (3, *zero)
    if mirror:
        origin = src - end
    return (0, sb + origin, width, first, stop - first,
            floor(ratio * Q), floor(phase * Q))


def cases():
    # Full Cartesian boundary sample: 8:1 edge, negative placement, source crop,
    # mirror, narrow clip and one-pixel output. No runtime-generated seed.
    for src, dst, taps, mirror in itertools.product(
            (1, 2, 7, 8, 9, 127, 1024, 8191, 8192, 8193, 16383),
            (1, 2, 16, 1024, 1920, 16383), (1, 2, 4, 8), (0, 1)):
        for shift in (-dst, -dst // 2, 0, 1):
            yield (src, 16383, 0, src, shift, shift + dst, 0, 16383, taps, mirror)
            yield (src, 16383, 0, src, shift, shift + dst, 1, 2, taps, mirror)
    rng = random.Random(223)
    for _ in range(20000):
        surface = rng.randint(1, 16383)
        sb = rng.randrange(surface)
        se = rng.randint(sb + 1, surface)
        width = rng.randint(1, 16383)
        start = rng.randint(-width, 16383)
        cb = rng.randint(-16383, 16383)
        yield (surface, rng.randint(1, 16383), sb, se, start, start + width,
               cb, cb + rng.randint(0, 32766), rng.randint(1, 8), rng.randrange(2))
    base = [1920, 1200, 0, 1080, 0, 1200, 0, 1200, 4, 0]
    for index in range(10):
        # Signed and unsigned boundaries, including endpoint subtraction wider
        # than int32. Each input stays representable by its public field type.
        values = (0, 1, 16383, 16384, (1 << 31) - 1)
        values += ((1 << 32) - 1,) if index in (0, 1, 8, 9) else (-(1 << 31), -1)
        for value in values:
            item = base.copy()
            item[index] = value
            yield tuple(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    args = parser.parse_args()
    inputs = list(cases())
    result = subprocess.run([args.exe, '--stream'], input=''.join(
        ' '.join(map(str, case)) + '\n' for case in inputs),
        text=True, capture_output=True, check=True, timeout=60)
    outputs = result.stdout.splitlines()
    if len(outputs) != len(inputs):
        raise AssertionError(f'Expected {len(inputs)} results, got {len(outputs)}')
    counts = [0] * 4
    for case, line in zip(inputs, outputs):
        expected = oracle(case)
        actual = tuple(map(int, line.split()))
        if actual != expected:
            raise AssertionError(f'{case}: got {actual}, expected {expected}')
        counts[actual[0]] += 1
    if not all(counts):
        raise AssertionError(f'Missing status coverage: {counts}')
    print(f'Rational oracle: {len(inputs)} cases passed; OK/EMPTY/INVALID/UNSUPPORTED={counts}')


if __name__ == '__main__':
    main()
