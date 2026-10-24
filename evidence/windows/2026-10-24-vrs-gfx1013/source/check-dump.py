# Checks the FragCoord column of a vrsprobe --dump file (W x H texels of uvec4: id, fragcoord, rate, marker).
# vrsprobe's own RESULT line checks the ids; this checks that every pixel holds the FragCoord that its shading
# rate gives: the pixel centre at 1x1, the centre of the coarse fragment at 2x2, 2x1 or 1x2.
# Usage: python check-dump.py FILE [--size N]
# Output: one line per FILE: counts of pixels whose FragCoord is the 1x1 centre, the 2x2, 2x1 or 1x2 coarse
# centre, or none of them; bad markers; the first three pixels that match nothing.
import struct
import sys


def centre(p, n):
    """FragCoord * 16 of pixel p for a coarse fragment n pixels wide, as vrsprobe.frag packs it."""
    return (p // n * n) * 16 + n * 8


def main(argv):
    path = argv[0]
    size = int(argv[argv.index('--size') + 1]) if '--size' in argv else 256
    data = open(path, 'rb').read()
    if len(data) != size * size * 16:
        sys.exit('%s: %d bytes, expected %d for --size %d' % (path, len(data), size * size * 16, size))
    shapes = {'1x1': (1, 1), '2x2': (2, 2), '2x1': (2, 1), '1x2': (1, 2)}
    counts = dict.fromkeys(shapes, 0)
    other, marker, first = 0, 0, []
    for y in range(size):
        for x in range(size):
            _, fc, _, mk = struct.unpack_from('<4I', data, (y * size + x) * 16)
            if mk != 0xA5A5A5A5:
                marker += 1
            fx, fy = fc & 0xFFFF, fc >> 16
            hit = False
            for name, (w, h) in shapes.items():
                if fx == centre(x, w) and fy == centre(y, h):
                    counts[name] += 1
                    hit = True
            if not hit:
                other += 1
                if len(first) < 3:
                    first.append('(%d,%d)=(%.4f,%.4f)' % (x, y, fx / 16.0, fy / 16.0))
    # The four centres of one pixel are all different, so each pixel adds to one count at most.
    print('FRAGCOORD file=%s pixels=%d centre1x1=%d centre2x2=%d centre2x1=%d centre1x2=%d none=%d bad_marker=%d '
          'first_none=%s' % (path, size * size, counts['1x1'], counts['2x2'], counts['2x1'], counts['1x2'], other,
                             marker, ','.join(first) or '-'))


if __name__ == '__main__':
    main(sys.argv[1:])
