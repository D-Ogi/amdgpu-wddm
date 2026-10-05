import sys, json, hashlib
from pathlib import Path
sys.path.insert(0, str(Path('P:/BC-250/bc250-win/tools/win').resolve()))
from target import Target
from PIL import Image
from collections import Counter

r = Path('P:/BC-250/scratch/fl-probe-2026-09-27/kmtflip001')
r.mkdir(exist_ok=True)
t = Target()
names = ['run-kmtflip001.log', 'done-kmtflip001.json', 'stdout-kmtflip001.txt', 'stderr-kmtflip001.txt',
         'run-kmtflip001.ps1', 'worker-kmtflip001.ps1', 'launch-kmtflip001.ps1'] + [f'screen-kmtflip001-{i}.png' for i in range(3)]
for n in names:
    t.pull('C:\\BC250\\m13\\kmt-flip001\\' + n, str(r / n))
result = []
hashes = {}
for i in range(3):
    p = r / f'screen-kmtflip001-{i}.png'
    hashes[p.name] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
    im = Image.open(p).convert('RGB')
    roi = im.crop((180, 200, 440, 380))
    counts = Counter(roi.getdata())
    result.append(dict(capture=i, size=im.size, roi=[180, 200, 440, 380], pixels=roi.width * roi.height,
                       colors=counts.most_common(5)))
(r / 'screen-kmtflip001-analysis.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
(r / 'capture-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
