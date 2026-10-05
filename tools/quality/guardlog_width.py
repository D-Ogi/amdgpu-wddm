"""Worst-case width of every GuardLog format against the driver log ring's line.

    python guardlog_width.py [--kmd <dir>] [--baseline <file>] [--limit 159] [--out <dir>]
    python guardlog_width.py --list-over          # the over-width formats as baseline lines
    python guardlog_width.py --prune              # remove baseline lines that no longer apply

GuardLog prints into `char line[BC250_LOG_TEXT]` with RtlStringCchVPrintfA (driver/kmd/guard.c).
BC250_LOG_TEXT is 160 bytes, the terminator included, and the print truncates without a word.
A format whose widest printing is longer than 159 characters therefore loses its last fields on
the lab. That is BD-070: the scan-out summary of 0.7.207.1 printed "... format/geometry/pitch/
size/segment/alignment/gated 0/0/" and the refusal counts were gone.

A conversion counts at the width its type can print: %ld/%d/%i 11, %u/%lu 10, %lld/%llu 20,
%x/%lx 8, %llx/%I64x/%p 16, %c 1, %s/%S/%ws/%Z 32 (a name), %% 1. Adjacent string literals are
joined as the compiler joins them. The count is a worst case: a %s that always holds "open" or a
counter that never passes four digits prints shorter. A line that only the worst case puts over
the limit belongs in the baseline, not in a split.

The gate reads the driver's own translation units and the files they include by text: *.c, *.inc
and *.h directly under <root>. The host tests in driver/kmd/test stub GuardLog and write to no log
ring, so they stay out.

The baseline is a ratchet. It names every format that was already over the limit when this gate
landed, by file and format text (not by line number, so an edit somewhere else in the file does
not move it), with the width it had. The gate fails when
  - a format outside the baseline is over the limit, or
  - a baselined format gets wider.
It passes, and prints a note to remove the baseline line, when a baselined format shrinks under
the limit or is no longer in the sources. Nothing adds a baseline line by itself: --list-over
prints the lines for a human to review, and --prune only removes stale ones. --prune also writes
the width each kept format has now, so a format that got shorter cannot grow back to its old
allowance. It refuses to touch the file while the gate fails.
"""
import argparse
import pathlib
import re
import sys

LIMIT = 159                     # BC250_LOG_TEXT 160 bytes, the terminator included

# wddm.c includes wddm_allocation_identity.inc, which holds a GuardLog call of its own, and a
# header could hold one too. A suffix left out here is a format the gate cannot see.
SUFFIXES = ('.c', '.inc', '.h')

WIDTH = [(r'%%', 1), (r'%[-+ #0]*\d*(?:ll|I64)[dixXu]', 20), (r'%[-+ #0]*\d*l?[di]', 11),
         (r'%[-+ #0]*\d*l?u', 10), (r'%[-+ #0]*\d*l?[xX]', 8), (r'%[-+ #0]*\d*p', 16),
         (r'%[-+ #0]*\d*c', 1), (r'%[-+ #0]*\d*(?:\.\d+|\.\*)?(?:s|S|ws|wZ|Z)', 32)]

