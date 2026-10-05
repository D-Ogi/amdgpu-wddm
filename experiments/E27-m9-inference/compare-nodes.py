"""Compare E27 diagnostic CPU/GPU F32 node destinations; report errors, not acceptance.
PROVENANCE: independent diagnostic using Python standard library.
"""
import collections
import json
import math
import pathlib
import re
import struct
import sys
root = pathlib.Path(sys.argv[1])
def nodes(side):
    counts = collections.Counter()
    result = {}
    for idx, name, op in re.findall(r"^NODE (\d+) (\S+) (\S+)$", (root / (side + ".out")).read_text().split("STOP_ON_HARDWARE_FAILURE")[0], re.M):
        key = (name, op, counts[(name, op)])
        counts[(name, op)] += 1
        path = root / side / (f"{int(idx):03d}-dst.f32")
        if path.exists():
            data = path.read_bytes()
            result[key] = (int(idx), struct.unpack("<" + "f" * (len(data) // 4), data))
    return result
cpu, gpu = nodes("cpu"), nodes("gpu")
rows = []
for key, (idx, actual) in gpu.items():
    if key not in cpu:
        rows.append({"node": key, "gpu_index": idx, "comparison": "missing CPU node"})
        continue
    ci, expected = cpu[key]
    if len(actual) != len(expected):
        rows.append({"node": key, "gpu_index": idx, "comparison": "shape mismatch"})
        continue
    finite = all(math.isfinite(v) for v in actual + expected)
    err = [abs(a-b) for a,b in zip(actual,expected)]
    rows.append({"node": key, "cpu_index": ci, "gpu_index": idx, "elements": len(actual),
                 "finite": finite, "max_abs_error": max(err) if finite else None,
                 "relative_l2_error": math.sqrt(sum(e*e for e in err) / max(1e-30,sum(v*v for v in expected))) if finite else None})
print(json.dumps(rows, indent=2, allow_nan=False))
