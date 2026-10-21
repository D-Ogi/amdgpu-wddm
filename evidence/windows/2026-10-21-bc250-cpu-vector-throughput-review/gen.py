"""Generates kernels_gen.inc: explicit-assembly kernels (clang inline asm, AT&T syntax) for q44bench.c.

Every kernel is one function  void k_<name>(uint64_t iters, const void *in, void *out)  whose loop body is exactly the
listed instructions plus `dec; jnz`. `insns` in the table is the number of counted instructions per iteration (the
loop counter is not counted). Register use: ymm0-ymm11 accumulators, ymm12 aux/mask, ymm13 = 1.0, ymm14 = b, ymm15 = c.
in: 16 x 32 bytes, loaded into ymm0..ymm15 (or xmm0..xmm15) before the loop; out: 16 x 32 bytes, stored after it.
Memory kernels use a scratch area passed in `out` + 1024 (slots of 32 bytes).
"""
import math
import sys

kernels = []  # (name, insns, body list, width, group, note)


def reg(w, i):
    return f'%{"y" if w == 256 else "x"}mm{i}'


def chain_body(op, w, k, minlen=24, src=None):
    r = max(1, math.ceil(minlen / k))
    body = []
    for _ in range(r):
        for i in range(k):
            body.append(op(w, i))
    return body


OPS = {
    # name: function(width, accumulator index) -> instruction
    'fma': lambda w, i: f'vfmadd213ps {reg(w, 15)}, {reg(w, 14)}, {reg(w, i)}',   # a = b*a + c
    'fadd': lambda w, i: f'vaddps {reg(w, 15)}, {reg(w, i)}, {reg(w, i)}',          # a = a + c
    'fmul': lambda w, i: f'vmulps {reg(w, 14)}, {reg(w, i)}, {reg(w, i)}',          # a = a * b
    'fmaone': lambda w, i: f'vfmadd213ps {reg(w, 15)}, {reg(w, 13)}, {reg(w, i)}',  # a = 1.0*a + c (an add)
    'pshufb': lambda w, i: f'vpshufb {reg(w, 12)}, {reg(w, i)}, {reg(w, i)}',
    'vpaddd': lambda w, i: f'vpaddd {reg(w, 15)}, {reg(w, i)}, {reg(w, i)}',
    'vpmulld': lambda w, i: f'vpmulld {reg(w, 14)}, {reg(w, i)}, {reg(w, i)}',
    'vpermd': lambda w, i: f'vpermd {reg(256, i)}, {reg(256, 12)}, {reg(256, i)}',    # cross-lane, mask ymm12
    'vpermq': lambda w, i: f'vpermq $0x39, {reg(256, i)}, {reg(256, i)}',             # rotate qwords, cross-lane
    'vperm2i128': lambda w, i: f'vperm2i128 $0x01, {reg(256, i)}, {reg(256, i)}, {reg(256, i)}',  # swap halves
}

# Q15/Q22: chain-count sweep.
for op in ('fma', 'fadd', 'fmul'):
    for w in (128, 256):
        for k in (1, 2, 4, 5, 6, 8, 10, 12):
            kernels.append((f'{op}{w}_c{k}', None, chain_body(OPS[op], w, k), w, 'sweep', ''))
# Q24: FADD vs FMA(a,1,b) vs a 50/50 mix, 12 chains, plus the pair mixes that map ops onto pipes.
for w in (128, 256):
    kernels.append((f'fmaone{w}_c12', None, chain_body(OPS['fmaone'], w, 12), w, 'q24', ''))


def mix(a, b, w, k=12, minlen=24):
    """k chains: even chains run op a, odd chains op b, interleaved a b a b."""
    r = max(1, math.ceil(minlen / k))
    body = []
    for _ in range(r):
        for i in range(k):
            body.append(OPS[a if i % 2 == 0 else b](w, i))
    return body


for (a, b) in (('fadd', 'fmaone'), ('fadd', 'fma'), ('fadd', 'fmul'), ('fma', 'fmul'), ('fma', 'pshufb'),
               ('fadd', 'pshufb'), ('fma', 'vpaddd'), ('fadd', 'vpaddd'), ('pshufb', 'vpaddd'), ('fmul', 'pshufb')):
    for w in (128, 256):
        kernels.append((f'mix_{a}_{b}{w}', None, mix(a, b, w), w, 'q24', ''))
# Q25: integer and shuffle throughput (12 chains) and the cross-lane permutes (Q27).
for op in ('pshufb', 'vpaddd', 'vpmulld'):
    for w in (128, 256):
        kernels.append((f'{op}{w}_c12', None, chain_body(OPS[op], w, 12), w, 'q25', ''))
        kernels.append((f'{op}{w}_c1', None, chain_body(OPS[op], w, 1), w, 'q25', 'latency'))
