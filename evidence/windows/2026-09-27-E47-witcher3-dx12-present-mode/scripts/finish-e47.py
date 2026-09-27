import hashlib, json, pathlib, shutil

src = pathlib.Path('P:/BC-250/scratch/witcher3/dx12')
root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E47-witcher3-dx12-present-mode')
for d in ('scripts', 'patches', 'run008/state'):
    (root / d).mkdir(parents=True, exist_ok=True)
common = ['present.csv', 'present-summary.json', 'smoke-config.json', 'modules.json', 'result.json', 'launch.json',
          'vkd3d.log', 'witcher3_dxgi.log', 'before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt',
          'dxvk.conf', 'smoke/receipt.json', 'smoke/stdout.txt', 'smoke/stderr.txt']
for n in ('006', '007', '008'):
    run = src / f'run{n}'
    (root / f'run{n}' / 'smoke').mkdir(parents=True, exist_ok=True)
    for f in common + [f'done-w3dx12-{n}.json']:
        if (run / f).exists():
            shutil.copyfile(run / f, root / f'run{n}' / f)
    shutil.copyfile(run / f'run-w3dx12-{n}.utf8.log', root / f'run{n}' / f'run-w3dx12-{n}.log')
    shutil.copyfile(run / 'input-003.log', root / f'run{n}' / 'input-003-to-008.log')
    for f in [f'run-w3dx12-{n}.ps1', f'worker-w3dx12-{n}.ps1', f'launch-w3dx12-{n}.ps1', f'stop-{n}.ps1', f'status-{n}.ps1', f'pull-{n}.py']:
        shutil.copyfile(src / f, root / 'scripts' / f)
for f in sorted((src / 'run008' / 'state').glob('*.json')):
    shutil.copyfile(f, root / 'run008' / 'state' / f.name)
shutil.copyfile(src / '0001-gdi-immediate-2732f9c8.patch', root / 'patches' / '0001-gdi-immediate-2732f9c8.patch')
for f in ['make-w3dx12-mode.py', 'make-w3dx12-repeat.py', 'input-dx12-003.ps1', 'input-launch-dx12-003.ps1', 'precheck-006.ps1',
          'memstate.ps1', 'cpustate.ps1', 'pull-state-008.py', 'analyze-present-log.py', 'finish-e47.py']:
    shutil.copyfile(src / f, root / 'scripts' / f)
shutil.copyfile(pathlib.Path('P:/BC-250/scratch/icd-candidates/vulkan_radeon.CF3948D6.recipe.json'), root / 'scripts' / 'vulkan_radeon.CF3948D6.recipe.json')

caps = {}
for n in ('006', '007', '008'):
    for p in sorted((src / f'run{n}').glob('auto-*.png')):
        caps[f'run{n}/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
for p in sorted(src.glob('run00[678]-s*.png')):
    caps[f'{p.name} (half scale)'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
(root / 'capture-hashes.json').write_text(json.dumps(caps, indent=2) + '\n', encoding='utf-8')

facts = pathlib.Path('P:/BC-250/bc250-win/docs/facts.md')
text = facts.read_text(encoding='utf-8')
assert '| M607 |' not in text and '| M606 |' in text
link = '[E47](../evidence/windows/2026-09-27-E47-witcher3-dx12-present-mode/RESULT.md)'
row = ('| M607 | Witcher 3 DX12 present modes on the GDI present path, three steered runs of the E43 save within 21 min on the same '
       'boot (diagnostic KMD 153, CPU DWM 4596). Candidate ICD CF3948D6 (E43 tree plus IMMEDIATE on the GDI path, no DwmFlush wait '
       'in that mode) honours VKD3D_SWAPCHAIN_PRESENT_MODE end to end: FIFO run 006 header mode 2, DwmFlush 20.0 ms mean; IMMEDIATE '
       'run 007 header mode 0, DwmFlush 0, present call 25.5 ms against 41.4. The box is slower than in E43: run 008, an exact repeat '
       'of E43 run 005 with E43\'s ICD 6661C2D2, gives copy 19.5 ms (E43 3.3), DwmFlush 19.8 (12.1), scene interval 55.6 ms (32.7); '
       '4.5 GB physical available with the game closed, commit 5.2/40.7 GB, pools 353/300 MB, CPU 45-50 % (CPU DWM 3.5 cores), '
       'no paging. The 6x slower memcpy of the host-visible swapchain image is not explained by memory or CPU load; KMD 153 '
       'in-session transition or the DWM instance are the changed variables (HYPOTHESIS: uncached/write-combined CPU mapping). '
       'With DwmFlush removed the scene stays at about 54 ms per frame, so on this box the game\'s own frame time limits the rate. '
       'All presents VK_SUCCESS, smoke PASS on both ICDs, registered ICD restored to 93B1D1FD after each run. | MEASURED (unit A) | '
       + link + ' | 2026-09-27 |\n')
if not text.endswith('\n'):
    text += '\n'
facts.write_text(text + row, encoding='utf-8')

files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
artifacts = {
    'candidate ICD (006, 007)': 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157 (fork amdgpu-wddm/radv-wddm2-gdi-immediate 2732f9c8 on radv-wddm2-present-log 0f9811a5; build log scratch/mesa-fork-2026-09-27/build-radv-gdi-immediate.log; recipe in scripts/)',
    'control ICD (008)': '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD (E43)',
    'registered ICD before and after every run': '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D (E38)',
    'witcher3.exe (bin/x64_dx12)': '609BCA7033D02E9F34D446F97E739332CB785E18D51E08476AEB7F30091038D5, 4.0.0.103190',
    'vkd3d-proton d3d12.dll / d3d12core.dll': '7B77ED5C / 90B1DAD6 (472989aa, E37 package)',
    'DXVK dxgi.dll (app-local)': '2E674A56 (3.1.1)',
    'E14 smoke': 'C:/BC250/m13/fork-consolidated001/run-smoke.ps1 with vkcompute.exe, receipts in run00x/smoke',
    'lab': 'unit A, boot 2026-09-26T18:44:31+02:00, KMD 0.7.153.1 (generation 431166497000, M603), UMD 8279AC7F, CPU DWM 4596, clock 1000 MHz VID 116',
    'screenshots': 'run006-008 auto-000..0xx (full scale) and run006-s1, run007-s1 (half scale) private in scratch/witcher3/dx12; hashes in capture-hashes.json',
}
json.dump({'artifacts': artifacts, 'files': files}, open(root / 'manifest.json', 'w'), indent=1)
print('captures', len(caps), 'files', len(files), 'facts appended')
