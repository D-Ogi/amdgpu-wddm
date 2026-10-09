"""Verify actual selected C warning flags and exercise the compiler's ABI gate.
Run in the same MSVC environment as the build. No GPU or lab access.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-commands', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--match', default=r'/src/amd/vulkan/')
    args = ap.parse_args()
    # utf-8-sig, not utf-8: Windows PowerShell 5.1 writes a byte-order mark for `Set-Content -Encoding utf8` and
    # PowerShell 7 does not, so the build host decided whether this gate could read its own compile_commands.json.
    entries = json.loads(args.compile_commands.read_text(encoding='utf-8-sig'))
    selected = [e for e in entries if re.search(args.match, e['file'].replace('\\', '/')) and e['file'].endswith('.c')]
    if not selected:
        raise SystemExit('FAIL: no selected C compilation commands')
    flag_sets = set()
    for entry in selected:
        # Meson quotes each MSVC argument; only warning options are needed here.
        command = entry.get('command') or subprocess.list2cmdline(entry['arguments'])
        flags = tuple(re.findall(r'(?i)(?<!\w)/(?:w[0-4]|wx-?|we\d+|wd\d+|w[1-4]\d+)\b', command))
        for number in ('4013', '4020', '4024'):
            relevant = [f.lower() for f in flags if f.lower() in ('/we'+number, '/wd'+number)]
            if not relevant or relevant[-1] != '/we'+number:
                raise SystemExit('FAIL: C'+number+' not fatal in '+entry['file'])
        flag_sets.add(flags)
    args.out.mkdir(parents=True, exist_ok=True)
    cases = {
        'valid': ('int declared(int a, int b); int main(void) { return declared(1, 2); }', None),
        'missing': ('int main(void) { return undeclared(1, 2); }', 'C4013'),
        'arity': ('int declared(int a, int b); int main(void) { return declared(1); }', 'C2198'),
    }
    for group, flags in enumerate(sorted(flag_sets)):
        for name, (source, diagnostic) in cases.items():
            path = args.out / (str(group)+'-'+name+'.c')
            path.write_text(source+'\n', encoding='utf-8')
            proc = subprocess.run(['cl', '/nologo', '/c', *flags, str(path), '/Fo'+str(path.with_suffix('.obj'))], capture_output=True)
            output = proc.stdout + proc.stderr
            path.with_suffix('.log').write_bytes(output)
            if diagnostic is None:
                ok = proc.returncode == 0
            else:
                ok = proc.returncode != 0 and diagnostic.encode() in output
            if not ok:
                raise SystemExit('FAIL: compiler control '+name+'; see '+str(path.with_suffix('.log')))
    print('PASS:', len(selected), 'selected C commands guarded;', len(flag_sets), 'warning configurations verified by valid/missing/arity controls')


if __name__ == '__main__':
    main()