for op in ('vpermd', 'vpermq', 'vperm2i128'):
    kernels.append((f'{op}256_c12', None, chain_body(OPS[op], 256, 12), 256, 'q27', 'cross-lane'))
    kernels.append((f'{op}256_c1', None, chain_body(OPS[op], 256, 1), 256, 'q27', 'cross-lane latency'))
# Q25: scalar integer adds, 12 independent registers (the loop counter is one more ALU op per iteration).
gprs = ['%r8', '%r9', '%r10', '%r11', '%r12', '%r13', '%r14', '%r15']
kernels.append(('scalar_add_c8', None, [f'add $1, {g}' for g in gprs] * 3, 64, 'q25', 'gpr'))
# Q34: clock. 100 dependent 64-bit adds per iteration (latency 1 by the ISA's usual implementation), and imul (3).
kernels.append(('clk_add_chain', None, ['add %rcx, %rax'] * 100, 64, 'clock', 'gpr'))
kernels.append(('clk_imul_chain', None, ['imul %rcx, %rax'] * 100, 64, 'clock', 'gpr'))
# Q34: the clock while FMAs run: 48 dependent adds set the time, 4 independent 256-bit FMAs ride along each iteration.
_b = []
for i in range(48):
    _b.append('add %rcx, %rax')
    if i % 12 == 0:
        _b.append(OPS['fma'](256, i // 12))
kernels.append(('clk_add48_fma4_256', 48, _b, 64, 'clock', 'gpr'))

# Q14: the MSVC pattern. Chains live in 32-byte memory slots (base %rdx = out + 1024), each FMA reads its slot and
# stores the result back, exactly as cpubench-avx2.exe 43AD563A: vmovups c->tmp; vfmadd231ps slot, b, tmp; store.
def memchain(w, nmem, nreg, nodep=False, rep=1):
    body = []
    for _ in range(rep):
        for i in range(nmem):
            t = reg(w, i % 2)
            src = 32 * i
            dst = 32 * (i + 16) if nodep else 32 * i
            body += [f'vmovups {reg(w, 15)}, {t}', f'vfmadd231ps {src}(%rdx), {reg(w, 14)}, {t}',
                     f'vmovups {t}, {dst}(%rdx)']
        for i in range(nreg):
            body.append(OPS['fma'](w, 2 + i))
    return body


for w in (128, 256):
    kernels.append((f'mem_chain10_{w}', 10, memchain(w, 10, 0), w, 'q14', 'MSVC fma256 pattern'))
    kernels.append((f'mem_nodep10_{w}', 10, memchain(w, 10, 0, nodep=True), w, 'q14', 'same, store to unread slot'))
    kernels.append((f'mem_chain1_{w}', 8, memchain(w, 1, 0, rep=8), w, 'q14', 'one chain through memory'))
    kernels.append((f'mem_chain2_reg8_{w}', 10, memchain(w, 2, 8), w, 'q14', 'MSVC fma128 pattern'))

out = []
out.append('/* Generated by gen.py. Do not edit. */')
for (name, insns, body, w, group, note) in kernels:
    n = insns if insns is not None else len(body)
    lines = []
    for i in range(16):
        lines.append(f'vmovups {32 * i}(%[in]), %ymm{i}')
    gpr_init = ['mov $1, %rcx', 'mov $1, %rax'] + [f'mov ${i}, {g}' for i, g in enumerate(gprs)]
    lines += gpr_init
    lines.append('lea 1024(%[out]), %rdx')
    lines.append('1:')
    lines += body
    lines.append('dec %[n]')
    lines.append('jnz 1b')
    for i in range(16):
        lines.append(f'vmovups %ymm{i}, {32 * i}(%[out])')
    lines.append('mov %rax, 512(%[out])')
    lines.append('vzeroupper')
    asm = '\\n\\t'.join(lines).replace('%', '%%').replace('%%[', '%[')
    # The clobber list names every register the body may touch; the compiler saves the callee-saved ones.
    out.append(f'static __attribute__((noinline)) void k_{name}(uint64_t n, const void *in, void *out)\n{{\n'
               f'   __asm__ volatile("{asm}"\n'
               f'      : [n] "+r"(n) : [in] "r"(in), [out] "r"(out)\n'
               f'      : "memory", "cc", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",'
               f'\n'
               f'        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",'
               f' "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");\n}}')
out.append('static const struct kspec { const char *name; int insns; int width; const char *group; const char *note;'
           ' void (*fn)(uint64_t, const void *, void *); } kspecs[] = {')
for (name, insns, body, w, group, note) in kernels:
    n = insns if insns is not None else len(body)
    out.append(f'   {{"{name}", {n}, {w}, "{group}", "{note}", k_{name}}},')
out.append('};')
open(sys.argv[1], 'w', newline='\n').write('\n'.join(out) + '\n')
print(len(kernels), 'kernels')
