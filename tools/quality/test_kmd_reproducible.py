"""Check the production KMD input ordering across both supported PowerShell hosts.

Only inert .c/.obj fixtures are created. No compiler, driver, or signing tools run.
--negative-control substitutes the historical Sort-Object implementation; the
same behavioral assertions must fail, rather than a parser or process error.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quote_ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--powershell51', default=shutil.which('powershell.exe'))
    parser.add_argument('--pwsh', default=shutil.which('pwsh.exe'))
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    helper = repo / 'tools/quality/kmd-build-context.ps1'
    build = repo / 'driver/kmd/build.ps1'
    if not helper.is_file() or not args.powershell51 or not args.pwsh:
        parser.error('production helper and PowerShell 5.1 / 7 are required')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixtures = out / 'fixtures'
    fixtures.mkdir(exist_ok=True)
    stems = ['bc250_sdma', 'bc250_sdma_copy', 'bc250_sdma_paging',
             'bc250_sdma_virtual_ptes', 'bc250_gfx', 'bc250_ring', 'driver',
             'wddm', 'a_b', 'a-b', 'A0']
    for extension in ('.c', '.obj'):
        for stem in reversed(stems):
            (fixtures / (stem + extension)).write_bytes(b'inert build-order fixture\n')
    script = out / 'order.ps1'
    control = ''
    if args.negative_control:
        control = '''
function Get-OrdinalBuildFiles([string]$Pattern) {
    return (Get-ChildItem -Path $Pattern -File | Sort-Object Name).FullName
}
'''
    script.write_text('''param([string]$Culture)
$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo($Culture)
. HELPER
CONTROL
$result = [ordered]@{ version=$PSVersionTable.PSVersion.ToString(); culture=$Culture }
foreach ($extension in @('c','obj')) {
    $paths = @(Get-OrdinalBuildFiles (Join-Path FIXTURES ('*.'+$extension)))
    $result[$extension] = @($paths | ForEach-Object { [IO.Path]::GetFileName($_) })
}
$result | ConvertTo-Json -Depth 4 -Compress
'''.replace('HELPER', quote_ps(helper)).replace('CONTROL', control)
                      .replace('FIXTURES', quote_ps(fixtures)), encoding='utf-8-sig')
    environment = os.environ.copy()
    environment['TMP'] = environment['TEMP'] = str(out)
    records = []
    errors = []
    for label, executable in [('powershell51', args.powershell51), ('pwsh', args.pwsh)]:
        for culture in ('pl-PL', 'en-US', 'tr-TR'):
            command = [executable, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                       '-File', str(script), '-Culture', culture]
            run = subprocess.run(command, capture_output=True, text=True,
                                 env=environment, timeout=30)
            log = out / (label + '-' + culture + '.txt')
            log.write_text(run.stdout + run.stderr, encoding='utf-8')
            if run.returncode:
                raise RuntimeError('host execution failed (not a CHECK): ' + str(log))
            result = json.loads(run.stdout.lstrip('\ufeff'))
            for extension in ('c', 'obj'):
                expected = sorted(stem + '.' + extension for stem in stems)
                if result[extension] != expected:
                    errors.append(label + '/' + culture + '/' + extension + ' is not ordinal')
            records.append({'host': label, 'result': result, 'command': command,
                            'host_sha256': digest(Path(executable))})
    build_text = build.read_text(encoding='utf-8-sig')
    for call in ("$sources = @(Get-OrdinalBuildFiles", 'Get-OrdinalBuildFiles "$obj\\*.obj"'):
        if call not in build_text:
            errors.append('production build is not wired to tested helper: ' + call)
    report = {'negative_control': args.negative_control,
              'helper_sha256': digest(helper), 'build_sha256': digest(build),
              'test_sha256': digest(Path(__file__)), 'runs': records, 'failures': errors}
    (out / 'record.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    for error in errors:
        print('FAIL CHECK ' + error)
    print(f'{len(records)} host/culture runs; {len(errors)} failures')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
