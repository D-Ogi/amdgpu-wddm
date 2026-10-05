"""Compare the entire logged client rectangle; no inferred corner masks."""
import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
args = parser.parse_args()
directory = args.directory
text = (directory / "stdout.txt").read_text(encoding="utf-8-sig")
match = re.search(r"capture_ready rgb=0,0,255 x=(\d+) y=(\d+) width=(\d+) height=(\d+)", text)
if not match:
    raise SystemExit("Missing final blue readiness")
x, y, width, height = map(int, match.groups())
if (width, height) != (640, 480):
    raise SystemExit("Unexpected client extent")
result = {"client": [x, y, width, height], "captures": {}}
for name in ("primary.bmp", "screen.png"):
    path = directory / name
    with Image.open(path) as source:
        image = source.convert("RGB")
    if x + width > image.width or y + height > image.height:
        raise SystemExit("Client outside capture")
    crop = image.crop((x, y, x + width, y + height))
    counts = Counter(crop.getdata())
    blue = counts[(0, 0, 255)]
    result["captures"][name] = {
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "image_size": list(image.size), "pixels": width * height,
        "blue": blue, "mismatches": width * height - blue,
        "client_rgb_sha256": hashlib.sha256(crop.tobytes()).hexdigest(),
    }
result["pass"] = all(c["mismatches"] == 0 for c in result["captures"].values())
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass"] else 1)
