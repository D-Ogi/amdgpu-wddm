import hashlib, json, pathlib

root = pathlib.Path('P:/BC-250/bc250-win/evidence/windows/2026-09-27-E48-witcher3-dx12-cpu-copy-witness')
m = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
files = {}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name != 'manifest.json':
        files[p.relative_to(root).as_posix()] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
changed = [k for k in files if k not in m['files'] or m['files'][k] != files[k]]
m['files'] = files
json.dump(m, open(root / 'manifest.json', 'w'), indent=1)
print('rehashed', len(files), 'changed', changed)
