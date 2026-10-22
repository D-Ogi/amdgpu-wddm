#!/usr/bin/env python3
"""Exact-rational CPU analysis; normalization is a hypothesis, not a GPU witness.

Limited-range inverse: Microsoft Recommended 8-Bit YUV Formats for Video
Rendering, Kr/Kb and RGB-to-YUV equations (inverted algebraically here).
https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering
Full-range model: Y/255 and chroma (code-128)/255, explicitly a comparison
convention. AMD zero-expansion model: Cr,Y,Cb divided by 256, S2.13 matrix,
then output clamped to [0,1] and multiplied by 255. The real CNVC/CM pipeline
normalization, clamp and rounding must be measured separately.
"""
import argparse
from fractions import Fraction as F
from itertools import product
import json
import subprocess


def reference(space, y, u, v):
    kr, kb = (F(299,1000), F(114,1000)) if space <= 2 else (F(2126,10000), F(722,10000))
    limited = space in (2, 4)
    luma = F(y-16,219) if limited else F(y,255)
    cb = F(u-128,224 if limited else 255)
    cr = F(v-128,224 if limited else 255)
    r = luma + 2*(1-kr)*cr
    b = luma + 2*(1-kb)*cb
    g = (luma-kr*r-kb*b)/(1-kr-kb)
    return [max(F(0),min(F(1),x))*255 for x in (r,g,b)]


def evaluate(matrix, y, u, v):
    inputs = (F(v,256),F(y,256),F(u,256),F(1))
    return [max(F(0),min(F(1),sum(matrix[4*r+c]*inputs[c] for c in range(4))))*255 for r in range(3)]


def analyze(space, matrix):
    # Analytic channel-sign checks and a complete neutral ramp catch signed
    # decoding, row ordering and range-selection errors independently of bytes.
    assert matrix[0] > 0 and matrix[1] > 0 and matrix[2] == 0
    assert matrix[4] < 0 and matrix[5] > 0 and matrix[6] < 0
    assert matrix[8] == 0 and matrix[9] > 0 and matrix[10] > 0
    last = [F(-1)]*3
    for y in range(256):
        cur = evaluate(matrix,y,128,128)
        assert all(a >= b for a,b in zip(cur,last))
        last = cur
    # Corners, clipping boundaries, neutral and near-neutral values; this is
    # a finite sample comparison, NOT a maximum over the entire color cube.
    levels = (0,16,64,127,128,129,192,235,240,255)
    worst = (F(-1),None)
    for y,u,v in product(levels, repeat=3):
        actual, expected = evaluate(matrix,y,u,v), reference(space,y,u,v)
        for channel in range(3):
            error = abs(actual[channel]-expected[channel])
            if error > worst[0]:
                worst = (error, {'Y':y,'Cb':u,'Cr':v,'channel':'RGB'[channel],
                                'computed':float(actual[channel]),'reference':float(expected[channel])})
    return {'space':space,'samples':1000,'neutral_ramp_monotonic':True,
            'max_sample_error_codes':float(worst[0]),
            'max_sample_error_exact':str(worst[0]),'counterexample':worst[1],
            'black_neutral_codes':[float(x) for x in evaluate(matrix,16 if space in (2,4) else 0,128,128)],
            'white_neutral_codes':[float(x) for x in evaluate(matrix,235 if space in (2,4) else 255,128,128)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True)
    parser.add_argument('--summary',action='store_true')
    args = parser.parse_args()
    lines = subprocess.check_output([args.exe,'--matrices'],text=True).splitlines()
    assert len(lines) == 4
    reports, matrices = [], []
    for space,line in enumerate(lines,1):
        values = list(map(int,line.split()))
        assert len(values) == 13 and values[0] == space
        assert all(0 <= v <= 65535 for v in values[1:])
        matrix = [F(v if v < 32768 else v-65536,8192) for v in values[1:]]
        matrices.append(matrix)
        reports.append(analyze(space,matrix))
    # Different colorimetry/range choices must not silently use the same table.
    assert len(set(tuple(m) for m in matrices)) == 4
    if args.summary:
        for report in reports:
            print(f"CSC space {report['space']}: 1000 samples, monotonic neutral ramp; "
                  f"conditional max sample error {report['max_sample_error_codes']:.9f} codes")
        print('CSC normalization is unproven on hardware; this is not an HLK color pass')
        return
    print(json.dumps({'kind':'conditional CPU arithmetic analysis',
          'hardware_normalization_proven':False,'hlk_color_tolerance_established':False,
          'normalization':'input code/256; output normalized*255; clamp [0,255]',
          'results':reports},indent=2))


if __name__ == '__main__':
    main()
