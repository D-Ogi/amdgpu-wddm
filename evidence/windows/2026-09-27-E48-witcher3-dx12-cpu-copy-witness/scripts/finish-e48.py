"""Assemble evidence E48 (cpucopy-001, run 009, cpucopy-002, run 010) and append facts row M610."""
import hashlib, json, pathlib, shutil, subprocess

src = pathlib.Path('P:/BC-250/scratch/witcher3/dx12')
cands = pathlib.Path('P:/BC-250/scratch/icd-candidates')
root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E48-witcher3-dx12-cpu-copy-witness')
for d in ('scripts', 'patches'):
    (root / d).mkdir(parents=True, exist_ok=True)


def text_copy(a, b):
    raw = a.read_bytes()
    t = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
    b.write_text(t, encoding='utf-8', newline='\n')


def copy(a, b):
    b.parent.mkdir(parents=True, exist_ok=True)
    if a.suffix in ('.txt', '.log') and a.read_bytes()[:2] in (b'\xff\xfe', b'\xfe\xff'):
        text_copy(a, b)
    else:
        shutil.copyfile(a, b)


def summarize(csv, out):
    subprocess.run(['python', str(src / 'analyze-present-log.py'), str(csv), '--json', str(out)], check=True, capture_output=True)


caps = {}
# game runs
common = ['present.csv', 'smoke-config.json', 'modules.json', 'result.json', 'launch.json', 'vkd3d.log', 'witcher3_dxgi.log',
          'before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt', 'dxvk.conf', 'input-003.log',
          'smoke/receipt.json', 'smoke/stdout.txt', 'smoke/stderr.txt', 'log-summary-after-010.txt']
