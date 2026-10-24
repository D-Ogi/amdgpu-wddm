"""BD-235: who writes the SDMA0 ring, and why no raw writer runs beside OS paging.

The SDMA0 ring does not track its read pointer (track_rptr false), so amdgpu_ring_alloc() cannot see unretired work on
it. The ring boundary pad and the request fit the ring only when no other writer has unretired work beside the paging
path (bc250_ring.c, gfx.c GFX_BOUNDARY_NOTE). This check holds that property in the source:

1. The writers are the functions that take Sdma0RingLock. They are exactly WRITERS. A function that writes a ring
   (amdgpu_ring_alloc and the other RING_WRITE calls) is a writer or one of HELPERS, which only writers call.
2. Every function that reaches a writer, by a call or through a table (g_Stages), is pinned in EDGES, up to a
   boundary function whose exclusion is the argument:
   - Bc250Escape: the raw writer escapes (RUN_GFX other than STATE, RUN_FENCE, RUN_SDMACOPY, RUN_SDMAIB) run only
     after the FullWddm admission check refuses every command outside WddmDiagnosticAllowed. OS paging exists only
     with the full table (entry.c, WddmGateOpen), which is also what sets FullWddm. The admission table itself is
     tested by escape-admission (escape_admission_test.c, both flavours).
   - GpuStartupInitialize: before WDDM publication (WddmStart publishes Device->Wddm only after it succeeds), and
     SdmaIbStartupControl refuses when Device->Wddm is set.
   - GfxPowerRetainedLocked: writes SDMA0 (the ring tests of the stages) only on resume, when PowerSuspended has kept
     the paging path from admitting a submission since the suspend, and after GfxAccessClose has drained access.
   - GfxSubmitPaging: the paging path, one submission at a time (PagingSubmitInFlight until its fence retires).
3. The orders those arguments need, in the source text:
   - Bc250Escape calls no raw writer before the FullWddm admission check;
   - SdmaIbStartupControl refuses a published adapter before its first SdmaCopyEscape call;
   - GfxPowerRetainedLocked closes access before it runs a stage, and refuses a resume that is not suspended;
   - GfxSubmitPagingAccess claims PagingSubmitInFlight before it takes Sdma0RingLock.
4. No preprocessor line names a function of this graph (a macro could add a path that the scan does not see).

A change to any of them fails here, so that the change comes with its own argument. Before the scan of the tree, five
mutations of the real source must each fail with their own finding; that is the negative control.
"""
from pathlib import Path
import argparse
import re
import sys

from check_dcn_write_sites import strip_comments

WRITERS = {'GfxFenceEscape', 'GfxSubmitPagingAccess', 'SdmaCopyEscape', 'SdmaPteStartupControl',
           'SdmaVaStartupControl', 'StageSdma'}
# Functions that write the ring under the lock that their caller holds: called only by a writer.
HELPERS = {'GfxSdmaBoundaryLocked', 'PagingWriteRing'}
BOUNDARY = {'Bc250Escape', 'GpuStartupInitialize', 'GfxPowerRetainedLocked', 'GfxSubmitPaging'}
EDGES = {
    ('Bc250Escape', 'GfxEscape'), ('Bc250Escape', 'GfxFenceEscape'), ('Bc250Escape', 'SdmaCopyEscape'),
    ('GfxEscape', 'GfxExecute'), ('GfxExecute', 'RunEngineStage'), ('GfxExecute', 'g_Stages'),
    ('GfxInitializeHardware', 'GfxExecute'), ('GfxInitializeHardware', 'SdmaIbStartupControl'),
    ('GfxPowerRetainedLocked', 'RunEngineStage'), ('GfxSubmitPaging', 'GfxSubmitPagingAccess'),
    ('GpuStartupInitialize', 'GfxInitializeHardware'), ('RunEngineStage', 'g_Stages'),
    ('SdmaIbStartupControl', 'SdmaCopyEscape'), ('SdmaIbStartupControl', 'SdmaVaStartupControl'),
    ('SdmaVaStartupControl', 'SdmaPteStartupControl'), ('g_Stages', 'StageSdma'),
}
RAW_CALLS = ('GfxEscape(', 'GfxFenceEscape(', 'SdmaCopyEscape(')
ORDERS = [
    ('Bc250Escape', 'device->FullWddm && !WddmDiagnosticAllowed(', RAW_CALLS),
    ('SdmaIbStartupControl', 'Device->Wddm != NULL', ('SdmaCopyEscape(',)),
    ('GfxPowerRetainedLocked', 'if (Resume && !Gfx->PowerSuspended) return', ('GfxAccessClose(',)),
    ('GfxPowerRetainedLocked', 'GfxAccessClose(Device);', ('RunEngineStage(',)),
    ('GfxSubmitPagingAccess', 'InterlockedCompareExchange(&gfx->PagingSubmitInFlight, 1, 0)', ('Sdma0RingLock',)),
]

