"""Select one exact payload before copying, signing or checking provenance."""
import argparse
import copy
import json
from pathlib import Path, PurePosixPath

FLAVOURS = ('release', 'lab')


def select_sources(sources: dict, flavour: str = 'release') -> dict:
    if flavour not in FLAVOURS:
        raise ValueError('unknown package flavour')
    declared = sources.get('flavour')
    if declared is not None and declared != flavour:
        raise ValueError('source selection belongs to a different flavour')
    rows = sources['files']
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ValueError('invalid source rows')
    tagged = sources.get('schema', 0) >= 4 or any('flavour' in row for row in rows)
    if not tagged:
        if flavour != 'release':
            raise ValueError('legacy source pins do not contain a lab build')
        selected = rows
    else:
        selected = []
        keys = set()
        for row in rows:
            tag, path = row.get('flavour'), row.get('path')
            if tag not in (*FLAVOURS, 'shared') or not isinstance(path, str):
                raise ValueError('every source row needs a release, lab or shared flavour')
            if (tag, path.casefold()) in keys:
                raise ValueError('duplicate flavour/source slot')
            keys.add((tag, path.casefold()))
            if tag == 'shared' and PurePosixPath(path).suffix.lower() in ('.sys', '.dll', '.exe', '.config'):
                raise ValueError('PE images and loader configs need flavour-specific pins')
            if tag in (flavour, 'shared'):
                selected.append(row)
        # A missing lab row must not silently remove a component. The same slot
        # exists in both flavours, even when a third-party file has equal bytes.
        if {r['path'].casefold() for r in selected} != {r['path'].casefold() for r in rows}:
            raise ValueError(f'incomplete {flavour} source set')
    paths = [r['path'].casefold() for r in selected]
    if len(paths) != len(set(paths)):
        raise ValueError('duplicate selected source slot')
    result = copy.deepcopy(sources)
    result['files'] = copy.deepcopy(selected)
    result['flavour'] = flavour
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', required=True, type=Path)
    parser.add_argument('--flavour', choices=FLAVOURS, default='release')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = select_sources(json.loads(args.sources.read_text(encoding='utf-8-sig')), args.flavour)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
