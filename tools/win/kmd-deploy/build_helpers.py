"""Build the deployment helpers from this kit's sources, never from an older attempt.

All output is under the requested fresh directory. The receipt binds the source
revision, source bytes, compiler, linker, SDK paths, commands and output bytes.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from kmdcommon import BASE, REPO, ROOT, TEMPLATE, digest

SOURCES = {
    'bounded-child': 'template/kmd168-transition/bounded-child.cpp',
    'select-driver': 'template/kmd-transition/select-driver.cpp',
    'receipt-pipe-fixture': 'template/kmd168-transition/receipt-pipe-fixture.cpp',
}


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {command}\n{result.stdout}\n{result.stderr}')
    return result.stdout.strip()


def inputs():
    paths = [BASE / path for path in SOURCES.values()] + [BASE / 'build_helpers.py', BASE / 'kmdcommon.py']
    paths += sorted((TEMPLATE / 'kmd168-transition').glob('*.ps1'))
    paths += [TEMPLATE / 'kmd-transition/build-select-driver.ps1']
    return {p.relative_to(BASE).as_posix(): digest(p) for p in paths}


def build(out):
    out = Path(out).resolve()
    if not out.is_relative_to((ROOT / 'scratch').resolve()):
        raise ValueError('Helper output must stay under workspace scratch')
    out.mkdir(parents=True, exist_ok=False)
    clang = ROOT / 'toolchain/llvm-23.1.4-win64/bin/clang-cl.exe'
    linker = clang.with_name('lld-link.exe')
    version = run([clang, '--version'])
    print(version, flush=True)
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = Path(run([vswhere, '-latest', '-products', '*', '-property', 'installationPath']))
    msvc = sorted((vs / 'VC/Tools/MSVC').iterdir(), key=lambda p: tuple(map(int, p.name.split('.'))))[-1]
    sdk = ROOT / 'toolchain/nuget/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0'
    libs = ROOT / 'toolchain/nuget/microsoft.windows.sdk.cpp.x64/c'
    try:
        repo = Path(run(['git', '-C', BASE, 'rev-parse', '--show-toplevel']))
        source_base = BASE
    except RuntimeError:
        # An operator copy is allowed only when all build inputs match the repository copy.
        repo = Path(os.environ.get('BC250_KMD_DEPLOY_SOURCE_REPO', str(REPO))).resolve()
        source_base = repo / 'tools/win/kmd-deploy'
    source_commit = run(['git', '-C', repo, 'rev-parse', 'HEAD'])
    source_hashes = inputs()
    for rel, sha in source_hashes.items():
        run(['git', '-C', repo, 'ls-files', '--error-unmatch', (source_base / rel).relative_to(repo)])
        if digest(source_base / rel) != sha:
            raise RuntimeError('Operator copy differs from repository source: ' + rel)
    env = dict(os.environ, TEMP=str(out), TMP=str(out), CL='', _CL_='', LINK='')
    commands = {}
    for stem, rel in SOURCES.items():
        cmd = [clang, '/nologo', '/EHsc', '/std:c++17', '/W4', '/WX', '/O2', '/MT',
               '/D_CRT_SECURE_NO_WARNINGS', '-march=x86-64-v3', '-mtune=znver2',
               '-fuse-ld=lld', '/Brepro', f'/imsvc{msvc / "include"}']
        # Treat SDK headers as external; their MSVC-only pragmas are not warnings in our sources.
        cmd += [f'/imsvc{sdk / name}' for name in ('ucrt', 'um', 'winrt', 'shared')]
        cmd += [f'/Fo{out / (stem + ".obj")}', f'/Fe{out / (stem + ".exe")}', BASE / rel,
                '/link', '/Brepro', '/INCREMENTAL:NO', f'/LIBPATH:{msvc / "lib/x64"}',
                f'/LIBPATH:{libs / "ucrt/x64"}', f'/LIBPATH:{libs / "um/x64"}', 'kernel32.lib']
        if stem == 'select-driver':
            cmd += ['setupapi.lib', 'cfgmgr32.lib']
        commands[stem] = list(map(str, cmd))
        print(subprocess.list2cmdline(commands[stem]), flush=True)
        result = run(cmd, env=env, cwd=out)
        (out / (stem + '.build.log')).write_text(result + '\n', encoding='utf-8')
        if stem != 'receipt-pipe-fixture':
            run([out / (stem + '.exe'), '--help'], env=env)
    if inputs() != source_hashes:
        raise RuntimeError('Helper sources changed during the build')
    receipt = {'schema': 1, 'source_commit': source_commit, 'source_files': source_hashes,
               'source_dirty': bool(run(['git', '-C', repo, 'status', '--porcelain', '--', source_base])),
               'compiler': {'path': str(clang), 'sha256': digest(clang), 'version': version},
               'linker': {'path': str(linker), 'sha256': digest(linker), 'version': run([linker, '--version'])},
               'dependencies': {'msvc_headers_libraries': str(msvc), 'sdk_headers': str(sdk), 'sdk_libraries': str(libs)},
               'commands': commands,
               'files': {stem + '.exe': digest(out / (stem + '.exe')) for stem in SOURCES}}
    (out / 'build-manifest.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def verify(out, receipt):
    """Refuse stale, absent or modified outputs and source changes before staging."""
    if json.loads((Path(out) / 'build-manifest.json').read_text(encoding='utf-8')) != receipt:
        raise ValueError('Helper build manifest changed')
    if receipt.get('source_files') != inputs():
        raise ValueError('Helper source bytes changed')
    expected = {stem + '.exe' for stem in SOURCES}
    if set(receipt.get('files', {})) != expected:
        raise ValueError('Helper manifest file set differs')
    present = {p.name for p in Path(out).glob('*.exe')}
    if present != expected:
        raise ValueError('Helper executable set differs')
    for name, sha in receipt['files'].items():
        if digest(Path(out) / name) != sha:
            raise ValueError('Helper executable changed: ' + name)
    for key in ('compiler', 'linker'):
        if digest(receipt[key]['path']) != receipt[key]['sha256']:
            raise ValueError('Helper toolchain changed: ' + key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    receipt = build(args.out)
    verify(args.out, receipt)
    print('PASS: helper source build ' + str(args.out / 'build-manifest.json'))


if __name__ == '__main__':
    main()