IDENT = re.compile(r'[A-Za-z_]\w*')
# A definition starts at column 0 and its body ends at the first closing brace at column 0 (the KMD's style).
FN = re.compile(r'^(?![#\s{}])[^;{}()]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{', re.M)
TABLE = re.compile(r'^(?![#\s{}])(?:[^;{}()=]|\{[^{}]*\})*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*=\s*\{', re.M)
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof'}
# The calls that write a ring (allocate, write, commit, undo, emit, submit). The bc250_sdma_paging_* builders write
# packets into a buffer, not into the ring.
RING_WRITE = re.compile(r'\b(?:amdgpu_ring_(?:alloc|commit|write\w*|insert_nop|undo)|'
                        r'bc250_sdma_(?:ring_test\w*|emit_\w+|submit_\w+|signal_fence|copy_test))\s*\(')
LOCK = re.compile(r'Acquire\w*\s*\(\s*&\s*\w+\s*->\s*Sdma0RingLock')


def strip_strings(text):
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def scan(sources):
    """sources: {file name: text}. Returns the findings, an empty list when the tree holds the property."""
    nodes, defines = {}, []
    for name, raw in sources.items():
        text = strip_strings(strip_comments(raw))
        defines += [(name, line) for line in text.split('\n') if line.lstrip().startswith('#')]
        for rx in (FN, TABLE):
            for m in rx.finditer(text):
                if m.group(1) in KEYWORDS:
                    continue
                end = text.find('\n}', m.end())
                nodes.setdefault(m.group(1), []).append(text[m.end() - 1:end + 2])
    findings = []
    writers = {n for n, bodies in nodes.items() if any(LOCK.search(b) for b in bodies)}
    if writers != WRITERS:
        findings.append('writers of SDMA0: new %s, gone %s' % (sorted(writers - WRITERS), sorted(WRITERS - writers)))
    ring = {n for n, bodies in nodes.items() if any(RING_WRITE.search(b) for b in bodies)}
    if ring != WRITERS | HELPERS:
        findings.append('ring writes outside the writers and helpers: new %s, gone %s'
                        % (sorted(ring - WRITERS - HELPERS), sorted((WRITERS | HELPERS) - ring)))
    callers = {}
    for name, bodies in nodes.items():
        for ref in {x for b in bodies for x in IDENT.findall(b)}:
            if ref in nodes and ref != name:
                callers.setdefault(ref, set()).add(name)
    edges, seen, stack = set(), set(writers), list(writers)
    while stack:
        callee = stack.pop()
        for caller in callers.get(callee, ()):
            edges.add((caller, callee))
            if caller not in seen:
                seen.add(caller)
                if caller not in BOUNDARY:
                    stack.append(caller)
    for helper in sorted(HELPERS):
        if not callers.get(helper) or not callers[helper] <= WRITERS:
            findings.append('%s: called by %s, only by writers expected' % (helper, sorted(callers.get(helper, ()))))
    if edges != EDGES:
        findings.append('paths to SDMA0 writers: new %s, gone %s' % (sorted(edges - EDGES), sorted(EDGES - edges)))
    for function, first, later in ORDERS:
        bodies = nodes.get(function, [])
        if len(bodies) != 1:
            findings.append('%s: %d definitions, 1 expected' % (function, len(bodies)))
            continue
        body = bodies[0]
        at = body.find(first)
        if at < 0:
            findings.append('%s: no "%s"' % (function, first))
            continue
        for call in later:
            before = body.find(call)
            if before < 0:
                findings.append('%s: no "%s"' % (function, call))
            elif before < at:
                findings.append('%s: "%s" before "%s"' % (function, call, first))
    graph = seen | {c for e in EDGES for c in e}
    for name, line in defines:
        counted = re.sub(r'\b(?:RTL_NUMBER_OF|sizeof)\s*\(\s*\w+\s*\)', '', line)   # a size is not a path
        hit = sorted(set(IDENT.findall(counted)) & graph)
        if hit:
            findings.append('%s: a preprocessor line names %s: %s' % (name, hit, line.strip()))
    return findings