for n in ('009', '010'):
    run = src / f'run{n}'
    for f in common + [f'done-w3dx12-{n}.json']:
        if (run / f).exists():
            copy(run / f, root / f'run{n}' / f)
    text_copy(run / f'run-w3dx12-{n}.log', root / f'run{n}' / f'run-w3dx12-{n}.log')
    summarize(run / 'present.csv', root / f'run{n}' / 'present-summary.json')
    if (src / f'pfsample-{n}.local.txt').exists():
        text_copy(src / f'pfsample-{n}.local.txt', root / f'run{n}' / f'pfsample-{n}.txt')
    for p in sorted(run.glob('auto-*.png')):
        caps[f'run{n}/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
    for f in [f'run-w3dx12-{n}.ps1', f'worker-w3dx12-{n}.ps1', f'launch-w3dx12-{n}.ps1', f'stop-{n}.ps1', f'status-{n}.ps1', f'pull-{n}.py']:
        if (src / f).exists():
            shutil.copyfile(src / f, root / 'scripts' / f)
# vkcube runs
for n in ('001', '002'):
    run = src / f'cpucopy-{n}'
    for p in sorted(run.iterdir()):
        if p.suffix == '.png':
            caps[f'cpucopy-{n}/{p.name}'] = hashlib.sha256(p.read_bytes()).hexdigest().upper()
        elif p.name.endswith('.utf8.log'):
            continue
        elif p.is_file():
            copy(p, root / f'cpucopy-{n}' / p.name)
    for c in sorted(run.glob('present-*.csv')):
        summarize(c, root / f'cpucopy-{n}' / (c.stem + '-summary.json'))
    for f in [f'run-cpucopy-{n}.ps1', f'launch-cpucopy-{n}.ps1', f'worker-cpucopy-{n}.ps1', f'status-cpucopy-{n}.ps1', f'pull-cpucopy-{n}.py']:
        if (src / f).exists():
            shutil.copyfile(src / f, root / 'scripts' / f)
for f in ['0002-host-cached-1dd68127.patch', '0003-memory-type-witness-238c495a.patch', '0004-copy-cycles-6358c3a9.patch']:
    shutil.copyfile(src / f, root / 'patches' / f)
for f in ['make-cpucopy.py', 'make-cpucopy-002.py', 'make-w3dx12-mode.py', 'pfsample.ps1', 'show-pfsample.py', 'pull-pfsample-010.py',
          'preserve-cpucopy-002-bad.ps1', 'unregister-010.ps1', 'logsummary-now.ps1', 'pull-logsummary-010.py', 'analyze-present-log.py',
          'patch-cycles.py', 'finish-e48.py', 'input-dx12-003.ps1', 'input-launch-dx12-003.ps1', 'precheck-006.ps1']:
    shutil.copyfile(src / f, root / 'scripts' / f)
for h in ('50E99A84', '09191AAA', '63AF86CB'):
    shutil.copyfile(cands / f'vulkan_radeon.{h}.recipe.json', root / 'scripts' / f'vulkan_radeon.{h}.recipe.json')
(root / 'capture-hashes.json').write_text(json.dumps(caps, indent=2) + '\n', encoding='utf-8')

facts = pathlib.Path('P:/BC-250/bc250-win/docs/facts.md')
text = facts.read_text(encoding='utf-8')
assert '| M610 |' not in text and '| M609 |' in text
link = '[E48](../evidence/windows/2026-09-27-E48-witcher3-dx12-cpu-copy-witness/RESULT.md)'
row = ('| M610 | The 6x slower CPU present copy of E47 (M607) is process-specific and not a mapping, memory, page-fault or scheduling '
       'effect. Same boot, same KMD 153, CPU DWM 4596, within one hour: vkcube on the CPU present path copies a 1920x1181 image in '
       '1.07 ms (8.5 GB/s) on the host-cached candidate 50E99A84, on E43\'s 6661C2D2 and on the witness 63AF86CB, and a 640x480 image '
       'in 0.14 ms (E46 level); the steered Witcher 3 DX12 scene copies 1920x1200 in 18.3 ms median (run 009, 3201 presents) and '
       '17.2 ms median (run 010, 2688 presents) with a floor of 16.9-17.9 ms and no value near vkcube\'s. The witness ICD shows the '
       'blit buffer in RADV memory type 5, properties 0xe (HOST_VISIBLE, COHERENT, CACHED) in both processes, and QueryThreadCycleTime '
       'around the copy gives 54.3 million cycles per game frame against 3.45 million in vkcube for the same byte count, CPU share '
       '1.00 in both (0.17 versus 2.6 bytes per cycle). The game\'s page faults are 0-22/s in the scene (side sampler). 50E99A84\'s '
       'HOST_CACHED request was a no-op because the port sets wsi->sw and upstream already selects a cached type; with sw the WSI '
       'waits for the image fence before the present, so copy time is memcpy time. Open: the physical placement (segment) and Lock2 '
       'cache attribute of the game\'s 9.2 MB blit buffer, not visible from the ICD (HYPOTHESIS: uncached BAR or non-aperture '
       'placement in the game process; 0.5 GB/s is the shape of an uncached read stream). Registered ICD 93B1D1FD restored after '
       'every stage, smoke PASS, no KMD change. | MEASURED (unit A) | ' + link + ' | 2026-09-27 |\n')
if not text.endswith('\n'):
    text += '\n'
facts.write_text(text + row, encoding='utf-8')

files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
artifacts = {
    'candidate ICD 50E99A84 (cpucopy-001 stage A, run 009)': '50E99A84C1FEA6295E6D54AB3E516498462E84FB3651A24A52FE5F2E942C151F (fork amdgpu-wddm/radv-wddm2-gdi-immediate 1dd68127; recipe in scripts/)',
    'control ICD 6661C2D2 (cpucopy-001 stage B)': '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD (E43)',
    'witness ICD 63AF86CB (cpucopy-002, run 010)': '63AF86CB862570709956E0376FDBDCB434DB3573E6A99412ED60311CFFAE48DA (fork 6358c3a9 = 1dd68127 + 238c495a + 6358c3a9; recipe in scripts/)',
    'intermediate ICD 09191AAA (built, never launched)': '09191AAADD4416CCF6C90343DE58152A9DE933B84817999A2868ABC9BB114614 (fork 238c495a; recipe in scripts/)',
    'registered ICD before and after every stage and run': '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D (E38)',
    'vkcube.exe': 'C:/BC250/m10/wsi-final/vkcube.exe, hash checked by the runner (see run-cpucopy-00x.ps1)',
    'witcher3.exe (bin/x64_dx12)': '609BCA7033D02E9F34D446F97E739332CB785E18D51E08476AEB7F30091038D5, 4.0.0.103190',
    'vkd3d-proton d3d12.dll / d3d12core.dll': '7B77ED5C / 90B1DAD6 (472989aa, E37 package)',
    'DXVK dxgi.dll (app-local)': '2E674A56 (3.1.1)',
    'E14 smoke': 'C:/BC250/m13/fork-consolidated001/run-smoke.ps1 with vkcompute.exe, receipts in run0xx/smoke',
    'lab': 'unit A, boot 2026-09-26T18:44:31+02:00, KMD 0.7.153.1 (generation 431166497000, M603), UMD 8279AC7F, CPU DWM 4596, clock 1000 MHz VID 116',
    'screenshots': 'run009/run010 auto-00x and cpucopy-00x stage screenshots (full scale) private in scratch/witcher3/dx12; hashes in capture-hashes.json',
    'cpucopy-002 first pass': 'malformed size list, two vkcube launches without a present log, preserved on the target as C:/BC250/m12/cpucopy-002-bad-sizes; not part of the results',
}
json.dump({'artifacts': artifacts, 'files': files}, open(root / 'manifest.json', 'w'), indent=1)
print('captures', len(caps), 'files', len(files), 'facts appended')
