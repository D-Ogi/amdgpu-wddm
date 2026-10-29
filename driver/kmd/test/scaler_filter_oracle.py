"""Check coefficient selection against AMD source and linear image interpolation."""
import argparse
from fractions import Fraction
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    text = (root / 'driver/amdgpu-import/reference/dce_scl_filters.c').read_text()
    tables = {name: [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]+', body)]
              for name, body in re.findall(
                  r'static const uint16_t (filter_\w+)\[\d+\] = \{(.*?)\};', text, re.S)}
    ratios = {0, 1, 524287, 524288, 524289, 4194303, 4194304, 0xffffffff}
    for threshold in (Fraction(4, 3), Fraction(5, 3)):
        below = int(threshold * 524288)
        ratios.update((below - 1, below, below + 1, below + 2))
    cases = [(kind, taps, ratio) for kind in (0, 1, 2, 0xffffffff)
             for taps in range(10) for ratio in sorted(ratios)]
    run = subprocess.run([args.exe, '--stream'], text=True, capture_output=True,
                         input=''.join(f'{k} {t} {r}\n' for k, t, r in cases), check=True)
    lines = run.stdout.splitlines()
    assert len(lines) == len(cases), (len(lines), len(cases))
    image_checks = 0
    selected = set()
    for (kind, taps, ratio), line in zip(cases, lines):
        actual = list(map(int, line.split()))
        valid = (kind <= 1 and 1 <= taps <= 8 and 0 < ratio < 4194304 and
                 (taps != 1 or ratio == 524288) and
                 (kind != 1 or taps in (1, 2)))
        expected = [0] * (4 + 264)
        if valid:
            expected[0:2] = [1, taps]
            if taps > 1:
                stride = (taps + 1) // 2 * 2
                expected[2:4] = [stride // 2, 33 * stride]
                if kind == 0:
                    r = Fraction(ratio, 524288)
                    suffix = ('upscale' if r < 1 else '116' if r < Fraction(4, 3)
                              else '149' if r < Fraction(5, 3) else '183')
                    name = 'filter_2tap_64p' if taps == 2 else f'filter_{taps}tap_64p_{suffix}'
                    selected.add(name)
                    source = tables[name]
                    assert len(source) == 33 * taps
                for phase in range(33):
                    coeff = ([4096 - phase * 64, phase * 64] if kind == 1
                             else source[phase * taps:(phase + 1) * taps])
                    expected[4 + phase * stride:4 + phase * stride + taps] = coeff
                if kind == 1:
                    # Independent image oracle: separable coefficients must
                    # reproduce bilinear interpolation, including ramps/edges.
                    for x in range(33):
                        for y in (0, 1, 16, 31, 32):
                            wx = [Fraction(v, 4096) for v in actual[4 + 2*x:6 + 2*x]]
                            wy = [Fraction(v, 4096) for v in actual[4 + 2*y:6 + 2*y]]
                            for a, b, c, d in ((0, 255, 255, 0), (0, 64, 128, 192), (255,)*4):
                                got = wy[0]*(wx[0]*a + wx[1]*b) + wy[1]*(wx[0]*c + wx[1]*d)
                                tx, ty = Fraction(x, 64), Fraction(y, 64)
                                want = a*(1-tx)*(1-ty) + b*tx*(1-ty) + c*(1-tx)*ty + d*tx*ty
                                assert got == want, (x, y, got, want)
                                image_checks += 1
        assert actual == expected, ((kind, taps, ratio), actual[:10], expected[:10])
    assert len(selected) == 25, selected
    # Positive counterexample: ModifiedLanczos is not the linear kernel.
    assert tables['filter_2tap_64p'][32:34] == [0x0e68, 0x0198]
    assert tables['filter_2tap_64p'][32:34] != [3072, 1024]
    print(f'Scaler filters: {len(cases)} plans, all 25 AMD tables, {image_checks} exact bilinear image checks PASS')


if __name__ == '__main__':
    main()
