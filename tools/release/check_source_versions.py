"""Reject stale first-party PE resources before building or signing a package."""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_flavour import select_sources


def check(sources, root, repo, version, flavour):
    repo, root = Path(repo), Path(root)
    spec = importlib.util.spec_from_file_location('versionstamp', repo / 'tools/build/versionstamp.py')
    stamp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stamp)
    table = stamp.load_table(repo / 'tools/build/versioninfo.json')
    company = stamp.inf_provider(repo / 'driver/kmd/bc250kmd.inf')
    selected = select_sources(sources, flavour)
    errors, checked = [], 0
    for row in selected['files']:
        name = Path(row['path']).name
        if 'third_party' in row or Path(name).suffix.lower() not in ('.dll', '.exe', '.sys') or name in table['exempt']:
            continue
        checked += 1
        path = root / row['source']
        if not path.is_file():
            errors.append(f"{row['path']}: source missing")
            continue
        for error in stamp.check_file(path, version, company, table, file_name=name, flavour=flavour):
            errors.append(f"{row['path']}: {error}")
    if not checked:
        errors.append('No first-party PE resources were checked')
    return checked, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', required=True, type=Path)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--flavour', required=True, choices=('release', 'lab'))
    args = parser.parse_args()
    try:
        checked, errors = check(json.loads(args.sources.read_text(encoding='utf-8-sig')), args.root,
                                args.repo, args.version, args.flavour)
    except (OSError, ValueError, KeyError) as error:
        checked, errors = 0, [str(error)]
    for error in errors:
        print('FAIL', error)
    print(f'Source version gate: {args.flavour}, {checked} PE resources, {len(errors)} findings')
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
