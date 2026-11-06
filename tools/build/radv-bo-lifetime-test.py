# SPDX-License-Identifier: MIT
"""Test the BO lifetime contract of a built WDDM2 winsys (BD-201, BD-212). Run via
build-radv-bo-lifetime-test.ps1.

The test file is src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_bo_lifetime_test.c
of the Mesa tree. It is independent of the implementation: a mocked D3DKMT host
checks every Evict, FreeGpuVirtualAddress and DestroyAllocation2 of every BO the
test destroys against the progress values the queues published at the destroy.
It is compiled with the recorded compile command of radv_wddm2_cs.c and linked
against:

  - radv_wddm2_bo.c, compiled HERE from the source with its own recorded flags
    plus -Dmalloc/-Dcalloc/-Drealloc pointing at the test's wrappers, so that the
    test can refuse every host allocation of the winsys for one call (the destroy
    path must need no host memory);
  - the build's radv_wddm2_cs.c object, unchanged;
  - the build's amd_common, vulkan_util, mesa_util and mesa_util_c11 libraries;
  - when the cs source calls the monitored fence (BD-317): vk_wddm2_monitored_fence.c,
    compiled HERE with its recorded flags (two data names renamed, see FENCE_RENAMES),
    and the build's vulkan_lite_runtime, vulkan_instance and compiler libraries.

A build with the dynamic C runtime (/MD) declares the C runtime functions
dllimport. The renamed allocators are the test's own functions, so the bo
translation unit is compiled with an empty _ACRTIMP: its calls into the C
runtime then go through the import library's thunks.

The lines of the winsys differ in three entry points, which this script reads
from the source (-DBD201_DEFERRED_API, -DBD201_BO_POOL,
-DBD201_HOSTED_IMPORT_FLAGS). The contract asserted is the decided one
(-DBD201_V2=1). --expect old builds the earlier contract, for a negative
control against a tree without the fix.

Both flavours of a build run the same cases: the test reads no BC250_* knob.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

WDDM2 = 'src/amd/vulkan/winsys/wddm2'
CS_REL = WDDM2 + '/radv_wddm2_cs.c'
BO_REL = WDDM2 + '/radv_wddm2_bo.c'
TEST_REL = WDDM2 + '/tests/radv_wddm2_bo_lifetime_test.c'
OBJDIR = 'src/amd/vulkan/vulkan_radeon.dll.p'
LIBS = ['src/amd/common/libamd_common.a', 'src/vulkan/util/libvulkan_util.a',
        'src/util/libmesa_util.a', 'src/c11/impl/libmesa_util_c11.a']
# BD-317: since c7456a94 radv_wddm2_cs.c calls vk_wddm2_monitored_fence_note_submitted. At a tree whose cs source
# has the call, the real vk_wddm2_monitored_fence.c of the tree is compiled with its recorded flags and linked with
# the Vulkan runtime libraries (and gdi32); an older tree links exactly as before. The test file defines its own
# vk_wddm2_monitored_fence_type and _hosted_type (the sync types the winsys names, unused here), and the real file
# defines them too, so these two data names are renamed in the real object only; its functions stay the real ones.
FENCE_REL = 'src/vulkan/runtime/vk_wddm2_monitored_fence.c'
FENCE_CALL = 'vk_wddm2_monitored_fence_note_submitted('
FENCE_RENAMES = ['-Dvk_wddm2_monitored_fence_type=bd317_real_vk_wddm2_monitored_fence_type',
                 '-Dvk_wddm2_monitored_fence_hosted_type=bd317_real_vk_wddm2_monitored_fence_hosted_type']
FENCE_PREFIX = 'vk_wddm2_monitored_fence_'


def test_defined_fence_names(text):
    """The vk_wddm2_monitored_fence_* names that a test file defines at file scope (the same rule as
    radv-queue-tests.py). The D3D ICD line's test also defines _hosted_bound_type and a _note_submitted stub; the
    real object gets each of these names renamed too, else the link finds two definitions. A data name counts when
    '=' follows it; a function name when its declarator ends in '{' before any ';'."""
    lines = text.splitlines()
    names = set()
    for i, line in enumerate(lines):
        if not line or line[0].isspace() or line.startswith(('extern', '#', '/', '*')):
            continue
        # A static definition does not clash with the real object, and a rename would leave the real name unresolved.
        if line.startswith('static') or (i and lines[i - 1].startswith('static')):
            continue
        for m in re.finditer(r'\b(' + FENCE_PREFIX + r'\w+)\b', line):
            rest = line[m.end():]
            if re.match(r'\s*=', rest):
                names.add(m.group(1))
            elif rest.lstrip().startswith('('):
                text_after, j = rest, i
                while ';' not in text_after and '{' not in text_after and j + 1 < len(lines):
                    j += 1
                    text_after += lines[j]
                if '{' in text_after and (';' not in text_after or text_after.index('{') < text_after.index(';')):
                    names.add(m.group(1))
    return sorted(names)


def fence_renames(test):
    """FENCE_RENAMES plus a rename of each fence name the test file defines itself."""
    have = {r[2:].split('=')[0] for r in FENCE_RENAMES}
    extra = [n for n in test_defined_fence_names(test.read_text(encoding='utf-8', errors='replace')) if n not in have]
    return FENCE_RENAMES + ['-D' + n + '=bd317_real_' + n for n in extra]
RUNTIME_LIBS = ['src/vulkan/runtime/libvulkan_lite_runtime.a', 'src/vulkan/runtime/libvulkan_instance.a',
                'src/compiler/libcompiler.a']
SYSLIBS = ['kernel32.lib', 'synchronization.lib', 'ws2_32.lib', 'advapi32.lib',
           'user32.lib', 'dbghelp.lib', 'ole32.lib', 'shlwapi.lib']
# The winsys knobs a test must not inherit from the environment.
KNOBS = ('BC250_DEFERRED_DESTROY', 'BC250_DEFERRED_WITNESS', 'BC250_DEFERRED_CAP_MB', 'BC250_IB_NOCOPY',
         'BC250_IB_DWORDS', 'BC250_DEFERRED_SUMMARY_S', 'BC250_SUBMIT_COALESCE', 'BC250_GATHER_SLOTS',
         'BC250_PROGRESS_FENCE', 'BC250_TRACE_SUBMITS', 'BC250_DRAW_STATS', 'BC250_BARRIER_TRACK',
         'BC250_DEFERRED_LOG', 'BC250_DEFERRED_CFG', 'BC250_MAKERESIDENT_LEGACY', 'AMDGPU_WDDM_DDI_TRACE')
ALLOC_DEFINES = ['-Dmalloc=bd201_test_malloc', '-Dcalloc=bd201_test_calloc', '-Drealloc=bd201_test_realloc']
LAB_MACRO = 'AMDGPU_WDDM_LAB_FLAVOUR'
TIMEOUT = 300


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
    """The build's own compile command for one source file, from the object in OBJDIR."""
    entries = json.load(open(os.path.join(build, 'compile_commands.json')))
    hits = [e for e in entries if e['file'].replace('\\', '/').endswith(rel)]
    if not hits:
        raise SystemExit('no compile command for ' + rel + ' in ' + build)
    for e in hits:
        if OBJDIR in e['command'].replace('\\', '/'):
            return e['directory'], split_windows(e['command'])
    return hits[0]['directory'], split_windows(hits[0]['command'])


