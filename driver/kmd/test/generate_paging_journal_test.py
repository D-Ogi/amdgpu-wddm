"""Extract the paging journal of paging_journal.c for paging_journal_test.c (BD-235 Q3a).

    python generate_paging_journal_test.py <out .inc> [<source root>] [<mutation>]

The test compiles paging_journal.c's own code without its #include lines: the record and the ring come from the
driver's headers (bc250kmd_escape.h, paging_journal.h, paging_identity.h), the kernel calls and the DXGK types are
mocks in the test. A mutation (a negative control) edits the extracted copy, never the tree:
    stamp-failed  the fence and sequence stamps take a failed build's record (the rule before BD-235 Q3a)
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[3]
source = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else repo
mutation = sys.argv[3] if len(sys.argv) > 3 else 'none'

journal = (source / 'driver/kmd/paging_journal.c').read_text(encoding='utf-8').replace('\r\n', '\n')
lines = journal.split('\n')
includes = [l for l in lines if l.startswith('#include')]
assert includes == ['#include "bc250kmd.h"', '#include "paging_journal.h"', '#include "paging_identity.h"'], \
    'paging_journal.c includes changed: %r' % includes
body = '\n'.join(l for l in lines if not l.startswith('#include'))
for name in ('void PagingJournalInit(', 'void PagingJournalNote(', 'void PagingJournalUpdate(',
             'void PagingJournalStampFence(', 'void PagingJournalStampSeq(', 'ULONG PagingJournalRead('):
    assert body.count(name) == 1, name + ' must be defined once in paging_journal.c'

if mutation == 'stamp-failed':
    skip = '        if (r->Flags & BC250_PJ_FLAG_FAILED) continue;\n'
    assert body.count(skip) == 2, 'stamp-failed: the two stamp walks must each skip a failed record once'
    body = body.replace(skip, '        /* negative control: a failed record is stamped */\n')
elif mutation != 'none':
    raise SystemExit('unknown mutation ' + mutation)

Path(sys.argv[1]).write_text(body, encoding='utf-8')