CALL = re.compile(r'\bGuardLog\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
PIECE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def width(fmt):
    """Characters the widest printing of this format can take."""
    total, i = 0, 0
    while i < len(fmt):
        if fmt[i] == '%':
            for pattern, w in WIDTH:
                m = re.match(pattern, fmt[i:])
                if m:
                    total += w
                    i += len(m.group(0))
                    break
            else:
                total += 1
                i += 1
        else:
            total += 1
            i += 1
    return total


def sources(root):
    """The files directly under <root> whose text the gate reads, sorted by name."""
    return sorted(p for p in pathlib.Path(root).iterdir()
                  if p.is_file() and p.suffix in SUFFIXES)


def formats(root):
    """[(file name, format text, width)] for every GuardLog call under <root>, sorted."""
    out = {}
    for path in sources(root):
        text = path.read_text(encoding='utf-8', errors='replace')
        for m in CALL.finditer(text):
            fmt = ''.join(PIECE.findall(m.group(1))).replace('\\n', '').replace('\\"', '"')
            if '\t' in fmt:
                raise ValueError('%s: a GuardLog format holds a tab, which the baseline file '
                                 'cannot carry: %r' % (path.name, fmt))
            out[(path.name, fmt)] = width(fmt)
    return sorted((f, t, w) for (f, t), w in out.items())


def line(file, fmt, w):
    return '%d\t%s\t%s' % (w, file, fmt)


def load(path):
    """{(file name, format text): width} from a baseline file. Missing file = empty baseline."""
    out = {}
    path = pathlib.Path(path)
    if not path.exists():
        return out
    for n, text in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not text.strip() or text.lstrip().startswith('#'):
            continue
        parts = text.split('\t')
        if len(parts) != 3 or not parts[0].strip().isdigit():
            raise ValueError('%s line %d is not "<width><tab><file><tab><format>": %r'
                             % (path, n, text))
        out[(parts[1], parts[2])] = int(parts[0])
    return out


def check(root, baseline, limit=LIMIT):
    """(failures, notes, report lines) for the sources under <root> against a loaded baseline."""
    found = formats(root)
    failures, notes, report = [], [], []
    seen = set()
    for file, fmt, w in found:
        seen.add((file, fmt))
        allowed = baseline.get((file, fmt))
        if w <= limit:
            if allowed is not None:
                notes.append('%s now prints at most %d characters: remove its baseline line\n'
                             '    %s' % (file, w, fmt))
            continue
        if allowed is None:
            failures.append('%s %d > %d characters, and it is not in the baseline: split the '
                            'line or shorten it\n    %s' % (file, w, limit, fmt))
        elif w > allowed:
            failures.append('%s %d characters, wider than the baselined %d: split the line or '
                            'shorten it\n    %s' % (file, w, allowed, fmt))
        else:
            report.append('baselined %s %d > %d\n    %s' % (file, w, limit, fmt))
    for file, fmt in sorted(baseline):
        if (file, fmt) not in seen:
            notes.append('no GuardLog call in %s has this format any more: remove its baseline '
                         'line\n    %s' % (file, fmt))
    return failures, notes, report


def main(argv=None):
    here = pathlib.Path(__file__).resolve()
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--kmd', default=str(here.parents[2] / 'driver' / 'kmd'))
    p.add_argument('--baseline', default=str(here.with_name('guardlog_width_baseline.txt')))
    p.add_argument('--limit', type=int, default=LIMIT)
    p.add_argument('--out', default='')
    p.add_argument('--list-over', action='store_true',
                   help='print the over-width formats as baseline lines and exit')
    p.add_argument('--prune', action='store_true',
                   help='remove the baseline lines the notes ask for, tighten the widths of the '
                        'lines that stay, then exit')
    a = p.parse_args(argv)

    found = formats(a.kmd)
    if a.list_over:
        for file, fmt, w in found:
            if w > a.limit:
                print(line(file, fmt, w))
        return 0

    baseline = load(a.baseline)
    failures, notes, report = check(a.kmd, baseline, a.limit)

    if a.prune:
        path = pathlib.Path(a.baseline)
        if failures:
            # A prune writes the widths it measures now. With a failure in the sources that would
            # hand the offending format an allowance, which is the one thing a ratchet must not do.
            print('guardlog-width: the gate fails, so nothing was pruned. %d format(s) over the '
                  'limit outside %s:' % (len(failures), path))
            print('\n'.join('FAIL: ' + t for t in failures))
            return 1
        keep = {(f, t): w for f, t, w in found if w > a.limit}
        kept, written = [], 0
        for text in path.read_text(encoding='utf-8').splitlines():
            if not text.strip() or text.lstrip().startswith('#'):
                kept.append(text)
                continue
            parts = text.split('\t')
            key = (parts[1], parts[2]) if len(parts) == 3 else None
            if key not in keep:
                continue
            # The width the format has now, not the width it had: a format that got shorter must
            # not keep room to grow back to its old length.
            kept.append(line(key[0], key[1], keep[key]))
            written += 1
        # LF, as .gitattributes asks for this file, so that a prune is not also a line-ending diff.
        path.write_text('\n'.join(kept) + '\n', encoding='utf-8', newline='\n')
        print('guardlog-width: %d baseline line(s) written to %s, %d removed'
              % (written, path, len(baseline) - written))
        return 0

    text = ['guardlog-width: %d GuardLog formats in %s, limit %d characters, %d baselined'
            % (len(found), a.kmd, a.limit, len(baseline))]
    text += ['  ' + t for t in report]
    text += ['note: ' + t for t in notes]
    text += ['FAIL: ' + t for t in failures]
    if failures:
        text.append('guardlog-width: FAIL, %d format(s) over the limit outside the baseline'
                    % len(failures))
    else:
        text.append('guardlog-width: PASS, %d baselined format(s) over the limit, %d note(s)'
                    % (len(report), len(notes)))
    print('\n'.join(text))
    if a.out:
        out = pathlib.Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / 'guardlog-width.txt').write_text('\n'.join(text) + '\n', encoding='utf-8')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
