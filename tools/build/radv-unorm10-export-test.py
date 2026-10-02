# SPDX-License-Identifier: MIT
"""Test the rounding of 10-bit UNORM colour exports (BD-049) in a built RADV tree. Run via
build-radv-unorm10-export-test.ps1.

The test source (src/amd/vulkan/tests/radv_unorm10_export_test.c in the Mesa fork) is compiled with the compile
command of radv_pipeline_graphics.c and linked against a static library made from the vulkan_radeon.dll objects plus
the libraries the DLL links. It runs twice: as is (must pass with no failures) and with --no-round, the negative
control, which must fail in both the NIR and the ACO export path. No GPU is involved. Rebuild Mesa first; the runner
does not establish object freshness.

Usage: radv-unorm10-export-test.py --source SRC --build BUILD --output NEW_DIR
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

TEST_REL = 'src/amd/vulkan/tests/radv_unorm10_export_test.c'
FLAGS_FROM = 'src/amd/vulkan/radv_pipeline_graphics.c'
DLL_TARGET = 'src/amd/vulkan/vulkan_radeon.dll'
TIMEOUT = 600


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest().upper()


def split_windows(command):
    """The command line as cl.exe itself parses it (CommandLineToArgvW)."""
    import ctypes
    from ctypes import wintypes
    f = ctypes.windll.shell32.CommandLineToArgvW
    f.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    f.restype = ctypes.POINTER(wintypes.LPWSTR)
    n = ctypes.c_int()
    argv = f(command, ctypes.byref(n))
    try:
        return [argv[i] for i in range(n.value)]
    finally:
        ctypes.windll.kernel32.LocalFree(argv)


def compile_command(build):
    for e in json.load(open(os.path.join(build, 'compile_commands.json'))):
        if e['file'].replace('\\', '/').endswith(FLAGS_FROM):
            return e['directory'], split_windows(e['command'])
    raise SystemExit('no compile command for ' + FLAGS_FROM)


def flags_only(argv):
    keep = []
    for a in argv[1:]:
        if a in ('/showIncludes', '/c') or a.startswith(('/Fd', '/Fo')) or a.replace('\\', '/').endswith(FLAGS_FROM):
            continue
        keep.append(a)
    return keep


def dll_link(build):
    """Inputs and libraries of the DLL's link edge in build.ninja, without the DLL-only switches."""
    text = open(os.path.join(build, 'build.ninja'), encoding='utf-8').read()
    m = re.search(r'^build ' + re.escape(DLL_TARGET) + r' \| [^\n]*?: \S+ ([^\n|]*)', text, re.M)
    if not m:
        m = re.search(r'^build ' + re.escape(DLL_TARGET) + r': \S+ ([^\n|]*)', text, re.M)
    if not m:
        raise SystemExit('no link edge for ' + DLL_TARGET)
    objects = [o.replace('$ ', ' ') for o in m.group(1).split() if o.endswith('.obj')]
    start = m.end()
    args = re.search(r'^ LINK_ARGS = (.*)$', text[start:], re.M).group(1)
    libs = []
    for a in shlex.split(args, posix=False):
        a = a.strip('"')
        if a.startswith('/WHOLEARCHIVE:'):
            libs.append(a[len('/WHOLEARCHIVE:'):])
        elif a.startswith('/') and not a.startswith('/NODEFAULTLIB'):
            continue
        else:
            libs.append(a)
    return objects, libs


def run(argv, cwd, log):
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    log.write('> ' + subprocess.list2cmdline(argv) + '\n' + p.stdout + p.stderr + '\n')
    return p.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    src, build = os.path.abspath(args.source), os.path.abspath(args.build)
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    test = Path(src) / TEST_REL
    cwd, argv = compile_command(build)
    objects, libs = dll_link(build)
    radv_lib, testobj, exe = out / 'radv_objects.lib', out / 'unorm10-test.obj', out / 'unorm10-test.exe'
    record = {'source_head': subprocess.check_output(['git', '-C', src, 'rev-parse', 'HEAD'], text=True).strip(),
              'source_status': subprocess.check_output(['git', '-C', src, 'status', '--porcelain'], text=True),
              'inputs': {str(p): sha(p) for p in (test, Path(build) / DLL_TARGET)}}
    with (out / 'build.log').open('w') as log:
        if run([argv[0], *flags_only(argv), '/c', '/Fo' + str(testobj), str(test)], cwd, log):
            raise SystemExit('test compile failed; see build.log')
        rsp = out / 'objects.rsp'
        rsp.write_text('\n'.join('"' + str(Path(cwd) / o) + '"' for o in objects))
        if run(['lib', '/nologo', '/OUT:' + str(radv_lib), '@' + str(rsp)], cwd, log):
            raise SystemExit('object library failed; see build.log')
        if run(['link', '/nologo', '/DEBUG', '/SUBSYSTEM:CONSOLE', '/OUT:' + str(exe), str(testobj), str(radv_lib),
                *libs], cwd, log):
            raise SystemExit('test link failed; see build.log')
    record['exe_sha256'] = sha(exe)
    results = {}
    for name, extra, must_pass in (('test', [], True), ('negative-control', ['--no-round'], False)):
        d = out / name
        d.mkdir()
        try:
            p = subprocess.run([str(exe), *extra, str(d)], capture_output=True, text=True, errors='replace',
                               timeout=TIMEOUT)
            code, text = p.returncode, p.stdout + '\n--- stderr ---\n' + p.stderr
        except subprocess.TimeoutExpired:
            code, text = 'timeout', ''
        (out / (name + '.txt')).write_text(text)
        summary = [l for l in text.splitlines() if l.startswith('unorm10 export')]
        fails = [l for l in text.splitlines() if l.startswith('FAIL')]
        parts = re.search(r'\(key (\d+), NIR (\d+), ACO (\d+)\)', summary[0]) if summary else None
        parts = dict(zip(('key', 'nir', 'aco'), map(int, parts.groups()))) if parts else None
        if must_pass:
            ok = code == 0 and not fails and parts == dict(key=0, nir=0, aco=0)
        else:
            # Both export paths must show the defect without the mask; the key checks do not depend on it.
            ok = code == 1 and bool(fails) and bool(parts) and parts['nir'] > 0 and parts['aco'] > 0
        results[name] = dict(exit=code, ok=ok, summary=summary[0] if summary else None, failures=parts,
                             first_failures=fails[:4])
        print(f"{name}: exit={code}, {'PASS' if ok else 'FAIL'}: {summary[0] if summary else 'no summary'}")
    record['results'] = results
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    return 0 if all(r['ok'] for r in results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
