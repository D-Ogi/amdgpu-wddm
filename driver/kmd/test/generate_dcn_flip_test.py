"""Compile the actual DcnFlipWriteSequence body against an MMIO latch model."""
from pathlib import Path
import argparse

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'dcn.c')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
s = a.source.read_text()
# From the plane format composer (M15.14), which the write sequence's callers use, to the end of the sequence.
start = s.index('static BOOLEAN DcnPlaneFormatRegisters(')
end = s.index("// M87's poll:", start)
body = s[start:end]
start = s.index('void DcnFlipEscape(')
body += s[start:s.index('\n}\n', start)+3]
fixture = Path(__file__).with_name('dcn_flip_test.c').read_text()
assert fixture.count('/* ACTUAL_SOURCE */') == 1
assert body.count('static NTSTATUS DcnFlipWriteSequence(') == 1
a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', body))
print('Extracted the plane format composer and DcnFlipWriteSequence from', a.source)
