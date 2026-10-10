"""Extract production VRAM geometry/capability code for CPU-only contract tests."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--caps-mutation', choices=['none', 'noop', 'frozen'], default='none')
args = parser.parse_args()
parts = []
functions = [
    ('vram.c', 'NTSTATUS VramStart('),
    ('vram.c', 'BOOLEAN VramFramebufferOffset('),
    ('wddm.c', 'static BOOLEAN WddmMemoryLayout('),
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
    parts.append(body)
fixture = Path(__file__).with_name('vram_geometry_test.c').read_text()
args.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', '\n'.join(parts)))
print('Extracted production VRAM start, framebuffer, layout, GART, aperture and capability patch')
