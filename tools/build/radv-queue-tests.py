# SPDX-License-Identifier: MIT
"""Test the compiled hosted queue winsys objects. Run via build-radv-queue-tests.ps1.

The scripted host exercises ownership and dispatch contracts, not a GPU or the
public Vulkan instance parser. Rebuild Mesa before running this test.

Two test programs, each case in its own process:
- radv_wddm2_queue_binding_test.c, compiled with radv_wddm2_cs.c's flags and
  linked against the DLL's cs/bo objects;
- radv_wddm2_hosted_sync_test.c (BD-038), when the tree has it, compiled with
  vk_wddm2_monitored_fence.c's flags and linked against the runtime libraries.
The cases are every entry of each file's own tests[] table, so a new test runs
without a change here. Each executable keeps its source's name: the deferred
destroy witness checks its own module name in the logged stack.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

CS_REL = 'src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c'
FENCE_REL = 'src/vulkan/runtime/vk_wddm2_monitored_fence.c'
WDDM2 = 'src/amd/vulkan/winsys/wddm2'
QUEUE_TEST = WDDM2 + '/tests/radv_wddm2_queue_binding_test.c'
SYNC_TEST = WDDM2 + '/tests/radv_wddm2_hosted_sync_test.c'
TIMEOUT = 60
# The winsys knobs a test must not inherit from the shell; the deferred destroy log and configuration are pointed
# at files of this run (the configuration at one that does not exist, so no test reads C:\BC250\tmp).
KNOBS = ('BC250_DEFERRED_DESTROY', 'BC250_DEFERRED_WITNESS', 'BC250_DEFERRED_CAP_MB', 'BC250_IB_NOCOPY',
         'BC250_IB_DWORDS', 'BC250_DEFERRED_SUMMARY_S', 'BC250_SUBMIT_COALESCE', 'BC250_GATHER_SLOTS',
         'BC250_PROGRESS_FENCE', 'BC250_TRACE_SUBMITS', 'BC250_DRAW_STATS')
SYSLIBS = ['kernel32.lib', 'synchronization.lib', 'ws2_32.lib', 'advapi32.lib',
           'user32.lib', 'dbghelp.lib', 'ole32.lib', 'shlwapi.lib']

def table(path):
    """The names of a test file's tests[] table, in order."""
    names = re.findall(r'^\s*\{"(\w+)", test_\w+\},', path.read_text(encoding='utf-8'), re.M)
    if not names:
        raise SystemExit('no tests[] table in ' + str(path))
    return names

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

def compile_command(rel):
    cc = json.load(open(os.path.join(BUILD, 'compile_commands.json')))
    for e in cc:
        if e['file'].replace('\\', '/').endswith(rel):
            return e['directory'], split_windows(e['command'])
    raise SystemExit('no compile command for ' + rel + ' in ' + BUILD)

def flags_only(argv, rel):
    keep = []
    for a in argv[1:]:
        u = a
        if u in ('/showIncludes', '/c') or u.startswith('/Fd') or u.startswith('/Fo'):
            continue
        if u.replace('\\', '/').endswith(rel):
            continue
        keep.append(u)
    keep.append('-I' + os.path.join(SRC, WDDM2))
    return keep

def run(argv, cwd, log):
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    log.write('> ' + subprocess.list2cmdline(argv) + '\n' + p.stdout + p.stderr + '\n')
    return p.returncode

def run_tests(exe, vdir, tests, env):
    results = {}
    for t in tests:
        try:
            p = subprocess.run([exe, t], capture_output=True, text=True, errors='replace', timeout=TIMEOUT, env=env)
            code, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            code, out, err = 'timeout', e.stdout or '', e.stderr or ''
            out = out if isinstance(out, str) else out.decode(errors='replace')
            err = err if isinstance(err, str) else err.decode(errors='replace')
        open(os.path.join(vdir, t + '.txt'), 'w', encoding='utf-8', errors='replace').write(
            out + '\n--- stderr ---\n' + err)
        fails = [l for l in out.splitlines() if l.startswith('FAIL')]
        passes = [l for l in out.splitlines() if l.startswith('PASS')]
        summary = [l for l in out.splitlines() if l.startswith('tests=')]
        results[t] = dict(exit=code if isinstance(code, str) else '0x%X' % (code & 0xFFFFFFFF),
                          passed=code == 0, checks_passed=len(passes), checks_failed=len(fails),
                          first_failures=fails[:4], summary=summary[0] if summary else None)
    return results