def flags_only(argv, rel, source):
    keep = []
    for a in argv[1:]:
        if a in ('/showIncludes', '/c') or a.startswith('/Fd') or a.startswith('/Fo'):
            continue
        if a.replace('\\', '/').endswith(rel):
            continue
        keep.append(a)
    keep.append('-I' + os.path.join(source, WDDM2))
    return keep


def dynamic_crt(argv):
    return any(a in ('/MD', '/MDd', '-MD', '-MDd') for a in argv)


def flavour(argv):
    """'lab' when the recorded compile command defines the lab macro, else 'release' (BD-167)."""
    return 'lab' if any(re.match(r'[-/]D' + LAB_MACRO + r'(=|$)', a) for a in argv) else 'release'


def run(argv, cwd, log):
    log.write('> ' + subprocess.list2cmdline(argv) + '\n')
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    log.write((p.stdout or '') + (p.stderr or '') + '\n')
    log.flush()
    return p.returncode


def table(path):
    """The names of the test file's tests[] table, in order."""
    names = re.findall(r'^\s*\{"(\w+)", test_\w+\},', path.read_text(encoding='utf-8'), re.M)
    if not names:
        raise SystemExit('no tests[] table in ' + str(path))
    return names


def line_switches(source):
    """The three entry points the lines differ in, read from their headers.

    deferred: the deferred-destroy teardown entry point.
    pool: the pool of BO structs, freed after it on the lines that keep one.
    import_flags: whether buffer_from_hosted takes the import flags argument.
    """
    header = (Path(source) / WDDM2 / 'radv_wddm2_bo.h').read_text(encoding='utf-8')
    winsys = (Path(source) / 'src/amd/vulkan/radv_radeon_winsys.h').read_text(encoding='utf-8')
    hosted = [l for l in winsys.splitlines() if 'buffer_from_hosted' in l]
    return (1 if 'radv_wddm2_deferred_finish' in header else 0,
            1 if 'radv_wddm2_bo_pool_finish' in header else 0,
            1 if hosted and 'uint32_t flags' in hosted[0] else 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True, help='a new directory for the results')
    parser.add_argument('--test', action='append', default=[], help='one test name; repeatable')
    parser.add_argument('--expect', choices=('v2', 'old'), default='v2',
                        help="'v2' (default) asserts the decided contract; 'old' records its requirements as "
                             "notes, for a negative control against a tree without the fix")
    args = parser.parse_args()
    source, build = os.path.abspath(args.source), os.path.abspath(args.build)
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)

    test = Path(source) / TEST_REL
    if not test.exists():
        raise SystemExit('no test file at ' + str(test))
    api, pool, import_flags = line_switches(source)
    v2 = 1 if args.expect == 'v2' else 0
    switches = ['-DBD201_DEFERRED_API=%d' % api, '-DBD201_BO_POOL=%d' % pool,
                '-DBD201_HOSTED_IMPORT_FLAGS=%d' % import_flags, '-DBD201_V2=%d' % v2]

    cwd, cs_argv = compile_command(build, CS_REL)
    _, bo_argv = compile_command(build, BO_REL)
    compiler = cs_argv[0]
    which = flavour(cs_argv)
    crt = ['/D_ACRTIMP='] if dynamic_crt(bo_argv) else []
    clang = 'clang-cl' in os.path.basename(compiler).lower()
    linker = 'lld-link' if clang else 'link'
    linker_path = os.path.join(os.path.dirname(compiler), linker + '.exe')
    if not os.path.exists(linker_path):
        linker_path = linker

    objects = [Path(build) / OBJDIR / 'winsys_wddm2_radv_wddm2_cs.c.obj']
    fence = FENCE_CALL in (Path(source) / CS_REL).read_text(encoding='utf-8', errors='replace')
    libs = [Path(build) / p for p in LIBS + (RUNTIME_LIBS if fence else [])]
    fence_obj = out / 'vk_wddm2_monitored_fence.bd317.obj'
    missing = [str(p) for p in objects + libs if not p.exists()]
    if missing:
        raise SystemExit('the build directory lacks: ' + ', '.join(missing))

    record = {
        'source': source, 'build': build, 'flavour': which, 'deferred_api': api, 'bo_pool': pool,
        'hosted_import_flags': import_flags, 'expect': args.expect, 'v2_requirements_asserted': bool(v2),
        'dynamic_crt': bool(crt), 'compiler': compiler, 'linker': linker_path, 'fence_object': fence,
        'source_head': subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', '-C', source, 'status', '--porcelain',
                                                      '--untracked-files=no'], text=True).strip()),
        'inputs': {}, 'results': {},
    }
    for p in [test, Path(source) / BO_REL, Path(source) / CS_REL, *([Path(source) / FENCE_REL] if fence else []),
              *objects, *libs]:
        record['inputs'][str(p)] = sha(p)

    bo_obj = out / 'radv_wddm2_bo.lifetime.obj'
    test_obj = out / 'radv_wddm2_bo_lifetime_test.obj'
    exe = out / 'radv_wddm2_bo_lifetime_test.exe'
    ok = True
    with (out / 'build.log').open('w', encoding='utf-8', errors='replace') as log:
        log.write('deferred_api=%d bo_pool=%d import_flags=%d v2=%d flavour=%s compiler=%s linker=%s\n' %
                  (api, pool, import_flags, v2, which, compiler, linker_path))
        if run([bo_argv[0], *flags_only(bo_argv, BO_REL, source), *ALLOC_DEFINES, *crt, *switches,
                '/c', '/Fo' + str(bo_obj), str(Path(source) / BO_REL)], cwd, log):
            raise SystemExit('radv_wddm2_bo.c did not compile with the renamed allocators; see build.log')
        if run([cs_argv[0], *flags_only(cs_argv, CS_REL, source), *switches,
                '/c', '/Fo' + str(test_obj), str(test)], cwd, log):
            raise SystemExit('the test did not compile; see build.log')
        extra = []
        if fence:
            fcwd, fence_argv = compile_command(build, FENCE_REL)
            renames = fence_renames(test)
            record['fence_renames'] = renames
            if run([fence_argv[0], *flags_only(fence_argv, FENCE_REL, source), *renames,
                    '/c', '/Fo' + str(fence_obj), str(Path(source) / FENCE_REL)], fcwd, log):
                raise SystemExit('vk_wddm2_monitored_fence.c did not compile; see build.log')
            extra = [str(fence_obj), 'gdi32.lib']
        # /DEBUG keeps a PDB next to the test: a case that faults gives a named stack under cdb.
        if run([linker_path, '/nologo', '/DEBUG', '/PDB:' + str(exe.with_suffix('.pdb')),
                '/OUT:' + str(exe), str(test_obj), str(bo_obj),
                *map(str, objects), *extra, *map(str, libs), *SYSLIBS], cwd, log):
            raise SystemExit('the test did not link; see build.log')
        record['inputs'][str(exe)] = sha(exe)

        env = {k: v for k, v in os.environ.items() if k not in KNOBS}
        names = args.test or table(test)
        for name in names:
            try:
                p = subprocess.run([str(exe), name], capture_output=True, text=True, errors='replace',
                                   timeout=TIMEOUT, env=env)
                code, output = p.returncode, (p.stdout or '') + '\n--- stderr ---\n' + (p.stderr or '')
            except subprocess.TimeoutExpired as e:
                code = 'timeout'
                output = e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            (out / (name + '.txt')).write_text(output, encoding='utf-8', errors='replace')
            fails = [l for l in output.splitlines() if l.startswith('FAIL')]
            passes = [l for l in output.splitlines() if l.startswith('PASS')]
            notes = [l for l in output.splitlines() if l.startswith('NOTE') or l.startswith('     breach:')]
            item = {
                'exit': code if isinstance(code, str) else '0x%X' % (code & 0xFFFFFFFF),
                'passed': code == 0, 'checks_passed': len(passes), 'checks_failed': len(fails),
                'failures': fails, 'notes': notes,
            }
            record['results'][name] = item
            print('radv_wddm2_bo_lifetime_test (%s) %s: exit=%s, checks=%d, failures=%d' % (
                which, name, item['exit'], len(passes), len(fails)))
            for l in fails + notes:
                print('   ' + l)
            ok = ok and code == 0 and bool(passes) and not fails
    (out / 'record.json').write_text(json.dumps(record, indent=2))
    print('record: ' + str(out / 'record.json'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
