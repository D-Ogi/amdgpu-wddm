"""Extract the BuildPagingBuffer DDI of wddm.c for paging_build_test.c (BD-235).

    python generate_paging_build_test.py <out .inc> [<source root>] [<mutation>]

The test compiles wddm.c's own WddmPagingBuildPosition, WddmCountTransfer, WddmBuildPagingBufferImpl and
Bc250WddmBuildPagingBuffer, with the two enums they use from bc250kmd.h and wddm.c and the paging journal kinds from
bc250kmd_escape.h; everything they call is a mock in the test. A mutation (a negative control) edits the extracted
copy, never the tree:
    absorb-inner-failure  a failed GPU_PHYSICAL update or FLUSH_TLB falls through to SUCCESS (the rule before BD-235 Q3)
    cpu-virtual-ignore    a failed CPU_VIRTUAL update is journalled and reported as SUCCESS (the rule before BD-235 Q3)
"""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[3]
source = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else repo
mutation = sys.argv[3] if len(sys.argv) > 3 else 'none'


def function(s, sig):
    start = s.index(sig)
    return s[start:s.index('\n}\n', start) + 3]


def once(s, anchor, replacement, name):
    hits = s.count(anchor)
    assert hits == 1, '%s: its anchor matched %d times, once expected' % (name, hits)
    return s.replace(anchor, replacement)


wddm = (source / 'driver/kmd/wddm.c').read_text(encoding='utf-8').replace('\r\n', '\n')
header = (source / 'driver/kmd/bc250kmd.h').read_text(encoding='utf-8').replace('\r\n', '\n')
escape = (source / 'driver/kmd/bc250kmd_escape.h').read_text(encoding='utf-8').replace('\r\n', '\n')

a = header.index('typedef enum _BC250_WDDM_PAGING_UNSUPPORTED')
unsupported = header[a:header.index('} BC250_WDDM_PAGING_UNSUPPORTED;', a) + len('} BC250_WDDM_PAGING_UNSUPPORTED;')]
a = wddm.index('typedef enum _BC250_WDDM_XFER {')
xfer = wddm[a:wddm.index('} BC250_WDDM_XFER;', a) + len('} BC250_WDDM_XFER;')]
kinds = []
for name in ('BC250_PJ_FILL', 'BC250_PJ_FLUSH_TLB', 'BC250_PJ_TRANSFER', 'BC250_PJ_VIRTUAL_FILL',
             'BC250_PJ_VIRTUAL_TRANSFER', 'BC250_PJ_FLAG_TO_SYSTEM'):
    found = re.findall(r'^#define ' + name + r'\b.*$', escape, re.M)
    assert len(found) == 1, name + ' must be defined once in bc250kmd_escape.h'
    kinds.append(found[0])

a = wddm.index('static ULONGLONG WddmPagingBuildPosition(')
b = wddm.index('\n}\n', wddm.index('static NTSTATUS Bc250WddmBuildPagingBuffer(HANDLE')) + 3
ddi = wddm[a:b]
assert 'static NTSTATUS WddmBuildPagingBufferImpl(' in ddi, 'the extraction must hold WddmBuildPagingBufferImpl'

if mutation == 'absorb-inner-failure':
    ddi = once(ddi, '            return pagingStatus;\n        }\n        if (written != 0)',
               '            /* negative control: the failure is absorbed */\n        }\n        if (written != 0)', mutation)
elif mutation == 'cpu-virtual-ignore':
    ddi = once(ddi, '        NTSTATUS status=VidMmUpdatePageTable(&pBuildPagingBuffer->UpdatePageTable);\n'
                    '        if (status != STATUS_SUCCESS) {',
               '        NTSTATUS status=VidMmUpdatePageTable(&pBuildPagingBuffer->UpdatePageTable);\n'
               '        if (0) { /* negative control: the status is ignored */', mutation)
elif mutation != 'none':
    raise SystemExit('unknown mutation ' + mutation)

# The types go first (the test's BC250_WDDM sizes arrays by them), the functions after the mocks.
Path(sys.argv[1]).with_name('paging_build_types.inc').write_text('\n'.join([unsupported, xfer] + kinds) + '\n',
                                                                 encoding='utf-8')
Path(sys.argv[1]).write_text(ddi, encoding='utf-8')