def mutate(sources, file, old, new):
    text = sources[file]
    if text.count(old) != 1:
        raise SystemExit('mutation anchor not found once in %s: %s' % (file, old))
    out = dict(sources)
    out[file] = text.replace(old, new)
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    a = p.parse_args()
    sources = {f.name: f.read_text(encoding='utf-8', errors='replace') for f in sorted(a.source.glob('*.c'))}
    escape = 'device->FullWddm && !WddmDiagnosticAllowed('
    controls = [
        ('a new writer of the ring', 'writers of SDMA0',
         mutate(sources, 'gfx.c', 'static BOOLEAN GfxPagingSubmitReadyAccess(',
                'static void LabNewWriter(BC250_GFX* gfx)\n{\n    KIRQL irql;\n'
                '    KeAcquireSpinLock(&gfx->Sdma0RingLock, &irql);\n    KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);\n}\n\n'
                'static BOOLEAN GfxPagingSubmitReadyAccess(')),
        ('a raw writer before the admission check', 'Bc250Escape: "GfxFenceEscape(" before',
         mutate(sources, 'display.c', '    if (device->FullWddm && !WddmDiagnosticAllowed(',
                '    if (data->Command == BC250_ESCAPE_RUN_FENCE) GfxFenceEscape(device, NULL);\n'
                '    if (device->FullWddm && !WddmDiagnosticAllowed(')),
        ('a new caller of a writer', 'paths to SDMA0 writers',
         mutate(sources, 'gfx.c', 'NTSTATUS GfxSetPowerRetained(_Inout_ BC250_DEVICE* Device, BOOLEAN Resume)\n{\n',
                'NTSTATUS GfxSetPowerRetained(_Inout_ BC250_DEVICE* Device, BOOLEAN Resume)\n{\n'
                '    SdmaCopyEscape(Device, NULL);\n')),
        ('the paging claim after the lock', 'GfxSubmitPagingAccess: "Sdma0RingLock" before',
         mutate(sources, 'gfx.c', 'if (InterlockedCompareExchange(&gfx->PagingSubmitInFlight, 1, 0) != 0) return STATUS_DEVICE_BUSY;',
                'KeAcquireSpinLock(&gfx->Sdma0RingLock, &irql); KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);\n'
                '    if (InterlockedCompareExchange(&gfx->PagingSubmitInFlight, 1, 0) != 0) return STATUS_DEVICE_BUSY;')),
        ('a ring write without the lock', 'ring writes outside the writers and helpers',
         mutate(sources, 'gfx.c', 'static BOOLEAN GfxPagingSubmitReadyAccess(',
                'static int LabUnlockedWrite(struct amdgpu_ring* ring)\n{\n    return amdgpu_ring_alloc(ring, 16);\n}\n\n'
                'static BOOLEAN GfxPagingSubmitReadyAccess(')),
    ]
    assert escape in sources['display.c']
    bad = 0
    for title, expected, mutated in controls:
        found = scan(mutated)
        if any(expected in f for f in found):
            print('control %s: refused as required (%s)' % (title, '; '.join(f for f in found if expected in f)))
        else:
            print('FAIL control %s: not refused by "%s"; findings %s' % (title, expected, found))
            bad += 1
    findings = scan(sources)
    for f in findings:
        print('FAIL ' + f)
    print('SDMA0 writers: %d writers, %d paths, %d orders; %d findings, %d control failures'
          % (len(WRITERS), len(EDGES), len(ORDERS), len(findings), bad))
    return 1 if findings or bad else 0


if __name__ == '__main__':
    sys.exit(main())