def build_program(out, log, test, rel, links):
    """Compile a test file with rel's recorded flags and link it; the executable keeps the source's name."""
    cwd, argv = compile_command(rel)
    obj, exe = out / (test.stem + '.obj'), out / (test.stem + '.exe')
    if run([argv[0], *flags_only(argv, rel), '/c', '/Fo' + str(obj), str(test)], cwd, log):
        raise SystemExit('test compile failed (' + test.name + '); see build.log')
    if run(['link', '/nologo', '/OUT:' + str(exe), str(obj), *map(str, links), *SYSLIBS], cwd, log):
        raise SystemExit('test link failed (' + test.name + '); see build.log')
    return exe


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
    env = {k: v for k, v in os.environ.items() if k not in KNOBS}
    env['BC250_DEFERRED_LOG'] = str(out / 'deferred-test.log')
    env['BC250_DEFERRED_CFG'] = str(out / 'absent.cfg')
    build = Path(BUILD)
    objdir = build / 'src/amd/vulkan/vulkan_radeon.dll.p'
    objects = [objdir / ('winsys_wddm2_radv_wddm2_' + k + '.c.obj') for k in ('cs', 'bo')]
    util = [build / p for p in ('src/vulkan/util/libvulkan_util.a', 'src/util/libmesa_util.a',
                                'src/c11/impl/libmesa_util_c11.a')]
    programs = [(Path(SRC) / QUEUE_TEST, CS_REL,
                 [*objects, build / 'src/amd/common/libamd_common.a', *util],
                 [Path(SRC) / CS_REL, Path(SRC) / WDDM2 / 'radv_wddm2_bo.c'])]
    if (Path(SRC) / SYNC_TEST).exists():
        programs.append((Path(SRC) / SYNC_TEST, FENCE_REL,
                         [build / p for p in ('src/vulkan/runtime/libvulkan_lite_runtime.a',
                                              'src/vulkan/runtime/libvulkan_instance.a',
                                              'src/compiler/libcompiler.a')] + util + ['gdi32.lib'],
                         [Path(SRC) / FENCE_REL]))
    record = {'source_head': subprocess.check_output(
        ['git', '-C', SRC, 'rev-parse', 'HEAD'], text=True).strip(), 'inputs': {}, 'programs': {}}
    # HEAD alone does not identify a modified working tree. Keep exact input hashes.
    for test, _, links, sources in programs:
        for p in [test, *sources, *[q for q in links if isinstance(q, Path)]]:
            record['inputs'][str(p)] = sha(p)
    record['inputs'][str(build / 'src/amd/vulkan/vulkan_radeon.dll')] = sha(build / 'src/amd/vulkan/vulkan_radeon.dll')
    ok = True
    with (out / 'build.log').open('w', encoding='utf-8', errors='replace') as log:
        for test, rel, links, _ in programs:
            exe = build_program(out, log, test, rel, links)
            vdir = out / test.stem
            vdir.mkdir()
            results = run_tests(str(exe), str(vdir), table(test), env)
            record['programs'][test.stem] = {'exe_sha256': sha(exe), 'results': results}
            for name, item in results.items():
                print(f"{test.stem} {name}: exit={item['exit']}, checks={item['checks_passed']}, "
                      f"failures={item['checks_failed']}")
            ok = ok and all(v['passed'] and v['checks_passed'] > 0 and not v['checks_failed']
                            for v in results.values())
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
