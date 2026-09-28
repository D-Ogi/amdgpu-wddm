# SPDX-License-Identifier: MIT
"""Test the compiled hosted queue winsys objects. Run via build-radv-queue-tests.ps1.

The scripted host exercises ownership and dispatch contracts, not a GPU or the
public Vulkan instance parser. Rebuild Mesa before running this test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

CS_REL = 'src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c'
WDDM2 = 'src/amd/vulkan/winsys/wddm2'
TESTS = ['bind_failure', 'second_queue', 'signal_wait', 'gather_teardown',
         'device_loss', 'abandon', 'outside_scope', 'internal_queue', 'sparse',
         'rebind', 'review_wait_failure', 'review_destroy_failure']
TIMEOUT = 60

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

def cs_command():
    cc = json.load(open(os.path.join(BUILD, 'compile_commands.json')))
    for e in cc:
        if e['file'].replace('\\', '/').endswith(CS_REL):
            return e['directory'], split_windows(e['command'])
    raise SystemExit('no compile command for radv_wddm2_cs.c in ' + BUILD)

def flags_only(argv):
    keep = []
    for a in argv[1:]:
        u = a
        if u in ('/showIncludes', '/c') or u.startswith('/Fd') or u.startswith('/Fo'):
            continue
        if u.replace('\\', '/').endswith(CS_REL):
            continue
        keep.append(u)
    keep.append('-I' + os.path.join(SRC, WDDM2))
    return keep

def run(argv, cwd, log):
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    log.write('> ' + subprocess.list2cmdline(argv) + '\n' + p.stdout + p.stderr + '\n')
    return p.returncode

def run_tests(exe, vdir):
    results = {}
    for t in TESTS:
        try:
            p = subprocess.run([exe, t], capture_output=True, text=True, errors='replace', timeout=TIMEOUT)
            code, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            code, out, err = 'timeout', e.stdout or '', e.stderr or ''
            out = out if isinstance(out, str) else out.decode(errors='replace')
            err = err if isinstance(err, str) else err.decode(errors='replace')
        open(os.path.join(vdir, t + '.txt'), 'w').write(out + '\n--- stderr ---\n' + err)
        fails = [l for l in out.splitlines() if l.startswith('FAIL')]
        passes = [l for l in out.splitlines() if l.startswith('PASS')]
        summary = [l for l in out.splitlines() if l.startswith('tests=')]
        results[t] = dict(exit=code if isinstance(code, str) else '0x%X' % (code & 0xFFFFFFFF),
                          passed=code == 0, checks_passed=len(passes), checks_failed=len(fails),
                          first_failures=fails[:4], summary=summary[0] if summary else None)
    return results


def main():
    global SRC, BUILD
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    SRC, BUILD = os.path.abspath(args.source), os.path.abspath(args.build)
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    test = Path(SRC) / WDDM2 / 'tests/radv_wddm2_queue_binding_test.c'
    objdir = Path(BUILD) / 'src/amd/vulkan/vulkan_radeon.dll.p'
    objects = [objdir / ('winsys_wddm2_radv_wddm2_' + k + '.c.obj') for k in ('cs', 'bo')]
    libraries = [Path(BUILD) / p for p in (
        'src/vulkan/util/libvulkan_util.a', 'src/amd/common/libamd_common.a',
        'src/util/libmesa_util.a', 'src/c11/impl/libmesa_util_c11.a')]
    libraries += ['kernel32.lib', 'synchronization.lib', 'ws2_32.lib', 'advapi32.lib',
                  'user32.lib', 'dbghelp.lib', 'ole32.lib', 'shlwapi.lib']
    cwd, argv = cs_command()
    flags = flags_only(argv)
    testobj, exe = out / 'queue-test.obj', out / 'queue-test.exe'
    record = {'source_head': subprocess.check_output(
        ['git', '-C', SRC, 'rev-parse', 'HEAD'], text=True).strip()}
    # HEAD alone does not identify a modified working tree. Keep exact input hashes.
    inputs = [test, Path(SRC) / CS_REL, Path(SRC) / WDDM2 / 'radv_wddm2_bo.c',
              Path(BUILD) / 'src/amd/vulkan/vulkan_radeon.dll', *objects,
              *[p for p in libraries if isinstance(p, Path)]]
    record['inputs'] = {str(p): sha(p) for p in inputs}
    with (out / 'build.log').open('w') as log:
        code = run([argv[0], *flags, '/c', '/Fo' + str(testobj), str(test)], cwd, log)
        if code:
            raise SystemExit('test compile failed; see build.log')
        code = run(['link', '/nologo', '/OUT:' + str(exe), str(testobj),
                    *map(str, objects), *map(str, libraries)], cwd, log)
        if code:
            raise SystemExit('test link failed; see build.log')
    record['exe_sha256'] = sha(exe)
    record['results'] = run_tests(str(exe), str(out))
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    results = record['results']
    for name, item in results.items():
        print(f"{name}: exit={item['exit']}, checks={item['checks_passed']}, failures={item['checks_failed']}")
    return 0 if all(v['passed'] and v['checks_passed'] > 0 and not v['checks_failed'] for v in results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
