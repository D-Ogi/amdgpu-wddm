"""BD-090: every register that a call of MmioDcnWrite or MmioDcnWriteEx names is on the list that function admits.

MmioDcnWriteEx (mmio.c) admits g_MmioDcnWriteAllow, the flip registers of M87, and in the lab flavour also
g_MmioDcnLabWriteAllow. It refuses any other offset with STATUS_ACCESS_DENIED, at run time, on the lab. The host suites'
register stubs did not know that list, so f50880f4 could write HUBP0_DCHUBP_CNTL there and pass: on unit A every
firmware-surface restore of the 31 line then failed, and the PnP stop handed Windows no POST display.

This reads the call sites in the KMD sources and the two lists in regs.generated.h, resolves each file's own #define
aliases, and refuses a site whose register is on neither list, or on the lab list outside an
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) block. It also refuses a site whose register argument it cannot resolve to a
name, except the escape's pass-through in mmio.c. Before the scan, two fixed snippets are the positive and the
negative control.
"""
from pathlib import Path
import argparse
import re
import sys

CALL = re.compile(r'\bMmioDcnWrite(?:Ex)?\s*\(\s*(\w+)\s*,\s*([^,()]+?)\s*,')
ALIAS = re.compile(r'^\s*#\s*define\s+(\w+)\s+(BC250_REG_DMU_\w+)\s*$', re.M)
LAB = 'AMDGPU_WDDM_LAB_FLAVOUR'
PASS_THROUGH = {('mmio.c', 'Offset')}


def strip_comments(text):
    """C comments to spaces, newlines kept, string and character literals left alone."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def lab_lines(text):
    """For each line (0-based), whether it lies inside an #if block that only the lab flavour compiles."""
    stack, inside = [], []
    for line in text.split('\n'):
        d = line.strip()
        if re.match(r'#\s*if', d):
            if re.match(r'#\s*ifdef\s+' + LAB + r'\b', d) or re.match(r'#\s*if\s+defined\s*\(\s*' + LAB + r'\s*\)\s*$', d):
                stack.append('lab')
            elif re.match(r'#\s*ifndef\s+' + LAB + r'\b', d) or re.match(r'#\s*if\s+!\s*defined\s*\(\s*' + LAB + r'\s*\)\s*$', d):
                stack.append('release')
            else:
                stack.append('other')
        elif re.match(r'#\s*el', d) and stack:
            stack[-1] = {'lab': 'release', 'release': 'lab'}.get(stack[-1], 'other')
        elif re.match(r'#\s*endif', d) and stack:
            stack.pop()
        inside.append('lab' in stack)
    return inside


def tables(regs):
    text = regs.read_text(encoding='utf-8')
    names = {m.group(1): int(m.group(2), 16)
             for m in re.finditer(r'#define (BC250_REG_DMU_\w+) 0x([0-9A-Fa-f]+)ul', text)}

    def table(name):
        m = re.search(r'static const unsigned long ' + name + r'\[\w+\] = \{(.*?)\};', text, re.S)
        if not m:
            sys.exit(f'{regs}: no table {name}')
        return {int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))}
    return names, table('g_MmioDcnWriteAllow'), table('g_MmioDcnLabWriteAllow')


def scan(name, text, names, release, lab):
    """The refusals of one source file: (line, register argument, reason)."""
    code = strip_comments(text)
    aliases = dict(ALIAS.findall(code))
    inside = lab_lines(code)
    refusals, sites = [], 0
    for m in CALL.finditer(code):
        sites += 1
        line = code.count('\n', 0, m.start())
        argument = m.group(2).strip()
        register = aliases.get(argument, argument)
        if register not in names:
            if (name, argument) not in PASS_THROUGH:
                refusals.append((line + 1, argument, 'not a register name this check can resolve'))
            continue
        offset = names[register]
        if offset in release:
            continue
        if offset in lab and inside[line]:
            continue
        reason = ('on the lab list only, outside a lab-flavour block' if offset in lab else
                  'not on g_MmioDcnWriteAllow: MmioDcnWriteEx refuses it with STATUS_ACCESS_DENIED')
        refusals.append((line + 1, register, reason))
    return sites, refusals


def controls(names, release, lab):
    by_offset = {v: k for k, v in names.items()}
    allowed = by_offset[min(release)]
    refused = 'BC250_REG_DMU_HUBP0_DCHUBP_CNTL'
    cases = [(f'x = MmioDcnWriteEx(Device, {allowed}, v, TRUE);', 0),
             (f'x = MmioDcnWriteEx(Device, {refused}, v, TRUE);', 1)]
    if lab:
        lab_name = by_offset[min(lab)]
        cases += [(f'#if defined({LAB})\nx = MmioDcnWriteEx(Device, {lab_name}, v, TRUE);\n#endif\n', 0),
                  (f'x = MmioDcnWriteEx(Device, {lab_name}, v, TRUE);', 1)]
    passed = 0
    for snippet, expected in cases:
        if len(scan('control.c', snippet, names, release, lab)[1]) == expected:
            passed += 1
        else:
            print(f'FAIL control: expected {expected} refusal(s) for {snippet!r}')
    return passed, len(cases)


def main():
    p = argparse.ArgumentParser()
    here = Path(__file__).resolve().parents[1]
    p.add_argument('--source', type=Path, default=here, help='directory of the KMD .c files')
    p.add_argument('--regs', type=Path, default=None, help='regs.generated.h (default: in --source, else the tree)')
    a = p.parse_args()
    regs = a.regs or (a.source / 'regs.generated.h' if (a.source / 'regs.generated.h').exists() else here / 'regs.generated.h')
    names, release, lab = tables(regs)
    passed, total = controls(names, release, lab)
    files = sorted(a.source.glob('*.c'))
    sites, refused = 0, 0
    for f in files:
        n, refusals = scan(f.name, f.read_text(encoding='utf-8'), names, release, lab)
        sites += n
        for line, register, reason in refusals:
            refused += 1
            print(f'FAIL {f.name}:{line}: {register}: {reason}')
    print(f'dcn write sites: {sites} sites in {len(files)} files, {refused} refused; controls {passed} of {total}')
    return 1 if refused or passed != total or not sites else 0


if __name__ == '__main__':
    sys.exit(main())
