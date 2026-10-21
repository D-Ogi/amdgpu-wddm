"""summarize.py - condenses a q44bench output (lab or dev) into the tables the answers use.

    python summarize.py <q44bench output.txt> [--ghz G]

Prints: the clock lines, the chain sweep per op/width (cycles per instruction and instructions per cycle from the
add-chain clock), the q24/q25/q27 groups, the memory-chain group, the frame lines of both passes, and the SMT lines.
"""
import re
import sys

path = sys.argv[1]
ghz = None
if '--ghz' in sys.argv:
    ghz = float(sys.argv[sys.argv.index('--ghz') + 1])
text = open(path, encoding='utf-8', errors='replace').read().split('\n')

clocks = []
for l in text:
    m = re.match(r'clock (\S+)\s+dependent add chain ns/add min ([\d.]+) med ([\d.]+) max ([\d.]+) -> ([\d.]+) GHz', l)
    if m:
        clocks.append((m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))))
for c in clocks:
    print(f'clock {c[0]:8s} ns/add min {c[1]:.5f} med {c[2]:.5f} max {c[3]:.5f} -> {c[4]:.3f} GHz')
if ghz is None and clocks:
    ghz = sum(1.0 / c[1] for c in clocks) / len(clocks)
if ghz is None:
    ghz = 1.0
    print('no clock lines and no --ghz: cycle columns below are ns')
else:
    print(f'GHz used for cycles: {ghz:.4f} (mean of 1/min ns per dependent add, or --ghz)')

reg = {}
for l in text:
    m = re.match(r'reg (\S+)\s+w(\d+)\s+insns/iter\s+(\d+) iters\s+(\d+) \| ns/insn min ([\d.]+) med ([\d.]+) max ([\d.]+)', l)
    if m:
        reg[m.group(1)] = dict(w=int(m.group(2)), insns=int(m.group(3)), mn=float(m.group(5)), md=float(m.group(6)),
                              mx=float(m.group(7)), line=l)


def cyc(ns):
    return ns * ghz


print('\n== chain sweep: cycles per instruction (min, med) and instructions per cycle (from min)')
for op in ('fma', 'fadd', 'fmul'):
    for w in (128, 256):
        row = []
        for k in (1, 2, 4, 5, 6, 8, 10, 12):
            r = reg.get(f'{op}{w}_c{k}')
            if r:
                row.append(f'k{k}:{cyc(r["mn"]):.3f}/{cyc(r["md"]):.3f}({1 / cyc(r["mn"]):.2f})')
        print(f'{op}{w:<4d} ' + ' '.join(row))
    r1 = reg.get(f'{op}256_c1')
    if r1:
        print(f'   {op} latency (k1, 256): {cyc(r1["mn"]):.3f} cycles; 128: {cyc(reg[op + "128_c1"]["mn"]):.3f}')

print('\n== other register kernels: name, cyc/insn min, med, max-spread %, insn/cyc, elements(FP32/int32) per ns')
for name, r in reg.items():
    if re.match(r'(fma|fadd|fmul)\d+_c\d+$', name):
        continue
    lanes = r['w'] // 32 if r['w'] in (128, 256) else 1
    spread = (r['mx'] - r['mn']) / r['mn'] * 100
    print(f'{name:28s} cyc {cyc(r["mn"]):.3f} med {cyc(r["md"]):.3f} spread {spread:5.1f}% ipc {1 / cyc(r["mn"]):.3f}'
          f' elem/ns {lanes / r["mn"]:.2f}')

print('\n== elements per ns, 128 vs 256 at 12 chains')
for op in ('fma', 'fadd', 'fmul', 'pshufb', 'vpaddd', 'vpmulld'):
    a, b = reg.get(f'{op}128_c12'), reg.get(f'{op}256_c12')
    if a and b:
        print(f'{op:8s} 128: {4 / a["mn"]:.2f}/ns  256: {8 / b["mn"]:.2f}/ns  ratio {(8 / b["mn"]) / (4 / a["mn"]):.2f}')

for l in text:
    if l.startswith(('q24', 'q27', 'csweep', 'smt', 'frame', 'tsc', 'buffers', 'topology', 'Q44', 'q44')):
        print(l)
