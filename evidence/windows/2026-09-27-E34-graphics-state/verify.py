from pathlib import Path
import json
import re

root = Path(__file__).resolve().parent
expected = ['depth-near', 'depth-reject-far', 'stencil-replace',
            'stencil-reject', 'stencil-accept', 'alpha-blend',
            'shader-branch', 'render-to-texture']

def read(path):
    data = path.read_bytes()
    return data.decode('utf-16' if data.startswith(b'\xff\xfe') else 'utf-8-sig')

results = {}
for run in ['080', '081', '082']:
    suffix = run if run == '082' else ''
    folder = root / run
    stdout = read(folder / f'stdout{suffix}.txt')
    cases = re.findall(r'case=(\S+) mismatches=(\d+)/4096 tolerance=(\d) fnv64=(\w+)', stdout)
    assert [case[0] for case in cases] == expected
    assert all(case[1] == '0' for case in cases)
    assert [case[2] for case in cases] == ['0'] * 5 + ['1', '0', '0']
    assert 'branch DXBC conditional opcode verified' in stdout
    assert 'PASS native graphics state: 8 cases, 32768 checked pixels' in stdout
    done = json.loads(read(folder / f'done{suffix}.json'))
    assert done['exit'] == 0 and done['dwm_before'] == done['dwm_after'] == [84]
    assert not re.search(r'device lost|VK_ERROR|failed', read(folder / f'stderr{suffix}.txt'), re.I)
    results[run] = cases
assert results['080'] == results['082']
assert all(a == b for i, (a, b) in enumerate(zip(results['080'], results['081'])) if i != 5)
print('PASS: three runs, eight cases each; GPU hashes match WARP; unchanged DWM')
