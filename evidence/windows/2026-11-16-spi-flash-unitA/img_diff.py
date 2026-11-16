"""Compare a flash image with a reference image: hashes and the differing byte runs (offsets only)."""
import hashlib
import json
import sys
from pathlib import Path

a, b = Path(sys.argv[1]).read_bytes(), Path(sys.argv[2]).read_bytes()
runs, start = [], None
for i in range(min(len(a), len(b))):
    if a[i] != b[i]:
        if start is None:
            start = i
    elif start is not None:
        runs.append((start, i))
        start = None
if start is not None:
    runs.append((start, min(len(a), len(b))))
gap_merged = []
for s, e in runs:  # merge runs closer than 4 KiB into regions for the summary
    if gap_merged and s - gap_merged[-1][1] < 0x1000:
        gap_merged[-1][1] = e
    else:
        gap_merged.append([s, e])
print(json.dumps({
    'image': {'name': Path(sys.argv[1]).name, 'bytes': len(a), 'sha256': hashlib.sha256(a).hexdigest()},
    'reference': {'name': Path(sys.argv[2]).name, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()},
    'differing_bytes': sum(x != y for x, y in zip(a, b)),
    'differing_runs': len(runs),
    'regions_merged_4k': [[f'0x{s:06X}', f'0x{e:06X}'] for s, e in gap_merged],
    'last_differing_offset': f'0x{runs[-1][1] - 1:06X}' if runs else None,
}, indent=2))
