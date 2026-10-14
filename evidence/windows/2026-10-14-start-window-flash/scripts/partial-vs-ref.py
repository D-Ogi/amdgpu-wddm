"""Is the partial picture the full text's top left 100x23 pixels on navy? Compare each run's c-surface-0 with its
c-ref: the pixels inside (0..99, 0..22) against the reference, the pixels outside against navy (0, 0, 128).

    python partial-vs-ref.py <run dir>...
"""
import sys
from pathlib import Path

from PIL import Image

NAVY = (0, 0, 128)
for d in map(Path, sys.argv[1:]):
    ref_file = next(f for f in sorted(d.iterdir()) if f.stem.endswith("-ref") and f.suffix in (".png", ".bmp"))
    case = ref_file.stem[:-len("-ref")]
    ref = Image.open(ref_file).convert("RGB")
    part = Image.open(d / (case + "-surface-0" + ref_file.suffix)).convert("RGB")
    w, h = ref.size
    r, p = ref.load(), part.load()
    inside = [(x, y) for y in range(h) for x in range(w) if x < 100 and y < 23]
    outside = [(x, y) for y in range(h) for x in range(w) if not (x < 100 and y < 23)]
    differ_in = sum(1 for x, y in inside if p[x, y] != r[x, y])
    not_navy = sum(1 for x, y in outside if p[x, y] != NAVY)
    ref_text_out = sum(1 for x, y in outside if r[x, y] != NAVY)
    print(f"{d.name}: inside 100x23 {len(inside)} px, {differ_in} differ from the reference; outside {len(outside)} px, "
          f"{not_navy} not navy (the reference has {ref_text_out} text pixels there)")
