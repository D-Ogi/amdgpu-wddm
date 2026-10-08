"""Offline gates of the generic KMD transition tool, on the development PC only (no lab access).

    python check-offline.py            py_compile, PowerShell 5.1 parse, literal gates and every host test
    python check-offline.py --quick    the same without the tests that launch bounded-child.exe

The bounded-child tests start short-lived hidden powershell.exe children inside a Job that the helper kills (at
most a few seconds); nothing resident, nothing with a visible window. Two historical tests are not run here:
test-select-driver-args.ps1 drives the device installer (select-driver.exe, byte-identical to the one the 171
promotion validated), and test-receipt-pipe.ps1 needs a fixture binary the 171 stage did not preserve.
Outputs go to host-tests/<UTC stamp>/ under this directory, never to C:.
"""
import argparse
import json
import py_compile
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmdcommon import BASE, HIST, OPS, POWERSHELL, TEMPLATE, HOST_TESTS, digest, historical_manifest, ps_env  # noqa: E402

PURE = ['test-identity', 'test-hang-detector', 'test-child-closure', 'test-cpu-snapshot', 'test-deployment-acceptance',
        'test-health-order', 'test-health-wait', 'test-install-observation', 'test-logged-transition',
        'test-package-cleanup', 'test-pnp-idle', 'test-readiness-health', 'test-readiness', 'test-registered-package',
        'test-setup-log', 'test-verify-cpu', 'test-parameters']
WITH_OUT = ['test-registration', 'test-release', 'test-stage', 'test-transition-policy']
WITH_TOOL = ['test-arm-helper', 'test-supervisor']
# The kit runs on the installed release and needs nothing from the old lab directories C:\BC250\m8 to m14, which go
# away (owner, 2026-10-08). The KMD client is the release's, resolved through InstallRoot: a kit file that names a
# client by path has a copy of its own, which can answer about another driver than the one on the GPU.
LEGACY_DIRECTORY = re.compile(r'(?i)C:\\BC250\\m(?:8|9|1[0-4])\\')
CLIENT_BY_PATH = re.compile(r'(?i)[a-z]:\\[^\s"\']*bc250kmd_cli\.exe')
SCANNED = ('.py', '.ps1', '.md', '.json', '.cpp', '.c')
# This file states both rules; every other file of the kit must satisfy them.
SCAN_EXEMPT = ('check-offline.py',)


def legacy_paths():
    """Every kit file that names an old lab directory or a KMD client by path, as 'name:line rule' rows."""
    hits = []
    for path in sorted(BASE.rglob('*')):
        if not path.is_file() or path.suffix.lower() not in SCANNED or path.name in SCAN_EXEMPT:
            continue
        if '__pycache__' in path.parts:
            continue
        rel = path.relative_to(BASE).as_posix()
        for number, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            if LEGACY_DIRECTORY.search(line):
                hits.append(f'{rel}:{number} old lab directory')
            if CLIENT_BY_PATH.search(line):
                hits.append(f'{rel}:{number} KMD client by path')
    return hits


def ps(script, *args, timeout=120):
    return subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), *map(str, args)],
                          capture_output=True, text=True, timeout=timeout, env=ps_env())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = HOST_TESTS / stamp
    out.mkdir(parents=True, exist_ok=False)
    results = []

    def record(name, ok, detail):
        results.append({'check': name, 'pass': ok, 'detail': detail.strip()[-600:]})
        print(('PASS ' if ok else 'FAIL ') + name + (': ' + detail.strip().splitlines()[-1] if detail.strip() else ''))

    for py in sorted(BASE.glob('*.py')):
        try:
            py_compile.compile(str(py), cfile=str(out / (py.stem + '.pyc')), doraise=True)
            record('py_compile ' + py.name, True, '')
        except py_compile.PyCompileError as error:
            record('py_compile ' + py.name, False, str(error))
    r = ps(OPS / 'parse-check.ps1', '-Root', f'{TEMPLATE};{OPS}')
    record('parse-check', r.returncode == 0, r.stdout + r.stderr)
    hits = legacy_paths()
    record('legacy lab paths', not hits, '\n'.join(hits[:10]) if hits else 'no kit file names C:\\BC250\\m8..m14 '
           'or a KMD client by path')

    transition = TEMPLATE / 'kmd-transition'
    kmd168 = TEMPLATE / 'kmd168-transition'
    # The kmd168 helpers in the template are the 171 stage's own files, byte for byte.
    files = historical_manifest()
    for path in sorted(kmd168.iterdir()):
        rel = 'kmd168-transition/' + path.name
        record('reused ' + rel, files.get(rel) == digest(path), 'hash ' + digest(path)[:8])
    for name in PURE:
        r = ps(transition / (name + '.ps1'))
        record(name, r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    for name in WITH_OUT:
        r = ps(transition / (name + '.ps1'), '-Out', out / name)
        record(name, r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    r = ps(BASE / 'tools/test-identity-revisions.ps1', '-Out', out / 'identity-revisions')
    record('identity revisions', r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    # The desktop pins freeze derives from lab-baseline.json (CPU desktop, or the router desktop's CPU route).
    r = subprocess.run([sys.executable, str(BASE / 'tools/test_desktop_pins.py')], capture_output=True, text=True, timeout=120)
    record('desktop pins', r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    # The package identity (R, B) from DriverVer 0.7.R.B, the release rule, labels and the freeze refusals.
    r = subprocess.run([sys.executable, str(BASE / 'tools/test_versions.py')], capture_output=True, text=True, timeout=300)
    record('versions', r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    r = ps(kmd168 / 'test-deadline.ps1')
    record('kmd168 test-deadline', r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    if not args.quick:
        tool = HIST / 'bounded-child.exe'
        if digest(tool) != files['bounded-child.exe']:
            raise SystemExit('bounded-child.exe differs from the 171 stage')
        for name in WITH_TOOL:
            r = ps(transition / (name + '.ps1'), '-Tool', tool, '-Out', out / name)
            record(name, r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
        r = ps(kmd168 / 'test-bounded-child.ps1', '-Exe', tool, '-Out', out / 'test-bounded-child')
        record('kmd168 test-bounded-child', r.returncode == 0 and 'PASS' in r.stdout, r.stdout + r.stderr)
    failed = [x['check'] for x in results if not x['pass']]
    summary = {'utc': stamp, 'quick': args.quick, 'checks': len(results), 'failed': failed, 'results': results}
    (out / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    print(f'{len(results) - len(failed)}/{len(results)} passed; summary {out / "summary.json"}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
