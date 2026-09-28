"""Assemble only. No network or lab mutation. Requires committed runner sources."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--rollback', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    dirty = subprocess.check_output(['git', '-C', str(REPO), 'status', '--porcelain', '--', str(HERE), str(HERE.parent / 'kmd168-transition')], text=True)
    if dirty.strip():
        raise SystemExit('Commit runner changes before staging')
    revision = subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    pins = json.loads((HERE / 'package-hashes.json').read_text())
    packages = {'candidate169': args.candidate, 'rollback166': args.rollback}
    for label, source in packages.items():
        for name, expected in pins[label].items():
            if digest(source / name) != expected:
                raise SystemExit(f'Package mismatch: {label}/{name}')
    if args.out.exists():
        raise SystemExit('Use a fresh stage directory')
    args.out.mkdir(parents=True)
    for label, source in packages.items():
        destination = args.out / label
        destination.mkdir()
        for name in pins[label]:
            shutil.copyfile(source / name, destination / name)
    for source in (HERE, HERE.parent / 'kmd168-transition'):
        destination = args.out / source.name
        destination.mkdir()
        for item in source.glob('*.ps1'):
            shutil.copyfile(item, destination / item.name)
    shutil.copyfile(HERE / 'package-hashes.json', args.out / 'package-hashes.json')
    shutil.copyfile(args.helper, args.out / 'bounded-child.exe')
    files = {f.relative_to(args.out).as_posix(): digest(f) for f in sorted(args.out.rglob('*')) if f.is_file()}
    manifest = {'schema': 1, 'runner_revision': revision, 'files': files}
    path = args.out / 'stage-manifest.json'
    path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'manifest_sha256': digest(path), 'file_count': len(files), 'runner_revision': revision}))

if __name__ == '__main__':
    main()
