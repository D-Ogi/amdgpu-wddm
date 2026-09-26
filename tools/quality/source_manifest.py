"""Bind KMD packages to Git identity, exact source inputs and final artifact hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone

SCOPE = ['driver', 'third_party', 'tools/quality', 'tools/win/stackbudget.py']
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def capture(repo):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args]).decode('utf-8')
    names = set(git('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *SCOPE).split('\0')) - {''}
    sources = {name: digest(repo / name) for name in sorted(names) if (repo / name).is_file()}
    status = git('status', '--porcelain=v1', '-z', '--untracked-files=all', '--', *SCOPE)
    return {'commit': git('rev-parse', 'HEAD').strip(),
            'tree': git('rev-parse', 'HEAD^{tree}').strip(),
            'source_scope': SCOPE, 'source_clean': not bool(status),
            'repository_clean': not bool(git('status', '--porcelain=v1', '-z', '--untracked-files=all')),
            'source_sha256': sources}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--stage', choices=['begin', 'end'], required=True)
    parser.add_argument('--package', type=Path, action='append', default=[])
    parser.add_argument('--require-clean', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    now = capture(args.repo.resolve())
    if args.require_clean and not now['source_clean']:
        raise RuntimeError('Uncommitted driver/build inputs cannot qualify for deployment')
    start = args.out / 'source-manifest.start.json'
    if args.stage == 'begin':
        data = {'schema': 1, 'started_utc': datetime.now(timezone.utc).isoformat(), 'source': now}
        start.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
        print('source capture:', now['commit'], 'clean=' + str(now['source_clean']))
        return
    data = json.loads(start.read_text(encoding='utf-8'))
    before = data['source']
    if any(before[key] != now[key] for key in ['commit', 'source_sha256', 'source_clean']):
        raise RuntimeError('Source inputs or Git identity changed during compilation')
    data['finished_utc'] = datetime.now(timezone.utc).isoformat()
    data['status'] = 'BUILT'
    data['deployment_source_eligible'] = now['source_clean']
    commands = args.out / 'compile_commands.json'
    data['compile_commands_sha256'] = digest(commands) if commands.exists() else None
    data['packages'] = {path.name: {p.name: digest(p) for p in sorted(path.iterdir())
                                  if p.is_file() and p.name != 'source-manifest.json'} for path in args.package}
    payload = json.dumps(data, indent=2) + '\n'
    (args.out / 'source-manifest.json').write_text(payload, encoding='utf-8')
    for path in args.package:
        (path / 'source-manifest.json').write_text(payload, encoding='utf-8')
    print('source/artifact manifest written; source eligible=' + str(now['source_clean']))

if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
