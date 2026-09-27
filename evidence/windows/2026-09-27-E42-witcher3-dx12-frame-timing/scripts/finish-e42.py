import hashlib, json, pathlib, shutil

src = pathlib.Path('P:/BC-250/scratch/witcher3/dx12')
root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E42-witcher3-dx12-frame-timing')
(root / 'scripts').mkdir(parents=True, exist_ok=True)
caps = {}
for run in ('run003', 'run004'):
    r = src / run
    (root / run).mkdir(exist_ok=True)
    for n in ['modules.json', 'result.json', 'launch.json', 'vkd3d.log', 'witcher3_dxgi.log', 'before-health.txt',
              'after-health.txt', 'before-clock.txt', 'after-clock.txt', 'dxvk.conf', f'done-w3dx12-{run[-3:]}.json',
              'presentmon.stdout.txt', 'presentmon.stderr.txt', 'logman-session.txt']:
        if (r / n).exists():
            shutil.copyfile(r / n, root / run / n)
    shutil.copyfile(r / f'run-w3dx12-{run[-3:]}.utf8.log', root / run / f'run-w3dx12-{run[-3:]}.log')
    # PresentMon wrote nothing: record the size explicitly instead of an empty file.
    (root / run / 'presentmon.csv.size').write_text('0 bytes (see run log presentmon_csv_bytes=0)\n', encoding='utf-8')
    for p in sorted(r.glob('auto-*.png')):
        caps[f'{run}/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
    for p in sorted(src.glob(f'{run}-s*.png')):
        caps[f'{p.name} (half scale)'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
shutil.copyfile(src / 'run004' / 'input-003-and-004.log', root / 'run004' / 'input-003-and-004.log')
shutil.copyfile(src / 'run003' / 'input-003.log', root / 'run003' / 'input-003.log')
for n in ['run-w3dx12-003.ps1', 'worker-w3dx12-003.ps1', 'launch-w3dx12-003.ps1', 'stop-003.ps1', 'status-003.ps1',
          'run-w3dx12-004.ps1', 'launch-w3dx12-004.ps1', 'input-dx12-003.ps1', 'input-launch-dx12-003.ps1',
          'precheck-003.ps1', 'postcheck-e42.ps1', 'pull-003.py', 'analyze-presentmon.py', 'finish-e42.py']:
    shutil.copyfile(src / n, root / 'scripts' / n)
shutil.copyfile('P:/BC-250/scratch/presentmon/PROVENANCE.md', root / 'scripts' / 'presentmon-PROVENANCE.md')
(root / 'capture-hashes.json').write_text(json.dumps(caps, indent=2) + '\n', encoding='utf-8')

facts = pathlib.Path('P:/BC-250/bc250-win/docs/facts.md')
text = facts.read_text(encoding='utf-8')
assert '| M577 |' not in text
row = ('| M577 | PresentMon 2.6.0 records no present of the Witcher 3 DX12 build on the port: two steered runs (225 s and '
       '162 s, menu, save load and the Kaer Morhen scene rendering normally, ICD 93B1D1FD, vkd3d-proton 472989aa) gave a '
       '0-byte CSV both with display/GPU tracking and with present cadence only, while the ETW session (DXGI, DxgKrnl, '
       'Kernel-Process providers) was running and the game had loaded the system dxgi.dll and DComp. Source reason: the '
       'WDDM RADV winsys sets no get_d3d12_command_queue, so wsi_common_win32 takes the CPU-image path (per-frame CPU '
       'copy/swizzle into a DIB, GDI BitBlt, DwmFlush), which emits no DXGI or dxgkrnl present events. Frame timing of '
       'this path needs an in-stack instrument. Restored after each run, Tctl peak 79.9. '
       '| MEASURED (unit A) | [E42](../evidence/windows/2026-09-27-E42-witcher3-dx12-frame-timing/RESULT.md) '
       '| 2026-09-27 |\n')
if not text.endswith('\n'):
    text += '\n'
facts.write_text(text + row, encoding='utf-8')

files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
artifacts = {
    'witcher3.exe (bin/x64_dx12)': '609BCA7033D02E9F34D446F97E739332CB785E18D51E08476AEB7F30091038D5, 4.0.0.103190',
    'vkd3d-proton d3d12.dll / d3d12core.dll': '7B77ED5C / 90B1DAD6 (472989aa, E37 package)',
    'DXVK dxgi.dll (app-local)': '2E674A56 (3.1.1)',
    'system dxgi.dll / DComp.DLL loaded by the WSI': 'DDC578A2 / 285F0317',
    'registered ICD': '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D (E38)',
    'PresentMon': 'PresentMon-2.6.0-x64.exe sha256 b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af (v2.6.0, MIT)',
    'lab': 'unit A, boot 2026-09-26T18:44:31+02:00, KMD unchanged (generation 575721647), CPU DWM 4400, clock 1000 MHz VID 116',
    'screenshots': 'run003 auto-000..006 and run004 auto-000..004 (full scale), manual run003-s1..s3 and run004-s1..s2 (half scale) private in scratch/witcher3/dx12; hashes in capture-hashes.json',
    'WSI source': 'fork amdgpu-wddm/radv-wddm2-kmt-enum c34ab7cd, src/vulkan/wsi/wsi_common_win32.cpp (supports_dxgi needs win32.get_d3d12_command_queue; not set in src/amd/vulkan)',
}
json.dump({'artifacts': artifacts, 'files': files}, open(root / 'manifest.json', 'w'), indent=1)
print('captures', len(caps), 'files', len(files), 'fact appended')
