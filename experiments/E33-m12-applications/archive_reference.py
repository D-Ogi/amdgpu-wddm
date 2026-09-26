"""Archive a pinned local Git tree and verify every archived blob for Linux use."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile


def prepare(source, revision, destination):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args])
    commit = git('rev-parse', '--verify', revision + '^{commit}').decode().strip()
    entries = {}
    for item in git('ls-tree', '-rz', '--full-tree', commit).split(b'\0'):
        if not item:
            continue
        meta, name = item.split(b'\t', 1)
        mode, kind, oid = meta.decode().split()
        if kind != 'blob':
            raise ValueError('Submodules require separately pinned archives')
        entries[name.decode()] = (mode, oid)
    destination.mkdir(parents=True, exist_ok=False)
    archive = destination / 'source.tar.gz'
    git('-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'archive',
        '--format=tar.gz', '--output=' + str(archive.resolve()), commit)
    seen = set()
    modes = {}
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            if member.isdir():
                continue
            if member.name in seen or member.name not in entries:
                raise ValueError('Unexpected/duplicate archive member: ' + member.name)
            mode, oid = entries[member.name]
            if member.issym() and mode == '120000':
                data = member.linkname.encode()
            elif member.isfile() and mode in {'100644', '100755'}:
                if bool(member.mode & 0o111) != (mode == '100755'):
                    raise ValueError('Executable mode changed: ' + member.name)
                data = tar.extractfile(member).read()
            else:
                raise ValueError('Archive type mismatch: ' + member.name)
            algorithm = 'sha1' if len(oid) == 40 else 'sha256'
            digest = hashlib.new(algorithm, b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if digest != oid:
                raise ValueError('Git blob differs: ' + member.name)
            seen.add(member.name)
            modes[mode] = modes.get(mode, 0) + 1
    if seen != set(entries):
        raise ValueError('Archive omitted tracked files (inspect export-ignore attributes)')
    report = {'commit': commit, 'archive': archive.name,
              'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'bytes': archive.stat().st_size, 'verified_blobs': len(seen),
              'git_modes': modes, 'verification': 'all archived blobs and executable/symlink modes match Git tree',
              'runtime_status': 'not built or executed'}
    (destination / 'source.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('repository', type=Path)
    p.add_argument('revision')
    p.add_argument('destination', type=Path)
    a = p.parse_args()
    print(json.dumps(prepare(a.repository, a.revision, a.destination), indent=2))
