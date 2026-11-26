"""Extract the 64 KiB page table code of vidmm.c for a CPU-only test of its index arithmetic."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
# Negative control: the encoder as it would be if a 64 KiB entry took one slot, as every entry did before.
parser.add_argument('--mutation', choices=['none', 'one-slot'], default='none')
args = parser.parse_args()
functions = [
    'ULONG VidMmPageTableEntrySlots(',
    'static BOOLEAN VidMmRefuse64K(',
    'static int VidMmEncodeEntry(',
    'static void VidMmNotePublished(',
    'BOOLEAN VidMmEncodePageTable(',
    'static BOOLEAN VidMmTable64KMarked(',
]
source = (args.source / 'vidmm.c').read_text()
parts = []
for name in functions:
    start = 0
    while True:
        start = source.index(name, start)
        brace = source.index('{', start)
        if source.find(';', start, brace) < 0:
            break
        start += len(name)
    end = source.index('\n}', brace) + 2
    body = source[start:end]
    if name == 'BOOLEAN VidMmEncodePageTable(' and args.mutation == 'one-slot':
        old = '*Physical = table + ((ULONGLONG)Update->StartIndex + Start) * slots * sizeof(ULONGLONG);'
        if body.count(old) != 1:
            raise SystemExit('the mutation point is not in VidMmEncodePageTable')
        body = body.replace(old, '*Physical = table + ((ULONGLONG)Update->StartIndex + Start) * sizeof(ULONGLONG);')
    parts.append(body)
fixture = Path(__file__).with_name('vidmm_64k_test.c').read_text()
args.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', '\n'.join(parts)))
print('Extracted the 64 KiB page table slot count, refusal, entry encoder, publication note and GPU encoder of vidmm.c')
