# SPDX-License-Identifier: MIT
"""Test the instance-level part of the hosted ICD's bc250GetMemoryAllocation (BD-309).

The test file is src/amd/vulkan/tests/radv_bc250_memory_query_test.c of the Mesa tree. It reaches the
query through vk_icdGetInstanceProcAddr with a NULL instance, as the D3D11 shell does, with a device
and a memory object made by hand and a fake winsys hook. The winsys side of the query is
radv_wddm2_bo_lifetime_test kmt_allocation (radv-bo-lifetime-test.py).

radv_instance.c is compiled HERE from the source, with the build's recorded compile command for it,
and so is the test. The two objects are linked with /FORCE:UNRESOLVED: the rest of the instance code
needs the whole driver, and no part of it runs in this test (the query and the name lookup before it
call nothing outside radv_instance.c). The linker's list of unresolved symbols goes to build.log.

Usage: radv-memory-query-test.py --source <mesa tree> --build <meson build dir> --output <new dir>
The last line is "MEMORY-QUERY PASS" or "MEMORY-QUERY FAIL <why>"; the exit code is 0 for a pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

INSTANCE_REL = 'src/amd/vulkan/radv_instance.c'
TEST_REL = 'src/amd/vulkan/tests/radv_bc250_memory_query_test.c'
# The build forces util/amdgpu_wddm_stdio.h into every file (/FI), which sends printf through amdgpu_wddm_stdio()
# of mesa_util. The test is compiled without it, so its lines go to its own stdout and need no Mesa library.
FORCED_STDIO = 'amdgpu_wddm_stdio.h'
SYSLIBS = ['kernel32.lib', 'synchronization.lib', 'ws2_32.lib', 'advapi32.lib', 'user32.lib', 'dbghelp.lib',
           'ole32.lib', 'shlwapi.lib']
TIMEOUT = 60


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest().upper()


def split_windows(command):
    """The command line as the compiler itself parses it (CommandLineToArgvW)."""
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


def compile_command(build, rel):
    entries = json.load(open(os.path.join(build, 'compile_commands.json')))
    hits = [e for e in entries if e['file'].replace('\\', '/').endswith(rel)]
    if not hits:
        raise SystemExit('MEMORY-QUERY FAIL no compile command for ' + rel + ' in ' + build)
    return hits[0]['directory'], split_windows(hits[0]['command'])


def flags_only(argv, rel):
    keep = []
    for a in argv[1:]:
        if a in ('/showIncludes', '/c') or a.startswith('/Fd') or a.startswith('/Fo'):
            continue
        if a.replace('\\', '/').endswith(rel):
            continue
        keep.append(a)
    return keep


def run(argv, cwd, log):
    log.write('> ' + subprocess.list2cmdline(argv) + '\n')
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    log.write((p.stdout or '') + (p.stderr or '') + '\n')
    log.flush()
    return p.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True, help='a new directory for the results')
    args = parser.parse_args()
    source, build = os.path.abspath(args.source), os.path.abspath(args.build)
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    test = Path(source) / TEST_REL
    instance = Path(source) / INSTANCE_REL
    if not test.exists():
        raise SystemExit('MEMORY-QUERY FAIL no test file at ' + str(test))

    cwd, argv = compile_command(build, INSTANCE_REL)
    compiler = argv[0]
    flags = flags_only(argv, INSTANCE_REL)
    linker = os.path.join(os.path.dirname(compiler), 'lld-link.exe')
    if not os.path.exists(linker):
        raise SystemExit('MEMORY-QUERY FAIL no lld-link next to ' + compiler)
    record = {
        'source': source, 'build': build, 'compiler': compiler, 'linker': linker,
        'source_head': subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', '-C', source, 'status', '--porcelain',
                                                      '--untracked-files=no'], text=True).strip()),
        'inputs': {str(test): sha(test), str(instance): sha(instance)},
    }
    instance_obj, test_obj, exe = out / 'radv_instance.obj', out / 'radv_bc250_memory_query_test.obj', \
        out / 'radv_bc250_memory_query_test.exe'
    with (out / 'build.log').open('w', encoding='utf-8', errors='replace') as log:
        if run([compiler, *flags, '/c', '/Fo' + str(instance_obj), str(instance)], cwd, log):
            raise SystemExit('MEMORY-QUERY FAIL radv_instance.c did not compile; see build.log')
        test_flags = [a for a in flags if not (a.upper().startswith('/FI') and a.endswith(FORCED_STDIO))]
        if len(test_flags) != len(flags) - 1:
            raise SystemExit('MEMORY-QUERY FAIL expected one forced ' + FORCED_STDIO + ' in the compile command')
        if run([compiler, *test_flags, '/c', '/Fo' + str(test_obj), str(test)], cwd, log):
            raise SystemExit('MEMORY-QUERY FAIL the test did not compile; see build.log')
        if run([linker, '/nologo', '/DEBUG', '/FORCE:UNRESOLVED', '/PDB:' + str(exe.with_suffix('.pdb')),
                '/OUT:' + str(exe), str(test_obj), str(instance_obj), *SYSLIBS], cwd, log):
            raise SystemExit('MEMORY-QUERY FAIL the test did not link; see build.log')
    record['inputs'][str(exe)] = sha(exe)
    try:
        p = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace', timeout=TIMEOUT)
        code, output = p.returncode, (p.stdout or '') + (p.stderr or '')
    except subprocess.TimeoutExpired:
        code, output = 'timeout', ''
    (out / 'run.txt').write_text(output, encoding='utf-8')
    fails = [l for l in output.splitlines() if l.startswith('FAIL')]
    passes = [l for l in output.splitlines() if l.startswith('PASS')]
    record.update({'exit': code if isinstance(code, str) else '0x%X' % (code & 0xFFFFFFFF),
                   'checks_passed': len(passes), 'failures': fails})
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    for l in fails:
        print('   ' + l)
    print('radv_bc250_memory_query_test: exit=%s, checks=%d, failures=%d' % (record['exit'], len(passes), len(fails)))
    if code == 0 and passes and not fails:
        print('MEMORY-QUERY PASS')
        return 0
    print('MEMORY-QUERY FAIL exit %s, %d failures' % (record['exit'], len(fails)))
    return 1


if __name__ == '__main__':
    sys.exit(main())
