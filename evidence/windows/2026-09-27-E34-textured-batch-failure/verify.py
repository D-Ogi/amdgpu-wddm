"""Check the recorded negative control without treating a crash as a pass."""
from pathlib import Path
import json
import re

p = Path(__file__).resolve().parent
v = json.loads((p / 'verification.json').read_text(encoding='utf-8'))
assert v['classification'] == 'FAIL'
for run in ['086', '087', '088', '089']:
    b = (p / run / 'stdout.txt').read_bytes()
    s = b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig')
    old = re.findall(r'case=(\S+) mismatches=(\d+)/4096', s)
    assert len(old) == 8 and all(x[1] == '0' for x in old)
    images = re.findall(r'textured=(\S+) pass=(\d) mismatches=(\d+)/4096 fnv64=(\w+)', s)
    assert [list(x) for x in images] == v['runs'][run]['textured_images']
    if run in ['086', '087']:
        assert len(images) == 8
        for form in ['t0-s0', 't1-s0']:
            assert [(x[1], x[2], x[3]) for x in images if x[0] == form] == [
                (str(i), '0', '4da4ffd8a1ace325' if i % 2 == 0 else '45946e4d9c66a325')
                for i in range(4)]
        assert v['runs'][run]['exit'] == 0
    else:
        assert not images and v['runs'][run]['exit'] == -1073740791
assert v['dump']['subcode'] == 7
assert v['dump']['assertion'] == 'count != 1'
print('Evidence checks pass; hosted textured control remains FAIL.')
