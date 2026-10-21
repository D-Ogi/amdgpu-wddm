"""The failure statuses a KMD DDI entry can return, checked in the sources against the DDI's return table.

    python tools/quality/ddi_returns.py --kmd driver/kmd [--self-test]

DxgkDdiSubmitCommandVirtual may fail with STATUS_INVALID_PARAMETER only. The OS then puts the calling device in an
error state and completes its fence after the packets ahead of it; any other failure is a bugcheck
(ref/ddi-display/d3dkmddi.md, DXGKDDI_SUBMITCOMMANDVIRTUAL, return values). Up to the q44 review the KMD passed
HipJournalBind's STATUS_INVALID_DEVICE_STATE straight out of that DDI.

From the function the DDI table assigns (Data->DxgkDdi<Name> = <function>), the gate follows every return
statement of the KMD's own NTSTATUS functions:
  - STATUS_SUCCESS and the DDI's allowed failures pass;
  - return f(...), with f defined in the KMD, is followed into f;
  - return name, where every assignment to name in that function is an allowed status or a call of a KMD
    function, follows those calls; a name without such an assignment fails;
  - anything else fails (a call the gate cannot read, a ternary, another status).
It reads the sources only: driver/kmd/*.c and *.inc, comments and string literals removed.

--self-test checks the tree, then two mutated copies that must fail: a STATUS_INVALID_DEVICE_STATE return at the
top of the DDI's function, and the HIP journal's status returned again as it was before the fix.
"""
import argparse
from pathlib import Path
import re
import sys

# DDI table member -> the failure statuses its reference page allows.
TABLE = {
    'DxgkDdiSubmitCommandVirtual': {'STATUS_INVALID_PARAMETER'},
}
SUCCESS = 'STATUS_SUCCESS'


def fail(message):
    raise SystemExit('ddi_returns: ' + message)


def strip(text):
    """The C text with comments and string and character literals blanked, newlines kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def matching(text, i, open_char, close_char):
    """Index just past the bracket that closes the one at text[i]."""
    depth = 0
    while i < len(text):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    fail('unbalanced %s%s' % (open_char, close_char))


def definitions(sources):
    """name -> (file, body) of every NTSTATUS function defined in the sources."""
    found = {}
    for name, text in sources.items():
        for m in re.finditer(r'\bNTSTATUS\s+(?:APIENTRY\s+)?(\w+)\s*\(', text):
            end = matching(text, m.end() - 1, '(', ')')
            rest = text[end:].lstrip()
            if not rest.startswith('{'):
                continue                              # a prototype or a function pointer type
            start = text.index('{', end)
            found[m.group(1)] = (name, text[start:matching(text, start, '{', '}')])
    return found


def check(sources, ddi, allowed):
    """(errors, functions followed, returns read) for one DDI table member."""
    defs = definitions(sources)
    entry = None
    for text in sources.values():
        m = re.search(r'\b%s\s*=\s*(\w+)\s*;' % ddi, text)
        if m:
            entry = m.group(1)
    if entry is None or entry not in defs:
        return ['%s: no function assigned in the DDI table' % ddi], 0, 0
    good = allowed | {SUCCESS}
    errors, seen, returns = [], set(), 0

    def follow(fn, why):
        nonlocal returns
        if fn in seen:
            return
        seen.add(fn)
        body = defs[fn][1]
        for m in re.finditer(r'\breturn\b\s*([^;]*);', body):
            returns += 1
            expr = ' '.join(m.group(1).split())
            value(fn, expr, 'return %s' % expr, body, why)

    def value(fn, expr, what, body, why):
        call = re.fullmatch(r'(\w+)\s*\(.*\)', expr)
        if expr in good:
            return
        if call and call.group(1) in defs:
            follow(call.group(1), why + [fn])
            return
        if re.fullmatch(r'\w+', expr) and not expr.startswith('STATUS_'):
            rhs = re.findall(r'(?<![\w.>])%s\s*=(?!=)\s*([^;]+);' % expr, body)
            if not rhs:
                errors.append('%s: %s, and %s is never assigned there (via %s)' % (fn, what, expr, ' > '.join(why)))
            for r in rhs:
                value(fn, ' '.join(r.split()), '%s = %s' % (expr, ' '.join(r.split())), body, why)
            return
        errors.append('%s (%s): %s can carry a status %s may not return (via %s)'
                      % (fn, defs[fn][0], what, ddi, ' > '.join(why + [fn])))

    follow(entry, [ddi])
    return errors, len(seen), returns


def read(kmd):
    files = sorted(list(kmd.glob('*.c')) + list(kmd.glob('*.inc')))
    if not files:
        fail('no sources under %s' % kmd)
    return {f.name: f.read_text(encoding='utf-8', errors='replace') for f in files}


def run(raw, label):
    sources = {name: strip(text) for name, text in raw.items()}
    bad = 0
    for ddi, allowed in sorted(TABLE.items()):
        errors, functions, returns = check(sources, ddi, allowed)
        for e in errors:
            print('FAIL %s %s' % (label, e))
        if not errors:
            print('%s %s: %d functions, %d returns, each %s' % (label, ddi, functions, returns,
                                                               ' or '.join(sorted(allowed | {SUCCESS}))))
        bad += len(errors)
    return bad


def sub(raw, name, old, new):
    if raw[name].count(old) != 1:
        fail('self-test anchor moved in %s (%d hits): %.70r' % (name, raw[name].count(old), old))
    copy = dict(raw)
    copy[name] = raw[name].replace(old, new)
    return copy


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--kmd', type=Path, required=True)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    raw = read(a.kmd)
    bad = run(raw, 'tree')
    if not a.self_test:
        return 1 if bad else 0
    mutants = {
        'early-device-state': sub(raw, 'wddm.c', 'static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter, '
                                  '_In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)\n{\n',
                                  'static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter, '
                                  '_In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)\n{\n'
                                  '    if (hAdapter == NULL) return STATUS_INVALID_DEVICE_STATE;\n'),
        'journal-status': sub(raw, 'wddm.c', 'Submit->SubmissionFenceId, journalStatus);\n        return STATUS_INVALID_PARAMETER;',
                              'Submit->SubmissionFenceId, journalStatus);\n        return journalStatus;'),
    }
    for name, copy in mutants.items():
        if run(copy, 'mutant ' + name) == 0:
            print('FAIL self-test: mutant %s passed' % name)
            bad += 1
        else:
            print('self-test: mutant %s failed as it must' % name)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
