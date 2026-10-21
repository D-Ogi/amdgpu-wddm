"""Find the innermost loops (backward conditional branches) of an llvm-objdump -d -M intel listing and print them.

Usage: python loops.py <listing> [--max N] [--vec] [--ctx K]
--vec keeps only loops that contain an xmm/ymm operand; --ctx K prints K instructions before and after the loop
(to show the timer calls around it). Prints per loop: start, end, instruction count, vector ops, memory operands.
"""
import re
import sys

args = sys.argv[1:]
path = args[0]
maxn = int(args[args.index('--max') + 1]) if '--max' in args else 80
ctx = int(args[args.index('--ctx') + 1]) if '--ctx' in args else 0
vec_only = '--vec' in args

line_re = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
insns = []
for raw in open(path, encoding='utf-8', errors='replace'):
    m = line_re.match(raw.rstrip('\n'))
    if m:
        insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
index = {a: i for i, (a, _, _) in enumerate(insns)}

for i, (addr, mn, ops) in enumerate(insns):
    if not mn.startswith('j') or mn == 'jmp':
        continue
    t = re.match(r'0x([0-9a-f]+)', ops)
    if not t:
        continue
    tgt = int(t.group(1), 16)
    if tgt >= addr or tgt not in index:
        continue
    s = index[tgt]
    body = insns[s:i + 1]
    if len(body) > maxn:
        continue
    vec = [b for b in body if re.search(r'\b[xy]mm\d+', b[2])]
    if vec_only and not vec:
        continue
    mem = [b for b in body if '[' in b[2] and not b[1].startswith('nop') and not b[1].startswith('lea')]
    print(f'=== loop {tgt:#x}-{addr:#x}: {len(body)} insns, {len(vec)} with xmm/ymm, {len(mem)} with a memory operand')
    for b in insns[max(0, s - ctx):s]:
        print(f'   . {b[0]:x}: {b[1]} {b[2]}')
    for b in body:
        print(f'     {b[0]:x}: {b[1]} {b[2]}')
    for b in insns[i + 1:i + 1 + ctx]:
        print(f'   . {b[0]:x}: {b[1]} {b[2]}')
