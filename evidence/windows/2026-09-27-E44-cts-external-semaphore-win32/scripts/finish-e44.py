import hashlib, json, pathlib, shutil

src = pathlib.Path('P:/BC-250/scratch/witcher3/dx12')
run = src / 'cts002'
root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E44-cts-external-semaphore-win32')
for d in ('cts002', 'cts002/baseline', 'cts002/candidate', 'cts001', 'scripts'):
    (root / d).mkdir(parents=True, exist_ok=True)
for n in ['cases.jsonl', 'baseline-summary.json', 'candidate-summary.json', 'before-health.txt', 'after-health.txt',
          'done-cts-extsem-002.json']:
    shutil.copyfile(run / n, root / 'cts002' / n)
shutil.copyfile(run / 'run-cts-extsem-002.utf8.log', root / 'cts002' / 'run-cts-extsem-002.log')
for phase in ('baseline', 'candidate'):
    for p in sorted((run / phase).glob('*.*')):
        shutil.copyfile(p, root / 'cts002' / phase / p.name)
# run 001: the rejected-option control (every case exit -1), keep the one error log that shows the reason
c1 = src / 'cts001'
for p in sorted(c1.glob('*')):
    if p.is_file():
        shutil.copyfile(p, root / 'cts001' / p.name)
for n in ['run-cts-extsem-001.ps1', 'run-cts-extsem-002.ps1', 'worker-cts-extsem-002.ps1', 'launch-cts-extsem-002.ps1',
          'poll-cts-extsem-002.ps1', 'cts-external-semaphore-win32.txt', 'pull-cts-002.py', 'pull-cts-002-all.py',
          'finish-e44.py']:
    shutil.copyfile(src / n, root / 'scripts' / n)

facts = pathlib.Path('P:/BC-250/bc250-win/docs/facts.md')
text = facts.read_text(encoding='utf-8')
assert '| M583 |' not in text
link = '[E44](../evidence/windows/2026-09-27-E44-cts-external-semaphore-win32/RESULT.md)'
row = ('| M583 | The 44 CTS cases dEQP-VK.api.external.semaphore.opaque_win32.* and .opaque_win32_kmt.* (cts-release-tools '
       'deqp-vk AE7BEFDD, one process per case, loader debug log kept) give the same result on the registered ICD 93B1D1FD '
       'and on the fence-share candidate 6661C2D2: 4 Pass (the info_binary and info_timeline queries), 40 NotSupported '
       '("Semaphore doesn\'t support exporting in external type"), 0 Fail, 0 Crash, 0 timeout; every process loaded '
       'wsi-final/vulkan_radeon.dll. Cause of the NotSupported, read from the info logs and the runtime source: the '
       'exportable and importable bits are advertised only for timeline semaphores with OPAQUE_WIN32 or D3D12_FENCE '
       '(vk_wddm2_monitored_fence_type exports NT handles), binary semaphores use the vk_sync_binary wrapper, which has '
       'no import or export, and OPAQUE_WIN32_KMT is never advertised by vk_semaphore.c. The 40 functional cases are '
       'binary-semaphore tests, so the CTS exercises the fence-share change only through the info query; its runtime '
       'witness stays M581 (vkd3d-proton D3D12_FENCE export). Run 001 of the same list was void: this deqp-vk build '
       'rejects --deqp-watchdog-total-time-limit and --deqp-watchdog-interval-time-limit (exit -1 on every case). '
       '| MEASURED (unit A) | ' + link + ' | 2026-09-27 |\n')
if not text.endswith('\n'):
    text += '\n'
facts.write_text(text + row, encoding='utf-8')

files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
artifacts = {
    'registered ICD before, restored after': '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D (E38)',
    'candidate ICD (phase 2)': '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD (fork amdgpu-wddm/radv-wddm2-present-log 0f9811a5 on radv-wddm2-fence-share 59f53088 on c34ab7cd)',
    'deqp-vk': 'C:/BC250/m12/cts-release-tools/deqp-vk.exe AE7BEFDD (hash checked by the run script)',
    'case list': 'scripts/cts-external-semaphore-win32.txt, 44 cases from the full CTS case list (no d3d12_fence semaphore group exists in it)',
    'lab': 'unit A, boot 2026-09-26T18:44:31+02:00, KMD unchanged (generation 575721647, completed 27480 -> 27521), UMD 8279AC7F, CPU DWM 4400, Tctl 68.2 -> 68.4 C',
    'window': 'Codex-granted lab window (agent coordination), run 02:14:57-02:15:23 UTC as interactive task BC250-M12-cts-extsem-002',
}
json.dump({'artifacts': artifacts, 'files': files}, open(root / 'manifest.json', 'w'), indent=1)
print('files', len(files), 'facts appended')
