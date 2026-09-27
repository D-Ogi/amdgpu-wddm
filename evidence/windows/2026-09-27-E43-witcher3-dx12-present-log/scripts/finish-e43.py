import hashlib, json, pathlib, shutil

src = pathlib.Path('P:/BC-250/scratch/witcher3/dx12')
run = src / 'run005'
root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E43-witcher3-dx12-present-log')
for d in ('run005', 'run005/smoke', 'scripts', 'patches'):
    (root / d).mkdir(parents=True, exist_ok=True)
for n in ['present.csv', 'present-summary.json', 'smoke-config.json', 'modules.json', 'result.json', 'launch.json',
          'vkd3d.log', 'witcher3_dxgi.log', 'before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt',
          'dxvk.conf', 'done-w3dx12-005.json', 'postcheck.txt', 'smoke/receipt.json', 'smoke/stdout.txt', 'smoke/stderr.txt']:
    if (run / n).exists():
        shutil.copyfile(run / n, root / 'run005' / n)
shutil.copyfile(run / 'run-w3dx12-005.utf8.log', root / 'run005' / 'run-w3dx12-005.log')
shutil.copyfile(run / 'input-003.log', root / 'run005' / 'input-003-to-005.log')
for p in sorted((run / 'patches').glob('*.patch')):
    shutil.copyfile(p, root / 'patches' / p.name)
for n in ['run-w3dx12-005.ps1', 'worker-w3dx12-005.ps1', 'launch-w3dx12-005.ps1', 'stop-005.ps1', 'status-005.ps1',
          'input-dx12-003.ps1', 'input-launch-dx12-003.ps1', 'postcheck-e42.ps1', 'pull-005.py', 'analyze-present-log.py',
          'finish-e43.py']:
    shutil.copyfile(src / n, root / 'scripts' / n)
caps = {}
for p in sorted(run.glob('auto-*.png')):
    caps[f'run005/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
for p in sorted(src.glob('run005-s*.png')):
    caps[f'{p.name} (half scale)'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
(root / 'capture-hashes.json').write_text(json.dumps(caps, indent=2) + '\n', encoding='utf-8')

facts = pathlib.Path('P:/BC-250/bc250-win/docs/facts.md')
text = facts.read_text(encoding='utf-8')
assert '| M580 |' not in text and '| M581 |' not in text
link = '[E43](../evidence/windows/2026-09-27-E43-witcher3-dx12-present-log/RESULT.md)'
rows = [
    ('| M580 | Per-present timing of the Witcher 3 DX12 build on the port (candidate ICD 6661C2D2 = baseline plus the WSI '
     'present log, vkd3d-proton 472989aa, 1920x1200, GDI present path): 9280 presents over 266 s; menu about 55-60 presents/s '
     '(interval median 16.7 ms), loaded Kaer Morhen scene locked at 29.7 presents/s (median 33.4 ms, p5 32.3, p95 34.5); '
     'the present call itself takes a median 15.8 ms of the frame on the presenting thread: CPU copy of the image into the '
     'DIB 3.2 ms, BitBlt 1.3 ms, DwmFlush 11.3 ms (p95 24.5). All presents VK_SUCCESS. E14 smoke PASS on the candidate, '
     'registered ICD restored to 93B1D1FD after the run. What locks the scene at two refresh periods (game vsync, FIFO '
     'present mode or DwmFlush after a miss) is not established. | MEASURED (unit A) | ' + link + ' | 2026-09-27 |\n'),
    ('| M581 | Creating the exportable monitored fence with Shared/NtSecuritySharing derived from VK_SYNC_IS_SHAREABLE '
     '(fork 59f53088; the runtime sets VK_SYNC_IS_SHARED only after the first export, so the previous flags left the object '
     'non-shareable and D3DKMTShareObjects failed unchecked) removes vkd3d-proton\'s d3d12_shared_fence_open_export_kmt '
     'VK_ERROR_UNKNOWN (vr -13) failures: zero in the E43 run against four in every earlier run on 93B1D1FD (E40, E41, E42), '
     'same game, same package, rendering unchanged. The Win32 external-semaphore CTS cases have not been run yet. '
     '| MEASURED (unit A) | ' + link + ' | 2026-09-27 |\n'),
]
if not text.endswith('\n'):
    text += '\n'
facts.write_text(text + ''.join(rows), encoding='utf-8')

files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
artifacts = {
    'candidate ICD': '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD (fork amdgpu-wddm/radv-wddm2-present-log 0f9811a5 on radv-wddm2-fence-share 59f53088 on c34ab7cd; build log scratch/mesa-fork-2026-09-27/build-radv-presentlog.log)',
    'registered ICD before and after': '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D (E38)',
    'witcher3.exe (bin/x64_dx12)': '609BCA7033D02E9F34D446F97E739332CB785E18D51E08476AEB7F30091038D5, 4.0.0.103190',
    'vkd3d-proton d3d12.dll / d3d12core.dll': '7B77ED5C / 90B1DAD6 (472989aa, E37 package)',
    'DXVK dxgi.dll (app-local)': '2E674A56 (3.1.1)',
    'E14 smoke': 'C:/BC250/m13/fork-consolidated001/run-smoke.ps1 with vkcompute.exe, receipt in run005/smoke',
    'lab': 'unit A, boot 2026-09-26T18:44:31+02:00, KMD unchanged (generation 575721647), UMD 8279AC7F, CPU DWM 4400, clock 1000 MHz VID 116',
    'screenshots': 'run005 auto-000..008 (full scale) and run005-s1 (half scale) private in scratch/witcher3/dx12; hashes in capture-hashes.json',
}
json.dump({'artifacts': artifacts, 'files': files}, open(root / 'manifest.json', 'w'), indent=1)
print('captures', len(caps), 'files', len(files), 'facts appended')
