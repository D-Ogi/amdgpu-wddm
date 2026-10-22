"""Reject known executable-memory requests in production KMD C sources.

BD-218 regression guard, not an HVCI compatibility proof. Scan every conditional
branch, excluding driver/kmd/test. Comments and string/character literals do not
count. This lexical check cannot resolve numeric protections, computed values,
token-pasting macros, external libraries, or runtime mappings. These still need
compiled-artifact inspection and Code Integrity Verifier/HVCI acceptance.

Contracts: WDK wdm.h and MicrosoftDocs/windows-driver-docs,
driversecurity/implement-hvci-compatible-code.md. Legacy contiguous allocators
return executable memory; use MmAllocateContiguousNodeMemory with NX protection.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re


FORBIDDEN = frozenset({
    "MmAllocateContiguousMemory", "MmAllocateContiguousMemorySpecifyCache",
    "MmAllocateContiguousMemorySpecifyCacheNode", "NonPagedPool",
    "NonPagedPoolExecute", "NonPagedPoolCacheAligned",
    "POOL_FLAG_NON_PAGED_EXECUTE", "PAGE_EXECUTE", "PAGE_EXECUTE_READ",
    "PAGE_EXECUTE_READWRITE", "PAGE_EXECUTE_WRITECOPY",
})
LEX = re.compile(
    r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
    r'[A-Za-z_][A-Za-z_0-9]*', re.S)


def findings(source):
    # Translation phase 2 precedes comment removal. Retain physical line numbers
    # even when an identifier or // comment continues across a spliced newline.
    parts, locations, line, pos = [], [], 1, 0
    for match in re.finditer(r'\\\r?\n', source):
        chunk = source[pos:match.start()]
        parts.append(chunk)
        for c in chunk:
            locations.append(line)
            line += c == '\n'
        line += 1
        pos = match.end()
    chunk = source[pos:]
    parts.append(chunk)
    for c in chunk:
        locations.append(line)
        line += c == '\n'
    text = ''.join(parts)
    return [{'line': locations[m.start()], 'token': m[0]}
            for m in LEX.finditer(text) if m[0] in FORBIDDEN]


def check(root):
    base = root / 'driver/kmd'
    errors, files = [], []
    for required in ('gart.c', 'gpumem.c'):
        if not (base / required).is_file():
            errors.append(f'missing required production source: driver/kmd/{required}')
    def traversal_error(error):
        raise error

    try:
        # pathlib glob can suppress directory access errors. Incomplete scans
        # must fail, including an unreadable directory that has no known anchor.
        for directory, dirs, names in os.walk(base, onerror=traversal_error):
            dirs.sort()
            if Path(directory) == base:
                dirs[:] = [d for d in dirs if d.lower() != 'test']
            for filename in sorted(names):
                path = Path(directory) / filename
                if path.suffix.lower() not in ('.c', '.h'):
                    continue
                raw = path.read_bytes()
                name = path.relative_to(root).as_posix()
                files.append({'path': name, 'sha256': hashlib.sha256(raw).hexdigest()})
                for hit in findings(raw.decode('utf-8-sig')):
                    errors.append(f"{name}:{hit['line']}: executable-memory token {hit['token']}")
    except (OSError, UnicodeError) as error:
        errors.append(f'incomplete source scan: {error}')
    return {'status': 'FAIL' if errors else 'PASS', 'errors': errors, 'files': files,
            'scope': 'production driver/kmd C and H lexical regression check only',
            'hvci_acceptance': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = check(args.root)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"kmd-nx: {result['status']}, {len(result['files'])} source files")
    for error in result['errors']:
        print(error)
    return int(bool(result['errors']))


if __name__ == '__main__':
    raise SystemExit(main())
