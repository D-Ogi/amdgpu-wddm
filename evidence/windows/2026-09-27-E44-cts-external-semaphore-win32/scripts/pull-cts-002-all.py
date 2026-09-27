import sys, re, io, tarfile, json, collections
from pathlib import Path
sys.path.insert(0, str(Path('P:/BC-250/bc250-win/tools/win').resolve()))
from target import Target

r = Path('P:/BC-250/scratch/witcher3/dx12/cts002')
t = Target()
done = t.ssh('cmd /c "tar -cf - -C C:\\BC250\\m12 cts-extsem-002"', timeout=300, binary=True)
if done.returncode or not done.stdout:
    raise SystemExit('tar failed: ' + done.stderr.decode('utf-8', 'replace'))
with tarfile.open(fileobj=io.BytesIO(done.stdout), mode='r') as tar:
    members = [m for m in tar.getmembers() if m.isfile()]
    for m in members:
        rel = m.name.replace('cts-extsem-002/', '', 1)
        if not rel.startswith(('baseline/', 'candidate/')):
            continue
        dst = r / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(tar.extractfile(m).read())
print('files', sum(1 for _ in r.rglob('*.*')))
cases = [l.strip() for l in Path('P:/BC-250/scratch/witcher3/dx12/cts-external-semaphore-win32.txt').read_text().splitlines() if l.strip()]
reasons = collections.defaultdict(collections.Counter)
for phase in ['baseline', 'candidate']:
    for i, case in enumerate(cases, 1):
        qpa = (r / phase / ('%03d.qpa' % i)).read_text(encoding='utf-8', errors='replace')
        m = re.findall(r'<Result StatusCode="([^"]+)">([^<]*)</Result>', qpa)
        key = m[0] if m else ('?', '?')
        reasons[phase][key] += 1
        if 'unsupported' in key[1].lower() or i in (1, 21, 23, 43):
            pass
    print(phase)
    for k, v in reasons[phase].most_common():
        print('  %3d  %s: %s' % (v, k[0], k[1][:200]))
# loader/ICD evidence from one err file
err = (r / 'candidate' / '003.err').read_text(encoding='utf-8', errors='replace')
print('err lines with ICD:', [l.strip()[:160] for l in err.splitlines() if 'vulkan_radeon' in l][:3])
out = (r / 'candidate' / '003.out').read_text(encoding='utf-8', errors='replace')
print('out tail:', out[-400:])
