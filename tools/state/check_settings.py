"""The settings the kernel driver reads against driver/state/catalog.json (the 'driver-state-settings' gate).

Every GuardReadSetting and GuardConsumeSetting call in driver/kmd names a root DWORD entry of the catalog, and a
literal default in the call equals the catalog's code default. A name outside the catalog reads as 0 at run time
(DriverStateNumber returns STATUS_INVALID_PARAMETER, guard.c maps it to 0), which is how BD-190 turned the boot
logo and the lab diagnostics off without a line in any log. Exit 1 on the first unknown name or default mismatch.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CALL = re.compile(r'\b(GuardReadSetting|GuardConsumeSetting)\(\s*L"([A-Za-z0-9]+)"\s*,\s*([^()]*?)\s*\)')
DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\d+)\s*[uUlL]*\s*(?://.*|/\*.*)?$', re.M)


def sweep(root):
    catalog = json.loads((root / 'driver/state/catalog.json').read_text(encoding='utf-8'))
    settings = {s['name']: s for s in catalog['values'] if s['path'] == ''}
    sources = sorted(p for p in (root / 'driver/kmd').glob('*.[ch]'))
    macros = {}
    for p in sources:
        for m in DEFINE.finditer(p.read_text(encoding='utf-8', errors='replace')):
            macros.setdefault(m.group(1), int(m.group(2)))
    failures, checked = [], 0
    for p in sources:
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in CALL.finditer(text):
            where = f"{p.relative_to(root).as_posix()}:{text.count(chr(10), 0, m.start()) + 1}"
            name, default = m.group(2), m.group(3).strip()
            spec = settings.get(name)
            if spec is None or spec['type'] != 'dword':
                failures.append(f'{where}: {name} is not a root DWORD entry of the catalog')
                continue
            checked += 1
            if re.fullmatch(r'\d+[uUlL]*', default):
                value = int(re.match(r'\d+', default).group(0))
            elif default in macros:
                value = macros[default]
            else:
                continue    # a computed default: the catalog code default still decides at run time
            if spec['kind'] == 'setting' and spec['code'] is not None and spec['code'] != value:
                failures.append(f'{where}: {name} default {value} in the call, {spec["code"]} in the catalog')
    return checked, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', default=str(ROOT), help='repository root (default: this checkout)')
    args = parser.parse_args()
    checked, failures = sweep(Path(args.root).resolve())
    for line in failures:
        print('FAIL ' + line)
    print(f'driver-state-settings: {checked} reads match the catalog, {len(failures)} failures')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
