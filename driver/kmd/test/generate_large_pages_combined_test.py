"""Extract the production text that the 2 MiB VRAM layout and the 64 KiB host pages share, for one host test of both.

From vidmm.c: the state, the start, the two encoders' common path, the publication, the table marks, the walk and the
summary. From wddm.c and vram.c: the memory layout, the choice of the large page layout, and the lines of
WddmCreateAdmit that give a UMD allocation its size and alignment. From bc250kmd.h and wddm.c: the constants those
texts name. Nothing is restated: a constant or a function that is not found once is an error, not a pass.
"""
from pathlib import Path
import argparse
import re

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
# Negative control: the 64 KiB sizes give an allocation the 64 KiB alignment whatever it had, so an allocation that
# the 2 MiB layout had placed on 2 MiB loses that placement. The test must then fail.
parser.add_argument('--mutation', choices=['none', 'sizes-drop-2mib'], default='none')
args = parser.parse_args()


def read(name):
    return (args.source / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def defines(text, names, where):
    lines = []
    for name in names:
        found = re.findall(r'^#define ' + re.escape(name) + r'\b.*$', text, flags=re.M)
        if len(found) != 1:
            raise SystemExit(f'{name} must be defined once in {where}')
        lines.append(found[0])
    return '\n'.join(lines)


def function(text, name, where):
    start = 0
    while True:
        start = text.find(name, start)
        if start < 0:
            raise SystemExit(f'{name} is not defined in {where}')
        brace = text.index('{', start)
        if text.find(';', start, brace) < 0:
            break
        start += len(name)
    return text[start:text.index('\n}', brace) + 2]


def between(text, first, last, where):
    if text.count(first) != 1 or text.count(last) != 1:
        raise SystemExit(f'the text from "{first[:40]}" to "{last[:40]}" must be in {where} once')
    start = text.index(first)
    end = text.index(last, start) + len(last)
    return text[start:end]


header = read('bc250kmd.h')
wddm = read('wddm.c')
vidmm = read('vidmm.c')
vram = read('vram.c')

constants = '\n'.join([
    defines(header, ['BC250_VRAM_POOL_BELOW', 'BC250_VRAM_TOP_RESERVED', 'BC250_VIDMM_HUGE_PAGE_BYTES',
                     'BC250_VIDMM_GIANT_PAGE_BYTES', 'BC250_LARGE_PAGE_LAYOUT_NONE', 'BC250_LARGE_PAGE_LAYOUT_GIANT',
                     'BC250_LARGE_PAGE_LAYOUT_HUGE', 'BC250_VIDMM_PAGE_64K_BYTES', 'BC250_VIDMM_PAGE_64K_SLOTS'],
            'bc250kmd.h'),
    defines(wddm, ['BC250_WDDM_SEGMENT_VRAM', 'BC250_WDDM_SEGMENT_TABLES', 'BC250_WDDM_SEGMENT_APERTURE'], 'wddm.c'),
])
# The state of vidmm.c as one text: its constants, the C_ASSERTs that tie them to bc250_pte.h, the struct, g_VidMm.
state = between(vidmm, '#define BC250_VIDMM_SHADOW_TAG', 'static BC250_VIDMM g_VidMm;', 'vidmm.c')
vidmm_functions = [
    'static NTSTATUS VidMmLargePageContext(',
    'NTSTATUS VidMmStartLayout(',
    'void VidMmStop(',
    'static void VidMmCountEncoding(',
    'static void VidMmNoteRefusal(',
    'ULONG VidMmPageTableEntrySlots(',
    'static BOOLEAN VidMmRefuse64K(',
    'static int VidMmEncodeEntry(',
    'static void VidMmNotePublished(',
    'BOOLEAN VidMmEncodePageTable(',
    'NTSTATUS VidMmCommitPagingUpdate(',
    'static BOOLEAN VidMmTable64KMarked(',
    'NTSTATUS VidMmCommitPagingCopy(',
    'NTSTATUS VidMmCommitPagingFill(',
    'static BOOLEAN VidMmLocalPageAllowed(',
    'static BOOLEAN VidMmLargePage(',
    'BOOLEAN VidMmRootPhysical(',
    'static BOOLEAN VidMmTranslateViewLocked(',
    'void VidMmSummary(',
]
parts = [constants, state]
parts += [function(vidmm, name, 'vidmm.c') for name in vidmm_functions]
parts += [function(vram, name, 'vram.c') for name in ['BOOLEAN VramFramebufferOffset(', 'BOOLEAN VramFramebufferExtent(']]
parts += [function(wddm, name, 'wddm.c') for name in ['static BOOLEAN WddmMemoryLayout(',
                                                       'static void WddmSettleLargePageLayout(']]

# WddmCreateAdmit, a UMD allocation: from the segment of its heap to the end of the 64 KiB sizes, one text, so that
# the order of the 2 MiB placement and the 64 KiB sizes is the order of wddm.c.
admit = between(wddm, 'segment = (view.heap == UMD_BLOB_HEAP_GTT) ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;',
                'object->UmdVidMmBytes = info->Size;\n        }\n', 'wddm.c')
for needed in ['info->Size = (SIZE_T)ROUND_TO_PAGES((SIZE_T)view.bytes);',
               'device->LargePageLayout != BC250_LARGE_PAGE_LAYOUT_NONE && segment == BC250_WDDM_SEGMENT_VRAM',
               'if (device->SystemSizes64K && info->Size >= BC250_VIDMM_PAGE_64K_BYTES']:
    if admit.count(needed) != 1:
        raise SystemExit(f'WddmCreateAdmit: "{needed[:50]}" is not in the extracted lines once')
if admit.index('device->LargePageLayout != BC250_LARGE_PAGE_LAYOUT_NONE') > admit.index('device->SystemSizes64K'):
    raise SystemExit('WddmCreateAdmit: the 2 MiB placement no longer comes before the 64 KiB sizes')
if args.mutation == 'sizes-drop-2mib':
    old = 'info->Alignment = (UINT)alignment;'
    if admit.count(old) != 1:
        raise SystemExit('the mutation point is not in WddmCreateAdmit')
    admit = admit.replace(old, 'info->Alignment = (UINT)PAGING_HOST_PAGE_64K;')

fixture = Path(__file__).with_name('large_pages_combined_test.c').read_text(encoding='utf-8')
for marker in ['/* ACTUAL_SOURCE */', '/* CREATE_ADMIT_SOURCE */']:
    if fixture.count(marker) != 1:
        raise SystemExit(f'the fixture must hold {marker} once')
fixture = fixture.replace('/* ACTUAL_SOURCE */', '\n'.join(parts)).replace('/* CREATE_ADMIT_SOURCE */', admit)
args.out.write_text(fixture, encoding='utf-8')
print('Extracted the state, start, encoders, publication, table marks, walk and summary of vidmm.c, the memory layout '
      'and the large page layout choice of wddm.c, and the size and alignment lines of WddmCreateAdmit')
