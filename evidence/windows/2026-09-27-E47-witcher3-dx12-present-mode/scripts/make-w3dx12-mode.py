"""Derive a Witcher 3 DX12 present-mode run (006, 007, ...) from E43's run 005 scripts.

Usage: python make-w3dx12-mode.py <new3> <CANDHASH64> <fork-commit> <MODE> "<what>"

MODE is the VKD3D_SWAPCHAIN_PRESENT_MODE value (FIFO, IMMEDIATE, MAILBOX, FIFO_RELAXED). Everything else
(candidate swap with baseline copy and hash-checked restore, E14 smoke, package placement, present log, stop
file, screenshots, finally block) stays as in run 005. The derived scripts land next to this file.
"""
import re
import sys
from pathlib import Path

new, cand, commit, mode, what = sys.argv[1:6]
src = Path('P:/BC-250/scratch/witcher3/dx12')
old = '005'
old_cand = '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD'
assert len(cand) == 64 and cand.upper() == cand
short = cand[:8]

header = (
    f"# witcher3-dx12-{new}: candidate ICD {short} (fork amdgpu-wddm/radv-wddm2-gdi-immediate {commit} = 6661C2D2's\n"
    f"# present-log tree 0f9811a5 + IMMEDIATE present mode on the GDI path, no DwmFlush wait in that mode) swapped in\n"
    f"# place of the registered ICD for the duration of the run, E14 compute smoke as positive control first, then the\n"
    f"# E43 steered Witcher 3 DX12 run with VKD3D_SWAPCHAIN_PRESENT_MODE={mode} ({what}). The finally block restores the\n"
    f"# registered file to 93B1D1FD from the copy taken here and verifies the hash. No UMD/DWM/KMD change.\n"
)


def common(text):
    text = text.replace('w3dx12-' + old, 'w3dx12-' + new)
    text = text.replace('witcher3-dx12-' + old, 'witcher3-dx12-' + new)
    text = text.replace('stop-' + old, 'stop-' + new)
    text = text.replace('input-' + old, 'input-' + new)
    text = text.replace('run' + old, 'run' + new)
    text = text.replace('run ' + old, 'run ' + new)
    text = text.replace('vulkan_radeon.6661C2D2.dll', f'vulkan_radeon.{short}.dll')
    text = text.replace(old_cand, cand)
    return text


# run script
t = (src / f'run-w3dx12-{old}.ps1').read_text(encoding='utf-8')
lines = t.split('\n')
assert lines[0].startswith('# witcher3-dx12-005:') and lines[4].startswith('# 93B1D1FD')
t = header + '\n'.join(lines[5:])
t = common(t)
anchor = '  $env:BC250_WSI_PRESENT_LOG = "$out\\present.csv"\n'
assert anchor in t
t = t.replace(anchor, anchor + f"  $env:VKD3D_SWAPCHAIN_PRESENT_MODE = '{mode}'\n")
t = t.replace(f'"# run {new}: no frame cap"', f'"# run {new}: no frame cap, VKD3D_SWAPCHAIN_PRESENT_MODE={mode}"')
fin = '  if (Test-Path -LiteralPath "$out\\vkd3d.log") { "vkd3d_fence_export_errors='
assert fin in t
t = t.replace(fin, '  if (Test-Path -LiteralPath "$out\\present.csv") { Select-String -LiteralPath "$out\\present.csv" -Pattern \'^# chain\' | ForEach-Object { "present_chain: " + $_.Line } }\n'
                   '  if (Test-Path -LiteralPath "$out\\vkd3d.log") { Select-String -LiteralPath "$out\\vkd3d.log" -Pattern \'PRESENT_MODE|present mode\' | ForEach-Object { "vkd3d_mode: " + $_.Line } }\n' + fin)
(src / f'run-w3dx12-{new}.ps1').write_text(t, encoding='utf-8', newline='\n')

for name in ['launch-w3dx12-{}.ps1', 'worker-w3dx12-{}.ps1', 'stop-{}.ps1', 'status-{}.ps1', 'pull-{}.py']:
    t = (src / name.format(old)).read_text(encoding='utf-8')
    t = common(t)
    if name.startswith('launch'):
        t = t.replace('# Launch witcher3-dx12-' + new + ' (candidate ICD 6661C2D2 swap',
                      f'# Launch witcher3-dx12-{new} (candidate ICD {short} swap, VKD3D_SWAPCHAIN_PRESENT_MODE={mode}')
    (src / name.format(new)).write_text(t, encoding='utf-8', newline='\n')
    assert old_cand not in t and '6661C2D2' not in t, name

print('wrote run/launch/worker/stop/status/pull for', new, 'candidate', short, 'mode', mode)
