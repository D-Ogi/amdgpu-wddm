"""Exact repeat of E43's run 005 (candidate 6661C2D2, no VKD3D present-mode override) under a new run number.

Usage: python make-w3dx12-repeat.py <new3> "<why>"
"""
import sys
from pathlib import Path

new, why = sys.argv[1:3]
src = Path('P:/BC-250/scratch/witcher3/dx12')
old = '005'


def common(text):
    for a, b in [('w3dx12-' + old, 'w3dx12-' + new), ('witcher3-dx12-' + old, 'witcher3-dx12-' + new),
                 ('stop-' + old, 'stop-' + new), ('input-' + old, 'input-' + new), ('run' + old, 'run' + new),
                 ('run ' + old, 'run ' + new)]:
        text = text.replace(a, b)
    return text


t = (src / f'run-w3dx12-{old}.ps1').read_text(encoding='utf-8')
lines = t.split('\n')
assert lines[0].startswith('# witcher3-dx12-005:')
lines[0] = f'# witcher3-dx12-{new}: exact repeat of E43 run 005 ({why}); candidate ICD 6661C2D2 (fork amdgpu-wddm/radv-wddm2-present-log 0f9811a5 = baseline'
t = common('\n'.join(lines))
fin = '  if (Test-Path -LiteralPath "$out\\vkd3d.log") { "vkd3d_fence_export_errors='
assert fin in t
t = t.replace(fin, '  if (Test-Path -LiteralPath "$out\\present.csv") { Select-String -LiteralPath "$out\\present.csv" -Pattern \'^# chain\' | ForEach-Object { "present_chain: " + $_.Line } }\n' + fin)
(src / f'run-w3dx12-{new}.ps1').write_text(t, encoding='utf-8', newline='\n')
for name in ['launch-w3dx12-{}.ps1', 'worker-w3dx12-{}.ps1', 'stop-{}.ps1', 'status-{}.ps1', 'pull-{}.py']:
    (src / name.format(new)).write_text(common((src / name.format(old)).read_text(encoding='utf-8')), encoding='utf-8', newline='\n')
print('wrote', new)
