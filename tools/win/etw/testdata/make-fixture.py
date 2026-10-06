#!/usr/bin/env python3
"""make-fixture.py - trim an xperf dumper text down to a committable fixture for etw-present-mode-test.py.

    python make-fixture.py BIG-DUMP.txt --out base-composed-excerpt.txt --from 2.0 --to 3.0

A full dump of a 15 s present-mode capture is 51 MB, which belongs nowhere near a repository. This keeps only
the event kinds `etw-present-mode.py` reads, only inside one time window, and only the present packets of
`QueuePacket Start` (the parser ignores the rest, and they are nine tenths of the rows). The header block keeps
the matching header lines and `EndHeader`, so the result is a dumper text the parser reads unchanged.

Timestamps, pids, thread ids, kernel pointers and physical addresses stay as they were: the fixture is evidence
of what the shapes look like, and a rewritten number would make the test prove nothing. Nothing in these event
kinds carries a user name, a SID, a host name or a network address; `--check` says so for a given file before it
is committed.
"""
import argparse
import re
import sys
from pathlib import Path

# The event kinds etw-present-mode.py reads. A closed list, so the fixture does not drift with the trace.
KEEP = (
    'Microsoft-Windows-DxgKrnl/PresentHistory/win:Start',
    'Microsoft-Windows-DxgKrnl/PresentHistory/win:Info',
    'Microsoft-Windows-DxgKrnl/PresentHistory/win:Stop',
    'Microsoft-Windows-DxgKrnl/PresentHistoryDetailed/win:Start',
    'Microsoft-Windows-DxgKrnl/Present/win:Info',
    'Microsoft-Windows-DxgKrnl/QueuePacket/win:Start',
    'Microsoft-Windows-DxgKrnl/IndependentFlip/win:Info',
    'Microsoft-Windows-DxgKrnl/MMIOFlip/win:Info',
    'Microsoft-Windows-DxgKrnl/Flip/win:Info',
    'Microsoft-Windows-DxgKrnl/VSyncDPC/win:Info',
    'Microsoft-Windows-DxgKrnl/VSyncInterrupt/win:Info',
    'Microsoft-Windows-Win32k/TokenCompositionSurfaceObject/win:Info',
    'Microsoft-Windows-Win32k/TokenStateChanged/win:Info',
    'Microsoft-Windows-Dwm-Core/SCHEDULE_SURFACEUPDATE/win:Info',
    'Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed/win:Info',
    'Microsoft-Windows-Dwm-Core/Windowed_Dx_Flip_Consumed/win:Info',
    'Microsoft-Windows-Dwm-Core/SCHEDULE_PRESENT/win:Start',
)
QUEUE_PACKET = 'Microsoft-Windows-DxgKrnl/QueuePacket/win:Start'

# What must not reach a repository from a trace. The dumper writes the SID column empty for these providers,
# and none of them carries a host or network name, but a fixture is checked instead of assumed.
FORBIDDEN = (
    ('a Windows SID', re.compile(r'\bS-1-5-21-\d')),
    ('an IPv4 address', re.compile(r'\b(?:\d{1,3}\.){3}\d{1,3}\b')),
    ('a UNC path', re.compile(r'\\\\[A-Za-z0-9_-]{2,}\\')),
    ('a user profile path', re.compile(r'(?i)[A-Z]:\\Users\\')),
)


def check(path):
    """Refuse a fixture that carries anything that must stay out of the repository."""
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    bad = [what for what, pattern in FORBIDDEN if pattern.search(text)]
    for what in bad:
        print('REFUSED: %s looks like it contains %s' % (path, what))
    if not bad:
        print('ok   %s carries no SID, address, UNC path or profile path' % path)
    return 1 if bad else 0


def trim(source, first_us, last_us):
    """The header lines of the kept event kinds, EndHeader, then the kept rows of the window."""
    out = []
    kept = dropped = 0
    with open(source, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('EndHeader'):
                out.append('EndHeader')
                break
            name = line.split(',', 1)[0].strip()
            if name in KEEP:
                out.append(line.rstrip('\n'))
        else:
            raise SystemExit('%s has no EndHeader: not an xperf dumper text' % source)
        for line in f:
            parts = [p.strip() for p in line.rstrip('\n').split(',')]
            if len(parts) < 4 or not parts[1].isdigit() or parts[0] not in KEEP:
                continue
            time = int(parts[1])
            if time < first_us or time >= last_us:
                continue
            if parts[0] == QUEUE_PACKET and 'true' not in [p.lower() for p in parts]:
                dropped += 1          # a packet that is not a present is not a frame; the parser skips it
                continue
            out.append(line.rstrip('\n'))
            kept += 1
    return out, kept, dropped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', help='the full xperf dumper text, or the fixture to --check')
    ap.add_argument('--out', help='where to write the trimmed fixture')
    ap.add_argument('--from', dest='start', type=float, default=0.0, help='window start, seconds (default 0)')
    ap.add_argument('--to', dest='stop', type=float, help='window end, seconds (default: the whole trace)')
    ap.add_argument('--check', action='store_true', help='only check a file for things that must not be committed')
    args = ap.parse_args(argv)
    if args.check:
        return check(args.dump)
    if not args.out:
        raise SystemExit('--out is required unless --check is given')
    last = int(args.stop * 1e6) if args.stop is not None else 1 << 62
    lines, kept, dropped = trim(args.dump, int(args.start * 1e6), last)
    Path(args.out).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    size = Path(args.out).stat().st_size
    print('%s: %d rows kept, %d non-present packets dropped, %.1f KB' % (args.out, kept, dropped, size / 1024.0))
    return check(args.out)


if __name__ == '__main__':
    sys.exit(main())
