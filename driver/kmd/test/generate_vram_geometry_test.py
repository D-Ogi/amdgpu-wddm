"""Extract production VRAM geometry/capability code for CPU-only contract tests."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--caps-mutation', choices=['none', 'noop', 'frozen'], default='none')
# BD-341 negative control: the top-of-VRAM check as it was before, with FramebufferLength in place of the range of
# VramFramebufferExtent. FramebufferLength is 0 on a start without a firmware display, so the cold cases must fail.
parser.add_argument('--range-mutation', choices=['none', 'framebuffer-length'], default='none')
# Negative controls of the 2 MiB layout (LargePagesSmallCarveout). huge-no-round: the base of that layout is rounded
# to 64 KiB only, so it is not a 2 MiB address. huge-both-levels: the translation context of that layout keeps
# level 2. Both must fail the test. A mutation whose source text is gone is an error, not a pass.
parser.add_argument('--layout-mutation', choices=['none', 'huge-no-round', 'huge-both-levels'], default='none')
args = parser.parse_args()
parts = []
functions = [
    ('vram.c', 'NTSTATUS VramStart('),
    ('vram.c', 'BOOLEAN VramFramebufferOffset('),
    ('vram.c', 'BOOLEAN VramFramebufferExtent('),
    ('vram.c', 'NTSTATUS VramFirmwareRangeBelowTop('),
    ('wddm.c', 'static BOOLEAN WddmMemoryLayout('),
    ('wddm.c', 'static void WddmSettleLargePageLayout('),
    ('vidmm.c', 'static NTSTATUS VidMmLargePageContext('),
    ('gart.c', 'static int RunSetup('),
    ('paging_window.c', 'int PagingApertureBytesValid('),
    ('wddm.c', 'static void VramPatchCaps('),
]
for filename, name in functions:
    source = (args.source / filename).read_text()
    start = 0
    while True:
        start = source.index(name, start)
        brace = source.index('{', start)
        if source.find(';', start, brace) < 0:
            break
        start += len(name)
    end = source.index('\n}', brace) + 2
    body = source[start:end]
    if name == 'static void VramPatchCaps(' and args.caps_mutation != 'none':
        replacement = '{ (void)Device; (void)Caps; (void)Bytes; }'
        if args.caps_mutation == 'frozen':
            replacement = '''{
    ULONGLONG value=8589934592ull;
    (void)Device;
    if (!Caps || Bytes<UMD_CAPS_BYTES) return;
    RtlCopyMemory((PUCHAR)Caps+UMD_CAPS_VRAM_TOTAL_OFFSET,&value,8);
    RtlCopyMemory((PUCHAR)Caps+UMD_CAPS_VISIBLE_VRAM_TOTAL_OFFSET,&value,8);
}'''
        body = body[:body.index('{')] + replacement
    if name == 'NTSTATUS VramFirmwareRangeBelowTop(' and args.range_mutation != 'none':
        replacement = '''{
    ULONGLONG fbOffset;
    if (!VramFramebufferOffset(Device, &fbOffset)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (fbOffset + Device->FramebufferLength > Device->VramLength - TopBytes) return STATUS_CONFLICTING_ADDRESSES;
    return STATUS_SUCCESS;
}'''
        body = body[:body.index('{')] + replacement
    if name == 'static BOOLEAN WddmMemoryLayout(' and args.layout_mutation == 'huge-no-round':
        original = 'align=BC250_VIDMM_HUGE_PAGE_BYTES;'
        if body.count(original) != 1:
            raise SystemExit('layout mutation huge-no-round: the source line is not there exactly once')
        body = body.replace(original, 'align=65536ull;')
    if name == 'static NTSTATUS VidMmLargePageContext(' and args.layout_mutation == 'huge-both-levels':
        original = '(Device->LargePageLayout == BC250_LARGE_PAGE_LAYOUT_HUGE) ? 1u : 0u'
        if body.count(original) != 1:
            raise SystemExit('layout mutation huge-both-levels: the source line is not there exactly once')
        body = body.replace(original, '0u')
    parts.append(body)
fixture = Path(__file__).with_name('vram_geometry_test.c').read_text()
args.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', '\n'.join(parts)))
print('Extracted production VRAM start, framebuffer, top-of-VRAM check, layout, large-page layout choice and context, '
      'GART, aperture and capability patch')
