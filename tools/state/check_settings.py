"""The settings the kernel driver reads against driver/state/catalog.json (the 'driver-state-settings' gate).

Every GuardReadSetting and GuardConsumeSetting call under driver/kmd (test/ excluded: the host fixtures mock the
function) names a root DWORD entry of the catalog, and a literal default in the call equals the catalog's code
default. A name outside the catalog reads as 0 at run time (DriverStateNumber returns STATUS_INVALID_PARAMETER,
guard.c maps it to 0), which is how BD-190 turned the boot logo and the lab diagnostics off without a line in any
log. The sweep is a small scanner, not a regex: comments are removed, every occurrence of the two names is
classified, and a call whose first argument is not a wide string literal, or whose parentheses do not balance, is a
failure, never a silent skip. The default accepts decimal, hex and octal literals with integer suffixes, redundant
parentheses, TRUE/FALSE and a macro defined with such a literal anywhere under driver/; the name may be a macro
defined as a wide literal. A computed default is
reported in the count and left to the catalog. Exit 1 on any failure.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('GuardReadSetting', 'GuardConsumeSetting')
IDENT = re.compile(r'\b(GuardReadSetting|GuardConsumeSetting)\b')
WIDE = re.compile(r'^L"([A-Za-z0-9]+)"$')
LITERAL = re.compile(r'^(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)[uUlL]*$')
DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\(*\s*(?:0[xX][0-9a-fA-F]+|[0-9]+)\s*[uUlL]*\s*\)*)\s*$', re.M)
NAME_DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(L"[A-Za-z0-9]+")\s*$', re.M)


def strip_comments(text):
    """Replace comments with spaces, keeping every newline so that line numbers stay right."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c == '"' or (c == 'L' and i + 1 < n and text[i + 1] == '"'):
            # keep string literals verbatim (the setting names are wide literals)
            j = i + (2 if c == 'L' else 1)
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def literal(value):
    m = LITERAL.match(value)
    if not m:
        return None
    digits = m.group(1)
    if digits[:2] in ('0x', '0X'):
        return int(digits, 16)
    if len(digits) > 1 and digits[0] == '0':
        return int(digits, 8)
    return int(digits, 10)


def unwrap(arg):
    arg = arg.strip()
    while arg.startswith('(') and arg.endswith(')') and balanced(arg[1:-1]):
        arg = arg[1:-1].strip()
    return arg


def balanced(s):
    depth = 0
    for c in s:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def split_args(s):
    """Split a call's argument text at top-level commas."""
    args, depth, start = [], 0, 0
    for i, c in enumerate(s):
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(s[start:i]); start = i + 1
    args.append(s[start:])
    return [a.strip() for a in args]


def calls(text, where, names):
    """Every occurrence of the two names: ('call', name, line, args) or ('decl', ...) or ('bad', ..., reason)."""
    for m in IDENT.finditer(text):
        line = text.count('\n', 0, m.start()) + 1
        j = m.end()
        while j < len(text) and text[j] in ' \t\r\n':
            j += 1
        if j >= len(text) or text[j] != '(':
            yield ('bad', m.group(1), line, f'{where}:{line}: {m.group(1)} used without a call')
            continue
        depth, k = 0, j
        while k < len(text):
            if text[k] == '(':
                depth += 1
            elif text[k] == ')':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if depth != 0:
            yield ('bad', m.group(1), line, f'{where}:{line}: unbalanced parentheses after {m.group(1)}')
            continue
        args = split_args(text[j + 1:k])
        after = text[k + 1:k + 64].lstrip()
        if args and not WIDE.match(args[0]) and (after.startswith('{') or after.startswith(';')):
            yield ('decl', m.group(1), line, args)     # the definition or a prototype
            continue
        if len(args) == 2 and args[0] in names:
            args = [names[args[0]], args[1]]
        if len(args) != 2 or not WIDE.match(args[0]):
            yield ('bad', m.group(1), line, f'{where}:{line}: {m.group(1)} call not understood: ({", ".join(args)})')
            continue
        yield ('call', m.group(1), line, args)


def sweep(root):
    catalog = json.loads((root / 'driver/state/catalog.json').read_text(encoding='utf-8'))
    settings = {s['name']: s for s in catalog['values'] if s['path'] == ''}
    sources = sorted(p for p in (root / 'driver/kmd').rglob('*.[ch]') if 'test' not in p.relative_to(root / 'driver/kmd').parts)
    macros, names = {}, {}
    for p in sorted((root / 'driver').rglob('*.[ch]')):
        text = strip_comments(p.read_text(encoding='utf-8', errors='replace'))
        for m in DEFINE.finditer(text):
            v = literal(unwrap(m.group(2)))
            if v is not None:
                macros.setdefault(m.group(1), v)
        for m in NAME_DEFINE.finditer(text):
            names.setdefault(m.group(1), m.group(2))    # a setting name behind a macro (fan.c, hwmon.c)
    failures, checked, computed = [], 0, 0
    for p in sources:
        where = p.relative_to(root).as_posix()
        text = strip_comments(p.read_text(encoding='utf-8', errors='replace'))
        for kind, fn, line, args in calls(text, where, names):
            if kind == 'decl':
                continue
            if kind == 'bad':
                failures.append(args)
                continue
            name, default = WIDE.match(args[0]).group(1), unwrap(args[1])
            spec = settings.get(name)
            if spec is None or spec['type'] != 'dword':
                failures.append(f'{where}:{line}: {name} is not a root DWORD entry of the catalog')
                continue
            checked += 1
            value = literal(default)
            if value is None and default in ('TRUE', 'FALSE'):
                value = 1 if default == 'TRUE' else 0
            if value is None and default in macros:
                value = macros[default]
            if value is None:
                computed += 1       # a computed default: the catalog code default still decides at run time
                continue
            if spec['kind'] == 'setting' and spec['code'] is not None and spec['code'] != value:
                failures.append(f'{where}:{line}: {name} default {value} in the call, {spec["code"]} in the catalog')
    return checked, computed, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', default=str(ROOT), help='repository root (default: this checkout)')
    args = parser.parse_args()
    checked, computed, failures = sweep(Path(args.root).resolve())
    for line in failures:
        print('FAIL ' + line)
    print(f'driver-state-settings: {checked} reads match the catalog ({computed} with a computed default), '
          f'{len(failures)} failures')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
