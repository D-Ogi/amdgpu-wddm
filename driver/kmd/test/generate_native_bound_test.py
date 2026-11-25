"""Extract WddmNativeAllocation of wddm.c for native_bound_test.c: the bound of a native virtual fill or transfer."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
# Negative control: the bound as the first 64 KiB build had it, the size the user-mode driver asked for.
parser.add_argument('--mutation', choices=['none', 'blob-size'], default='none')
args = parser.parse_args()
source = (args.source / 'wddm.c').read_text(encoding='utf-8').replace('\r\n', '\n')
name = 'static BOOLEAN WddmNativeAllocation('
if source.count(name) != 1:
    raise SystemExit('WddmNativeAllocation must be defined once in wddm.c')
start = source.index(name)
body = source[start:source.index('\n}\n', start) + 3]
if args.mutation == 'blob-size':
    old = 'allocation->UmdAlloc ? allocation->UmdVidMmBytes : allocation->Allocation.Size'
    if body.count(old) != 1:
        raise SystemExit('the mutation point is not in WddmNativeAllocation')
    body = body.replace(old, 'allocation->UmdAlloc ? allocation->UmdBytes : allocation->Allocation.Size')
fixture = Path(__file__).with_name('native_bound_test.c').read_text(encoding='utf-8')
if fixture.count('/* ACTUAL_SOURCE */') != 1:
    raise SystemExit('the fixture must hold ACTUAL_SOURCE once')
args.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', body), encoding='utf-8')
print('Extracted WddmNativeAllocation of wddm.c')
