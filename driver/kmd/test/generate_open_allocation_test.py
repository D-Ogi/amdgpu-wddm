"""F-q44-mem-3: the actual DxgkDdiOpenAllocation of wddm.c (Bc250WddmOpenAllocation) for open_allocation_test.c,
with the actual object list code it calls (WddmNewObjectPrepared, WddmNewObject, WddmObject, WddmFreeObject), the
actual handle-identity bind (wddm_allocation_identity.inc) and the object magic numbers.

  python generate_open_allocation_test.py <repo> <out.inc>
      [--success-null | --keep-earlier | --count-at-once | --refusal-null | --refusal-uncounted]

The mutations are the negative controls of run_open_allocation.ps1. The first three are each the code before
F-q44-mem-3 in one respect: no failure exit at all, so an admitted blob with no object (and a refused blob) is
answered STATUS_SUCCESS with a NULL handle; the objects opened earlier in the failed call are not freed; the LB7A open
counts go straight into StdAlloc, so an allocation that failed for memory is counted as opened. The last two are each
the code before BD-205 in one respect: a refused blob keeps the BD-060 NULL handle in a call that succeeds; a refused
blob fails the call but is not counted."""
from pathlib import Path
import re
import sys

repo = Path(sys.argv[1])
out = Path(sys.argv[2])


def fail(message):
    # Not an assert: python -O and PYTHONOPTIMIZE strip one (BD-085 review finding 4).
    raise SystemExit('generate_open_allocation_test: ' + message)


def extract(source, marker):
    if source.count(marker) != 1:
        fail('expected exactly one %r' % marker)
    a = source.index(marker)
    i = source.index('{', a) + 1
    depth = 1
    while depth:
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
        i += 1
    return source[a:i]


def sub(text, old, new):
    if text.count(old) != 1:
        fail('mutation anchor moved (%d hits): %.80r' % (text.count(old), old))
    return text.replace(old, new)


wddm = (repo / 'driver/kmd/wddm.c').read_text()
identity = (repo / 'driver/kmd/wddm_allocation_identity.inc').read_text()
magics = re.findall(r'^#define BC250_WDDM_MAGIC_\w+\s+\'[^\']{4}\'', wddm, re.M)
if len(magics) < 6:
    fail('the object magic numbers moved')
code = '\n'.join(magics) + '\n'
for source, marker in ((wddm, 'static BC250_WDDM_OBJECT* WddmNewObjectPrepared('),
                       (wddm, 'static BC250_WDDM_OBJECT* WddmNewObject('),
                       (wddm, 'static BC250_WDDM_OBJECT* WddmObject('),
                       (wddm, 'static void WddmFreeObject('),
                       (identity, 'static void WddmBindHandleIdentity('),
                       (wddm, 'static NTSTATUS Bc250WddmOpenAllocation(')):
    code += extract(source, marker) + '\n\n'

for mutation in sys.argv[3:]:
    if mutation == '--success-null':
        a = code.index('        if (!admitted || opened == NULL)\n')
        b = code.index('            return STATUS_NO_MEMORY;\n        }\n', a) + len('            return STATUS_NO_MEMORY;\n        }\n')
        code = code[:a] + code[b:]
    elif mutation == '--keep-earlier':
        code = sub(code, '                WddmFreeObject(WddmObject(pOpenAllocation->pOpenAllocation[j].hDeviceSpecificAllocation,\n'
                         '                                          BC250_WDDM_MAGIC_OPENED));\n', '')
    elif mutation == '--count-at-once':
        code = sub(code, '                    opens[slot]++;\n',
                   '                    wddm->StdAlloc.GdiOpened[slot][0]++;\n')
        code = sub(code, '            if (opens[slot] != 0) InterlockedAdd(&wddm->StdAlloc.GdiOpened[slot][0], opens[slot]);\n',
                   '            (void)opens[slot];\n')
    elif mutation == '--refusal-null':
        code = sub(code, '        if (!admitted || opened == NULL)\n', '        if (admitted && opened == NULL)\n')
    elif mutation == '--refusal-uncounted':
        code = sub(code, 'InterlockedIncrement(&wddm->UmdOpenRefused) <= BC250_WDDM_LOG_CALLS',
                   'wddm->UmdOpenRefused + 1 <= BC250_WDDM_LOG_CALLS')
        code = sub(code, 'InterlockedIncrement(&wddm->StdAlloc.GdiOpened[slot][1]) <= BC250_WDDM_LOG_CALLS',
                   'wddm->StdAlloc.GdiOpened[slot][1] + 1 <= BC250_WDDM_LOG_CALLS')
    else:
        fail('unknown mutation %r' % mutation)
out.write_text(code)
