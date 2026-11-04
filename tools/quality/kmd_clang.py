"""Clang commands for the kernel-mode driver, made from the cl.exe commands of its build.

driver/kmd/build.ps1 writes compile_commands.json with the exact cl.exe command of every translation unit. The
clang tools (kmd_tidy.py, kmd_ism.py) replay those commands with clang-cl in MSVC mode: the same defines, include
directories, packing and language mode, but no object, PDB or optimisation output.
"""
import json
import os
from pathlib import Path
import re

from msvc_analysis import split_command

# The default LLVM release of this workspace (CLAUDE.md compiler policy). --llvm overrides it.
LLVM_RELEASE = 'llvm-23.1.4-win64'

# cl.exe options that make outputs or change code generation only. Clang does not need them to parse and
# analyse the code, and /WX would make every warning that a check reports an error.
_DROP_PREFIX = ('/fo', '/fd', '/fe', '/fa', '/zi', '/z7', '/wx', '/brepro', '/gf', '/gy', '/o1', '/o2', '/od',
                '/ox', '/nologo', '/showincludes', '/analyze', '/sourcedependencies', '/mp', '/fs')
_DROP_EXACT = ('/c', '-c')

# Every clang command gets these. The target and the two MS modes are what clang-cl gives by default; they are
# explicit so that a change of the clang-cl default cannot change the parse. The WDK headers use Microsoft
# pragmas (prefast, warning numbers) that clang does not know: those warnings are about the kit, not the driver.
CLANG_EXTRA = ['--target=x86_64-pc-windows-msvc', '-fms-extensions', '-fms-compatibility',
               '-Wno-unknown-pragmas', '-Wno-nonportable-include-path', '-Wno-ignored-pragma-intrinsic',
               '-Wno-pragma-pack', '-Wno-microsoft-anon-tag', '-Wno-microsoft-enum-forward-reference',
               '/FI' + str(Path(__file__).resolve().with_name('kmd_clang_prelude.h'))]


def default_llvm_bin(workspace=None):
    """The bin directory of the workspace LLVM: BC250_LLVM_BIN, else <workspace>/toolchain/<release>/bin."""
    if os.environ.get('BC250_LLVM_BIN'):
        return Path(os.environ['BC250_LLVM_BIN'])
    root = workspace or os.environ.get('BC250_ROOT')
    if not root:
        # tools/quality -> repository -> workspace (the repository is <workspace>/bc250-win or a worktree of it).
        root = Path(__file__).resolve().parents[2]
        if not (Path(root) / 'toolchain').is_dir():
            root = Path(r'P:\BC-250')
    return Path(root) / 'toolchain' / LLVM_RELEASE / 'bin'


def load_entries(database, match):
    """The compile database entries whose file (with forward slashes) matches the regular expression."""
    db = json.loads(Path(database).read_text(encoding='utf-8-sig'))
    pattern = re.compile(match)
    selected = [e for e in db if pattern.search(e['file'].replace('\\', '/'))]
    if not selected:
        raise ValueError('No compilation commands matched ' + match + ' in ' + str(database))
    return selected


def entry_argv(entry):
    return list(entry.get('arguments') or split_command(entry['command']))


def clang_cl_argv(entry, clang_cl):
    """The clang-cl command of one entry: the build's options without output options, the source last."""
    argv = entry_argv(entry)
    compiler = Path(argv[0]).name.lower()
    if compiler not in ('cl.exe', 'cl', 'clang-cl.exe', 'clang-cl'):
        raise ValueError('Expected cl.exe or clang-cl.exe, found ' + argv[0])
    source = str(Path(entry['directory'], entry['file']))
    options = []
    for arg in argv[1:]:
        low = arg.lower()
        if low in _DROP_EXACT or low.startswith(_DROP_PREFIX):
            continue
        if Path(arg).name.lower() == Path(entry['file']).name.lower() and Path(entry['directory'], arg) == Path(source):
            continue
        options.append(arg)
    return [str(clang_cl), *CLANG_EXTRA, *options, source]


def repo_relative(path, repo):
    """A path as a repository-relative POSIX string, or the absolute path when it is outside the repository."""
    try:
        return Path(path).resolve().relative_to(Path(repo).resolve()).as_posix()
    except ValueError:
        return Path(path).as_posix()
