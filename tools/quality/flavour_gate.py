"""BD-167 flavour gate: no raw diagnostic escape and no forbidden override name in the released binaries.

    python flavour_gate.py --flavour release --package <dir> [--out <dir>]
    python flavour_gate.py --flavour release --package <package top> --manifest <files\\manifest.json>
                           --negative-control <payload path> [--out <dir>]
    python flavour_gate.py --flavour release --kmd <bc250kmd.sys> --pe <file|dir> [--pe ...] [--out <dir>]
    python flavour_gate.py --flavour lab --kmd <bc250kmd.sys> --kmd-only

An attestation-signed driver must not give user mode a raw read, write or submit primitive, and its user-mode
engines must not load shader code or another driver from a path that an environment variable names. The release
flavour removes both at compile time. One switch names the lab flavour everywhere: the C macro
AMDGPU_WDDM_LAB_FLAVOUR, which driver\\kmd\\build.ps1 -Lab, the shell builds with -Lab and the engine forks with
the meson option amdgpu-wddm-lab-flavour=true define. A build without it is the release flavour: the KMD has no raw
escape handler and the engines and shells have no lookup of the forbidden names. The criterion is the absence of
the string in the binary. A lookup that is compiled out passes; a lookup that is present but ignored at run time
fails.

The declaration (flavour_gate.json next to this file) holds:
  - the two KMD flavour lines that driver\\kmd\\entry.c logs, one per flavour;
  - one marker for each family of raw escapes: a format string that only the lab handler of that family logs.
    The KMD links with /OPT:REF, so the marker is in the SYS when the handler is compiled and dispatched;
  - the FORBIDDEN override names, each with what it does and the files whose lab build must still hold it
    (lab.every), or the reason no lab file holds it (lab.none, which the lab arm verifies: no file of a lab set may
    hold that name). An item of lab.every is a file name (every copy of that file) or a payload path such as
    d3d11/amdgpu_wddm_radv.dll (that copy only). A file's payload path is its path under the last directory named
    payload, else under the scanned directory, in lower case with forward slashes. A DriverStore member
    (payload/kmd/<member>, the table of tools/release/driver_package.py) has the payload path of its input slot:
    kmd/d3d11/amdgpu_wddm_radv_d3d11.dll is d3d11/amdgpu_wddm_radv.dll and
    kmd/wow64/d3d12/amdgpu_wddm_vkd3d32.dll is wow64/d3d12/amdgpu_wddm_vkd3d.dll, so one declaration serves both
    package layouts, and a file name item compares with the last part of the payload path. Paths make the list
    exact: a copy of a file named by path items that is not listed must not hold the name, and lab.note says why
    those copies lack it (for example another build line);
  - the ALLOWED names, each with the reason it stays, and the OUT-OF-SCOPE names, each with the reason. Neither
    list fails a set; out-of-scope hits are recorded. A name in two lists is an error. An allowed entry can carry a
    release_rule: in a release set, every file that holds the name must also hold each text of "requires" and no
    whole C string of "refuses" (any letter case). An allowed reason that starts with "BD-167 follow-up" is an open
    decision, and a release set fails while one is open;
  - the PACKAGE slots: the payload path of every PE that a whole package of either flavour holds (setup.exe in the
    top folder included). --manifest checks a package against this closed list.

Windows reads environment names in any letter case, so a forbidden name also counts as a whole C string (NUL on
both sides, ASCII or UTF-16LE) in another case, such as "radv_debug". Inside a longer text a name in another case
is ordinary prose and does not count ("zink_debug & ZINK_DEBUG..." in an assertion message).

Coverage: every identifier of this driver's own prefixes (BC250_, BC250<letters or digits>_ such as BC250DXVK_,
and AMDGPU_WDDM_) that a scanned file holds, in ASCII or UTF-16LE, must be in one of the three lists, and so must
the upper-case form of a whole C string that matches those prefixes in another case. An undeclared one fails
either flavour, so a new lookup cannot ship without a decision. The search has no left boundary, so that no
neighbouring byte can hide a name: a longer identifier that only ends in such a name shows up as that name and must
be declared too.

--flavour release passes when the KMD carries the release line, not the lab line and no escape marker, no
scanned PE (the KMD included) carries a forbidden name as a whole word, in ASCII or UTF-16LE, every prefixed
identifier is declared, every release rule holds and no decision is open.
--flavour lab passes when the KMD carries the lab line, not the release line, and every escape marker, and every
scanned file named in a forbidden entry's lab.every carries that name, with at least one such file in the set (for a
path item: a file at that path), no unlisted copy of a file named by path items carries it, no file carries a name
whose entry says lab.none, and every prefixed identifier is declared. So a build that lost the lab switch, a marker
list that no longer matches the handlers, or a path list that no longer matches the build lines, fails in the lab
arm instead of passing in silence. --kmd-only checks the KMD alone. A lab set never passes as release and a release
set never passes as lab.

--manifest binds the scan to one package (tools/release/build-release.ps1 runs it after manifest.json is written,
before the archive). Every PE under --package must be a row of the manifest (or its setup file) with the SHA256 of
the bytes this run scanned, every PE row of the manifest must be scanned, and the payload paths of the scanned PEs
must be the declaration's package slots, each exactly once. --negative-control <payload path> then adds a forbidden
name and an undeclared prefixed name to the scanned bytes of that one file, in memory, and the per-file check must
report both (release) or the undeclared one (lab); a scan that does not see them fails the set.

NDEBUG: the release flavour of every Mesa, DXVK and vkd3d-proton build config that a release row builds
(tools/release/release-sources.json, the recipe and its -Config) must define NDEBUG, so that no assert and no
debug-only parser is live in a shipped DLL. Its option set is "options" and then "release_options" (the recipe's
*-configs.json, NDEBUG_RECIPES), and the last -Db_ndebug of the two must be true. release_without_ndebug() names
every config that fails. Every other recipe of a release row compiles its shipped objects with NDEBUG in the release
flavour and with an optimisation level: release_recipe_findings() names a recipe that no table here classifies, a
file of RELEASE_COMPILES that does not spell its define, and a compiling file without /O1, /O2 or /Ox (clang: -O1 to
-O3 or -Os). test_flavour_gate.py runs both on the repository's own files. --flavour release also
fails every scanned PE with a live assert (live_asserts()): an import of _assert, _wassert or RtlAssert, an import
from a debug C runtime, or the assert message of the static C runtime, which the linker keeps only when some code
calls assert. A lab set keeps its asserts and only records them.

The gate fails closed: no flavour, a missing file, a file with a PE suffix that is not a PE image, no KMD, two KMD
files in a package, another kernel driver (.sys) in the set, two files with one payload path under one directory,
no other PE without --kmd-only, a malformed declaration or manifest, an entry without a reason, or a name in two
lists. A directory scan takes every file with a PE suffix and every other file that starts with a PE header. --out
writes flavour-gate.json with the SHA256 of every scanned file, its payload path and every finding, and in
--manifest mode the SHA256 of the manifest, the declaration and this gate.
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
DECLARATION = HERE / 'flavour_gate.json'
PE_SUFFIXES = ('.dll', '.sys', '.exe')
NAME = re.compile(r'[A-Z][A-Z0-9_]{2,}')
WORD = re.compile(rb'[A-Za-z0-9_]')
# The identifiers of this driver's own prefixes, which the declaration must cover (BD-167 coverage). The last
# character is not an underscore, so "BC250_FOO_%u" gives BC250_FOO and a bare prefix gives nothing.
PREFIXED = re.compile(r'(?:BC250[A-Z0-9]*|AMDGPU_WDDM)_[A-Z0-9_]*[A-Z0-9]')
UTF16_RUN = re.compile(rb'(?:[\x20-\x7e]\x00)+')
# Whole C strings: a run of printable characters with a NUL after it. The run starts after a byte that is not
# printable, so its first character can be a stray byte before the string; whole_strings() also tries the run
# without it.
ASCII_STRING = re.compile(rb'[\x20-\x7e]{3,}(?=\x00)')
UTF16_STRING = re.compile(rb'(?:[\x20-\x7e]\x00){3,}(?=\x00\x00)')
OPEN_DECISION = 'BD-167 follow-up'
NEGATIVE_FORBIDDEN = 'RADV_DEBUG'
NEGATIVE_UNDECLARED = 'BC250_FLAVOUR_GATE_NEGATIVE_CONTROL'


class GateError(Exception):
    """The input cannot be judged: the gate fails closed."""


def payload_path_ok(f):
    return (isinstance(f, str) and f == f.lower() and '\\' not in f and ':' not in f and
            all(p not in ('', '.', '..') for p in f.split('/')) and f.endswith(PE_SUFFIXES))


def load_declaration(path):
    try:
        d = json.loads(pathlib.Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        raise GateError('declaration %s unreadable: %s' % (path, e))
    if not isinstance(d, dict) or d.get('schema') != 1:
        raise GateError('declaration: schema 1 expected')
    kmd = d.get('kmd')
    if not isinstance(kmd, dict):
        raise GateError('declaration: no kmd section')
    if not isinstance(kmd.get('file'), str) or not kmd['file'].lower().endswith('.sys'):
        raise GateError('declaration: kmd.file must name the .sys')
    lines = kmd.get('flavour_markers')
    if not isinstance(lines, dict) or set(lines) != {'release', 'lab'}:
        raise GateError('declaration: kmd.flavour_markers needs exactly release and lab')
    if not all(isinstance(v, str) and v for v in lines.values()) or lines['release'] == lines['lab']:
        raise GateError('declaration: the two flavour markers must be distinct non-empty strings')
    if lines['release'] in lines['lab'] or lines['lab'] in lines['release']:
        raise GateError('declaration: one flavour marker contains the other')
    escapes = kmd.get('lab_escape_markers')
    if not isinstance(escapes, list) or not escapes:
        raise GateError('declaration: kmd.lab_escape_markers is empty')
    seen = set()
    for e in escapes:
        if not isinstance(e, dict) or not all(isinstance(e.get(k), str) and e[k] for k in ('escapes', 'marker', 'source')):
            raise GateError('declaration: an escape marker needs escapes, marker and source')
        if e['marker'] in seen:
            raise GateError('declaration: escape marker listed twice: %s' % e['marker'])
        seen.add(e['marker'])
    lists = {}
    for kind, field in (('forbidden', 'does'), ('allowed', 'reason'), ('out_of_scope', 'reason')):
        entries = d.get(kind, [] if kind == 'out_of_scope' else None)
        if not isinstance(entries, list) or (not entries and kind != 'out_of_scope'):
            raise GateError('declaration: %s list is empty' % kind)
        names = []
        for e in entries:
            if not isinstance(e, dict) or not isinstance(e.get('name'), str) or not NAME.fullmatch(e['name']):
                raise GateError('declaration: bad %s entry %r' % (kind, e))
            if not isinstance(e.get(field), str) or not e[field].strip():
                raise GateError('declaration: %s %s has no %s' % (kind, e['name'], field))
            if not isinstance(e.get('where'), str) or not e['where'].strip():
                raise GateError('declaration: %s %s has no where' % (kind, e['name']))
            if e['name'] in names:
                raise GateError('declaration: %s %s listed twice' % (kind, e['name']))
            if kind == 'forbidden':
                check_lab_field(e)
            if 'release_rule' in e:
                if kind != 'allowed':
                    raise GateError('declaration: %s %s: only an allowed entry has a release_rule' % (kind, e['name']))
                check_release_rule(e)
            names.append(e['name'])
        lists[kind] = names
    for a, b in (('forbidden', 'allowed'), ('forbidden', 'out_of_scope'), ('allowed', 'out_of_scope')):
        both = sorted(set(lists[a]) & set(lists[b]))
        if both:
            raise GateError('declaration: in both %s and %s: %s' % (a, b, ', '.join(both)))
    package = d.get('package')
    if (not isinstance(package, dict) or not isinstance(package.get('slots'), list) or not package['slots'] or
            not all(payload_path_ok(s) for s in package['slots']) or
            len(set(package['slots'])) != len(package['slots'])):
        raise GateError('declaration: package.slots needs the distinct lower-case payload paths of a whole package')
    if not isinstance(package.get('about'), str) or not package['about'].strip():
        raise GateError('declaration: package.about is empty')
    kmd_slots = [s for s in package['slots'] if s.endswith('.sys')]
    if kmd_slots != ['kmd/' + kmd['file'].lower()]:
        raise GateError('declaration: package.slots must hold kmd/%s and no other .sys' % kmd['file'].lower())
    return d


def check_lab_field(e):
    """A forbidden entry's lab field: {"every": [file names or payload paths]}, with "note" when it has paths, or
    {"none": reason}."""
    lab = e.get('lab')
    if not isinstance(lab, dict) or set(lab) not in ({'every'}, {'every', 'note'}, {'none'}):
        raise GateError('declaration: forbidden %s needs lab {"every": [...]} (and "note" with paths) or '
                        '{"none": reason}' % e['name'])
    if 'none' in lab:
        if not isinstance(lab['none'], str) or not lab['none'].strip():
            raise GateError('declaration: forbidden %s: lab.none needs a reason' % e['name'])
        return
    files = lab['every']
    if (not isinstance(files, list) or not files or len(set(files)) != len(files) or
            not all(payload_path_ok(f) for f in files)):
        raise GateError('declaration: forbidden %s: lab.every needs distinct lower-case PE file names or payload '
                        'paths (d3d11/amdgpu_wddm_radv.dll)' % e['name'])
    paths = [f for f in files if '/' in f]
    both = sorted({f for f in files if '/' not in f} & {p.rsplit('/', 1)[1] for p in paths})
    if both:
        raise GateError('declaration: forbidden %s: lab.every names %s both as a file and by path'
                        % (e['name'], ', '.join(both)))
    if 'note' in lab and (not isinstance(lab['note'], str) or not lab['note'].strip()):
        raise GateError('declaration: forbidden %s: lab.note is empty' % e['name'])
    if paths and 'note' not in lab:
        raise GateError('declaration: forbidden %s: lab.every with paths needs lab.note, why the other copies '
                        'lack the name' % e['name'])


def check_release_rule(e):
    """{"requires": [texts], "refuses": [texts], "why": reason}: requires is not empty, every text is printable
    ASCII."""
    rule = e['release_rule']
    if not isinstance(rule, dict) or set(rule) - {'requires', 'refuses', 'why'} or 'requires' not in rule:
        raise GateError('declaration: allowed %s: release_rule needs requires (and refuses, why)' % e['name'])
    for key in ('requires', 'refuses'):
        texts = rule.get(key, [])
        if (not isinstance(texts, list) or (key == 'requires' and not texts) or
                not all(isinstance(t, str) and len(t) >= 3 and all(0x20 <= ord(c) <= 0x7e for c in t) for t in texts)):
            raise GateError('declaration: allowed %s: release_rule.%s needs printable texts of 3 or more '
                            'characters' % (e['name'], key))
    if not isinstance(rule.get('why'), str) or not rule['why'].strip():
        raise GateError('declaration: allowed %s: release_rule.why is empty' % e['name'])


_MEMBERS = None


def driverstore_members():
    """{DriverStore member path under payload: its input slot}, in lower case, from the packager's own table
    (tools/release/driver_package.py), with every optional member (the OpenGL ICD of both architectures, the
    encoder MFT)."""
    global _MEMBERS
    if _MEMBERS is None:
        sys.path.insert(0, str(HERE.parent / 'release'))
        try:
            import driver_package
        except ImportError as e:
            raise GateError('tools/release/driver_package.py cannot be imported: %s' % e)
        finally:
            sys.path.pop(0)
        rows = driver_package.members({'opengl_icd': {'register': True, 'architectures': ['x64', 'x86']},
                                       'mft_h264': {'register': True}})
        _MEMBERS = {r['path'].split('/', 1)[1].lower(): r['source'].split('/', 1)[1].lower() for r in rows}
    return _MEMBERS


# The recipes whose release flavour must define NDEBUG, each with the file of its option sets (NDEBUG above).
NDEBUG_RECIPES = {
    'tools/build/build-mesa.ps1': 'tools/build/mesa-configs.json',
    'tools/build/build-dxvk.ps1': 'tools/build/dxvk-configs.json',
    'tools/build/build-vkd3d.ps1': 'tools/build/vkd3d-configs.json',
}
# Every other recipe that a release row names (NDEBUG above), with the files that compile its shipped objects and
# their compiler. Each file must spell the compiler's NDEBUG define (RELEASE_DEFINE) and an optimisation level
# (RELEASE_OPTIMISE): a compile line without an /O flag builds at /Od, as the D3D11 shell's did until tester.31. The
# KMD defines NDEBUG in both flavours: a kernel ASSERT follows DBG, not NDEBUG.
RELEASE_DEFINE = {'cl': "'/DNDEBUG'", 'clang': "'-DNDEBUG'"}
RELEASE_OPTIMISE = {'cl': re.compile(r"(?<![\w/])/O[12x](?!\w)"), 'clang': re.compile(r"(?<![\w-])-O[123s](?!\w)")}
RELEASE_COMPILES = {
    'tools/build/build-umd-d3d12.ps1': (('tools/build/build-umd-d3d12.ps1', 'cl'),),
    'tools/build/build-umd-dxvk.ps1': (('tools/build/build-umd-dxvk.ps1', 'cl'),),
    'driver/umd/mft-h264/build.ps1': (('driver/umd/mft-h264/build.ps1', 'cl'),),
    'tools/win/amdgpu_wddm_control/build.ps1': (('tools/win/amdgpu_wddm_control/build.ps1', 'cl'),),
    'tools/win/d3d11bench/build.ps1': (('tools/win/d3d11bench/build.ps1', 'cl'),),
    'tools/win/d3d12caps/build.ps1': (('tools/win/d3d12caps/build.ps1', 'cl'),),
    'driver/state/installer-build.ps1': (('driver/state/installer-build.ps1', 'cl'),),
    'compute/hip/build-runtime.ps1': (('compute/hip/build-runtime.ps1', 'cl'),),
    'compute/hip/build-rebuilt-controls.ps1': (('compute/hip/build-rebuilt-controls.ps1', 'clang'),
                                               ('compute/hipblas/build-hipblas.ps1', 'clang')),
    'driver/kmd/build.ps1': (('driver/kmd/build.ps1', 'cl'),),
}
# The recipes of release rows that compile nothing.
NOT_COMPILED = {'tools/build/write-umd-config.py': 'writes a text settings file'}
# The recipes that do not define NDEBUG yet, each with its compiler and reason. Their optimisation level is checked.
# release_recipe_findings() fails one that spells the define, so its entry moves to RELEASE_COMPILES in the change
# that adds it.
NDEBUG_PENDING = {
    'tools/build/build-umd-router.ps1': ('cl', 'no assert() in the router sources, and the live-assert rule covers '
                                               'its bytes'),
}
# A live assert in a PE (NDEBUG above): an import of an assert function (the C runtime's, or RtlAssert of a checked
# kernel build), an import from a debug C runtime, or the static C runtime's assert message, which the linker keeps
# (/OPT:REF) only when some code calls assert.
ASSERT_IMPORTS = ('_assert', '_wassert', 'RtlAssert')
DEBUG_RUNTIMES = ('ucrtbased.dll', 'vcruntime140d.dll', 'vcruntime140_1d.dll', 'msvcp140d.dll')
CRT_ASSERT_TEXT = 'Assertion failed: %Ts, file %Ts, line %d'


def release_without_ndebug(configs, rows):
    """The build configs that a release row (release-sources.json "files") builds with a recipe of NDEBUG_RECIPES
    and whose release flavour does not define NDEBUG, as sorted (recipe, config, row path) triples. configs maps each
    recipe to the "configs" object of its file. The release flavour is "options" and then "release_options", which
    the recipe adds without -Lab; meson takes the last -Db_ndebug. A row without -Config, or a config that its
    recipe's file does not have, cannot be judged."""
    missing = []
    for row in rows:
        built = row.get('built_from') or {}
        recipe = built.get('recipe')
        if recipe not in NDEBUG_RECIPES:
            continue
        args = list(built.get('args') or [])
        if '-Config' not in args or args.index('-Config') + 1 >= len(args):
            raise GateError('%s: a %s row without -Config' % (row.get('path'), recipe))
        name = args[args.index('-Config') + 1]
        if name not in (configs.get(recipe) or {}):
            raise GateError('%s: -Config %s is not in %s' % (row.get('path'), name, NDEBUG_RECIPES[recipe]))
        config = configs[recipe][name]
        options = list(config.get('options') or []) + list(config.get('release_options') or [])
        values = [o.split('=', 1)[1] for o in options if o.startswith('-Db_ndebug=')]
        if not values or values[-1] != 'true':
            missing.append((recipe, name, row.get('path')))
    return sorted(missing)


def recipe_code(text):
    """A PowerShell recipe without its comments (<# #> blocks and lines that start with #): a flag that only a comment
    names compiles nothing."""
    text = re.sub(r'<#.*?#>', '', text, flags=re.S)
    return '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))


def release_recipe_findings(rows, read):
    """The recipes of release rows (release-sources.json "files") that are not seen to build their release flavour
    with NDEBUG and an optimisation level, as sorted (recipe, why) pairs: a recipe that RELEASE_COMPILES,
    NDEBUG_RECIPES, NOT_COMPILED and NDEBUG_PENDING do not name, a file of RELEASE_COMPILES that does not spell its
    define, a compiling file without an optimisation level, and a pending recipe that spells the define now. Comments
    do not count (recipe_code()). The meson recipes are release_without_ndebug()'s. read(path) gives the text of a
    repository file, or None when there is no such file."""
    found = []
    for recipe in sorted({(row.get('built_from') or {}).get('recipe') for row in rows} - {None}):
        if recipe in NDEBUG_RECIPES or recipe in NOT_COMPILED:
            continue
        if recipe in NDEBUG_PENDING:
            files, pending = ((recipe, NDEBUG_PENDING[recipe][0]),), True
        elif recipe in RELEASE_COMPILES:
            files, pending = RELEASE_COMPILES[recipe], False
        else:
            found.append((recipe, 'not classified (RELEASE_COMPILES, NDEBUG_RECIPES, NOT_COMPILED, NDEBUG_PENDING)'))
            continue
        for path, compiler in files:
            text = read(path)
            if text is None:
                found.append((recipe, '%s: no such file' % path))
                continue
            text = recipe_code(text)
            if not RELEASE_OPTIMISE[compiler].search(text):
                found.append((recipe, '%s spells no optimisation level (%s)' % (path, RELEASE_OPTIMISE[compiler].pattern)))
            if pending and RELEASE_DEFINE[compiler] in text:
                found.append((recipe, '%s defines NDEBUG now: move it from NDEBUG_PENDING to RELEASE_COMPILES' % path))
            elif not pending and RELEASE_DEFINE[compiler] not in text:
                found.append((recipe, '%s does not spell %s' % (path, RELEASE_DEFINE[compiler])))
    return sorted(found)


def read_pe(path):
    p = pathlib.Path(path)
    try:
        data = p.read_bytes()
    except OSError as e:
        raise GateError('%s: unreadable: %s' % (p, e))
    if len(data) < 0x40 or data[:2] != b'MZ':
        raise GateError('%s: not a PE image (no MZ header)' % p)
    pe = int.from_bytes(data[0x3c:0x40], 'little')
    if pe + 4 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise GateError('%s: not a PE image (no PE signature)' % p)
    return data


def pe_imports(data):
    """[(dll, function)] of the import directory of a PE image (read_pe), a function by name or '#<ordinal>'. An
    image without an optional header imports nothing. A directory that points outside the image cannot be judged."""
    def u(at, size):
        if at < 0 or at + size > len(data):
            raise GateError('the import directory points outside the image (offset 0x%x)' % at)
        return int.from_bytes(data[at:at + size], 'little')

    def text(at):
        end = data.find(b'\0', at)
        if at >= len(data) or end < 0:
            raise GateError('an import name outside the image (offset 0x%x)' % at)
        return data[at:end].decode('latin-1')

    coff = u(0x3c, 4) + 4
    count, optsize = u(coff + 2, 2), u(coff + 16, 2)
    opt = coff + 20
    if optsize == 0:
        return []
    magic = u(opt, 2)
    if magic not in (0x10b, 0x20b):
        raise GateError('optional header magic 0x%x is neither PE32 nor PE32+' % magic)
    wide = magic == 0x20b
    if u(opt + (108 if wide else 92), 4) < 2:
        return []
    rva = u(opt + (112 if wide else 96) + 8, 4)
    if not rva:
        return []
    sections = [(u(s + 12, 4), max(u(s + 8, 4), u(s + 16, 4)), u(s + 20, 4))
                for s in range(opt + optsize, opt + optsize + 40 * count, 40)]

    def at(r):
        for va, size, raw in sections:
            if va <= r < va + size:
                return raw + r - va
        raise GateError('import RVA 0x%x is in no section' % r)

    found, d, step = [], at(rva), 8 if wide else 4
    for _ in range(len(data) // 20):
        thunks, name, first = u(d, 4), u(d + 12, 4), u(d + 16, 4)
        if not (thunks or name or first):
            return found
        dll, t = text(at(name)), at(thunks or first)
        for _ in range(len(data) // step):
            v = u(t, step)
            if not v:
                break
            found.append((dll, '#%d' % (v & 0xffff) if v >> (8 * step - 1) else text(at(v & 0x7fffffff) + 2)))
            t += step
        d += 20
    raise GateError('the import directory has no terminating entry')


def live_asserts(data):
    """What keeps an assert live in one PE image, as texts: empty for an image built with NDEBUG."""
    found = []
    for dll, fn in pe_imports(data):
        if fn in ASSERT_IMPORTS:
            found.append('imports %s!%s' % (dll, fn))
        if dll.lower() in DEBUG_RUNTIMES and 'imports the debug runtime ' + dll not in found:
            found.append('imports the debug runtime ' + dll)
    for enc, key in (('ascii', 'ascii'), ('utf-16-le', 'utf16')):
        if CRT_ASSERT_TEXT.encode(enc) in data:
            found.append('holds the assert message of the static C runtime (%s)' % key)
    return found


def starts_as_pe(path):
    """A file without a PE suffix that still starts with an MZ header: the scan takes it, and read_pe() judges it."""
    try:
        with open(path, 'rb') as f:
            return f.read(2) == b'MZ'
    except OSError as e:
        raise GateError('%s: unreadable: %s' % (path, e))


def has_text(data, text):
    """The whole C string as it is stored, terminator included: ASCII or UTF-16LE. With the terminator, the
    marker of RUN_DCN is not found inside the marker of RUN_DCNFLIP."""
    return (text + '\0').encode('ascii') in data or (text + '\0').encode('utf-16-le') in data


def holds(data, text):
    """A required text of a release rule: an identifier as a whole word (has_name), other text anywhere."""
    if NAME.fullmatch(text):
        return any(has_name(data, text))
    return text.encode('ascii') in data or text.encode('utf-16-le') in data


def _char(data, at, unit):
    """The character of one code unit at a byte offset, or None: an ASCII byte, or a UTF-16LE unit below 0x80."""
    if at < 0 or at + unit > len(data):
        return None
    if unit == 2 and data[at + 1] != 0:
        return None
    return data[at:at + 1]


def has_name(data, name):
    """Occurrences of an override name, (ascii, utf16), that are not part of a longer identifier: the name inside
    "RADV_DEBUG=hang" counts, the name inside MY_RADV_DEBUG_LEVEL does not. A neighbour counts as part of the
    identifier only when it is a word character and the character beyond it is printable too, so that a stray
    byte next to a string (an alignment byte, the tail of an ASCII string before a UTF-16 one) cannot hide a name.
    Doubt makes a finding, never a pass."""
    counts = []
    for enc, unit in (('ascii', 1), ('utf-16-le', 2)):
        needle = name.encode(enc)
        n, start = 0, 0
        while True:
            i = data.find(needle, start)
            if i < 0:
                break
            start = i + unit
            end = i + len(needle)
            joined = False
            for near, far in ((i - unit, i - 2 * unit), (end, end + unit)):
                c, d = _char(data, near, unit), _char(data, far, unit)
                if c and WORD.fullmatch(c) and d and 0x20 <= d[0] <= 0x7e:
                    joined = True
            if not joined:
                n += 1
        counts.append(n)
    return tuple(counts)


def whole_strings(data):
    """{(text, 'ascii' or 'utf16'): count} of the whole C strings of a file, each also without its first character
    (which can be a stray byte before the string)."""
    found = {}
    for rx, enc, key in ((ASCII_STRING, 'latin-1', 'ascii'), (UTF16_STRING, 'utf-16-le', 'utf16')):
        for m in rx.finditer(data):
            s = m.group(0).decode(enc)
            for t in (s, s[1:]) if len(s) > 3 else (s,):
                found[(t, key)] = found.get((t, key), 0) + 1
    return found


def prefixed_names(data):
    """Every identifier of this driver's prefixes in a file, in ASCII or UTF-16LE (PREFIXED)."""
    names = {m.group(0) for m in PREFIXED.finditer(data.decode('latin-1'))}
    for run in UTF16_RUN.finditer(data):
        names.update(m.group(0) for m in PREFIXED.finditer(run.group(0).decode('utf-16-le')))
    return names


def slot(root, f):
    """The payload path of a file found under a scanned directory: its path under the last directory named payload,
    else under the scanned directory, in lower case with forward slashes; a DriverStore member has the payload path
    of its input slot."""
    parts = [p.lower() for p in f.relative_to(root).parts]
    if 'payload' in parts[:-1]:
        parts = parts[len(parts) - parts[::-1].index('payload', 1):]
    s = '/'.join(parts)
    if s.startswith('kmd/'):
        return driverstore_members().get(s, s)
    return s


def base(slots, p):
    """The file name a lab.every item compares with: the last part of the payload path."""
    return slots[p].rsplit('/', 1)[-1]


def collect(package, kmd, pes, kmd_name, slots=None):
    """The KMD path and the other PE paths. Fails closed on an ambiguous or empty set. slots, when given, gets the
    payload path of each file: a file named on the command line has only its file name."""
    files = []
    slots = {} if slots is None else slots
    roots = ([package] if package else []) + list(pes or [])
    for root in roots:
        r = pathlib.Path(root)
        if r.is_dir():
            found = sorted(f for f in r.rglob('*') if f.is_file() and
                           (f.suffix.lower() in PE_SUFFIXES or starts_as_pe(f)))
            if not found:
                raise GateError('%s: no PE file (%s) under it' % (r, '/'.join(PE_SUFFIXES)))
            here = {}
            for f in found:
                s = slot(r, f)
                if s in here:
                    raise GateError('%s and %s: both have the payload path %s under %s' % (here[s], f, s, r))
                here[s] = f
                slots.setdefault(f, s)
            files += found
        elif r.is_file():
            files.append(r)
            slots.setdefault(r, r.name.lower())
        else:
            raise GateError('%s: no such file or directory' % r)
    if kmd:
        k = pathlib.Path(kmd)
        if not k.is_file():
            raise GateError('%s: KMD not found' % k)
        slots.setdefault(k, k.name.lower())
    else:
        named = [f for f in files if f.name.lower() == kmd_name.lower()]
        if len(named) != 1:
            raise GateError('%d files named %s in the scanned set; pass --kmd' % (len(named), kmd_name))
        k = named[0]
    others, seen = [], {k.resolve()}
    for f in files:
        if f.resolve() not in seen:
            seen.add(f.resolve())
            if f.suffix.lower() == '.sys':
                raise GateError('%s: another kernel driver in the set; the gate judges one KMD (%s) and has no policy '
                                'for a second one' % (f, k))
            others.append(f)
    return k, others


def scan_file(decl, data, forbidden, out_of_scope, declared):
    """The findings of one file: forbidden and out-of-scope names (ascii, utf16 and, for a whole C string in another
    letter case, variant), and the undeclared prefixed identifiers."""
    strings = whole_strings(data)
    variants = {}
    lowered = {}
    for (t, enc), n in strings.items():
        lowered[t.lower()] = lowered.get(t.lower(), 0) + n
        u = t.upper()
        if u != t:
            variants.setdefault(u, {}).setdefault(enc, 0)
            variants[u][enc] += n
    hits, outside = {}, {}
    for names, into in ((forbidden, hits), (out_of_scope, outside)):
        for name in names:
            a, u = has_name(data, name)
            v = variants.get(name, {})
            if a or u or v:
                into[name] = {'ascii': a, 'utf16': u}
                if v:
                    into[name]['variant'] = dict(sorted(v.items()))
    undeclared = set(prefixed_names(data))
    undeclared.update(u for u in variants if PREFIXED.fullmatch(u))
    return {'forbidden': hits, 'out_of_scope': outside, 'undeclared': sorted(undeclared - declared),
            'strings_lower': lowered}


def describe(hit):
    text = 'ascii %d, utf16 %d' % (hit['ascii'], hit['utf16'])
    if hit.get('variant'):
        text += ', in another letter case %s' % ', '.join('%s %d' % kv for kv in hit['variant'].items())
    return text


def file_failures(decl, flavour, path, finding, data):
    """The failures of one file's findings, the same for the real scan and for the negative control."""
    failures = []
    if flavour == 'release':
        for name in sorted(finding['forbidden']):
            failures.append('%s: %s (%s)' % (path, name, describe(finding['forbidden'][name])))
        for e in decl['allowed']:
            rule = e.get('release_rule')
            if not rule or not any(has_name(data, e['name'])):
                continue
            for text in rule['requires']:
                if not holds(data, text):
                    failures.append('%s: holds %s but not %r: %s' % (path, e['name'], text, rule['why']))
            for text in rule.get('refuses', []):
                if finding['strings_lower'].get(text.lower()):
                    failures.append('%s: holds %s and the string %r: %s' % (path, e['name'], text, rule['why']))
    for name in finding['undeclared']:
        failures.append('%s: %s is not declared: add it to the forbidden, allowed or out_of_scope list of the '
                        'declaration (BD-167 coverage)' % (path, name))
    return failures


def check(decl, flavour, kmd_path, others, kmd_only=False, slots=None, blobs=None):
    """(failures, notes, record). slots: {path: payload path} from collect(); a path without one has only its file
    name. blobs: {path: bytes} read already; the others are read here, once."""
    if flavour not in ('release', 'lab'):
        raise GateError('flavour must be release or lab, not %r' % flavour)
    if not others and not kmd_only:
        raise GateError('no UMD/ICD PE to scan; pass --pe/--package, or --kmd-only for a KMD-only set')
    failures, notes = [], []
    k = decl['kmd']
    other = 'lab' if flavour == 'release' else 'release'
    blobs = {} if blobs is None else blobs
    for p in [kmd_path] + list(others):
        if p not in blobs:
            blobs[p] = read_pe(p)
    kmd = blobs[kmd_path]
    record = {'flavour': flavour, 'kmd_only': bool(kmd_only), 'files': []}

    if not has_text(kmd, k['flavour_markers'][flavour]):
        failures.append('%s: no %s flavour line %r' % (kmd_path, flavour, k['flavour_markers'][flavour]))
    if has_text(kmd, k['flavour_markers'][other]):
        failures.append('%s: carries the %s flavour line' % (kmd_path, other))
    for e in k['lab_escape_markers']:
        present = has_text(kmd, e['marker'])
        if flavour == 'release' and present:
            failures.append('%s: raw escape still compiled in: %s (marker %r, %s)'
                            % (kmd_path, e['escapes'], e['marker'], e['source']))
        if flavour == 'lab' and not present:
            failures.append('%s: lab KMD lacks the marker of %s (%r, %s): the marker list is stale or the '
                            'handler is gone' % (kmd_path, e['escapes'], e['marker'], e['source']))
    if flavour == 'release':
        for e in decl['allowed']:
            if e['reason'].startswith(OPEN_DECISION):
                failures.append('declaration: allowed %s is an open decision (%s): a release set needs every '
                                'decision closed' % (e['name'], e['reason']))

    slots = dict(slots or {})
    for p in [kmd_path] + list(others):
        slots.setdefault(p, pathlib.Path(p).name.lower())
    forbidden = [f['name'] for f in decl['forbidden']]
    out_of_scope = [f['name'] for f in decl.get('out_of_scope', [])]
    declared = set(forbidden) | set(out_of_scope) | {f['name'] for f in decl['allowed']}
    held = {}   # file -> set of forbidden names it holds
    for path in [kmd_path] + list(others):
        data = blobs[path]
        finding = scan_file(decl, data, forbidden, out_of_scope, declared)
        asserts = live_asserts(data)
        held[path] = set(finding['forbidden'])
        record['files'].append({'path': str(path), 'slot': slots[path], 'sha256': hashlib.sha256(data).hexdigest().upper(),
                                'forbidden': finding['forbidden'], 'out_of_scope': finding['out_of_scope'],
                                'undeclared': finding['undeclared'], 'asserts': asserts})
        failures += file_failures(decl, flavour, path, finding, data)
        if flavour == 'release':
            failures += ['%s: a live assert, it %s: the release flavour builds with NDEBUG' % (path, a) for a in asserts]
        for name in sorted(finding['out_of_scope']):
            notes.append('%s: %s (out of scope, recorded)' % (path, name))

    if flavour == 'lab' and not kmd_only:
        # A lab build that lost the switch has none of its lab-only names: each named file must be in the set, and
        # each copy of it must hold each name that names it. A path item names one copy; the other copies of that
        # file must not hold the name, or the list (and its note) no longer matches the build lines.
        scanned = [kmd_path] + list(others)

        def copies(item):
            return [p for p in scanned if (slots[p] if '/' in item else base(slots, p)) == item]

        for item in sorted({n for f in decl['forbidden'] for n in f['lab'].get('every', [])}):
            if not copies(item):
                failures.append('lab set has no %s%s, which a forbidden entry names in lab.every'
                                % ('file at ' if '/' in item else '', item))
        for f in decl['forbidden']:
            every = f['lab'].get('every')
            if not every:
                for p in scanned:
                    if f['name'] in held[p]:
                        failures.append('%s (%s): holds %s, but its entry says no lab file does (lab.none: %s): the '
                                        'declaration is stale' % (p, slots[p], f['name'], f['lab']['none']))
                continue
            for p in [p for item in every for p in copies(item)]:
                if f['name'] not in held[p]:
                    failures.append('%s: the lab build lacks %s (%s): it was built without the lab switch, or the '
                                    'lookup is gone and the declaration is stale' % (p, f['name'], f['where']))
            listed = {i.rsplit('/', 1)[1] for i in every if '/' in i}
            for p in scanned:
                if base(slots, p) in listed and slots[p] not in every and f['name'] in held[p]:
                    failures.append('%s (%s): holds %s, but lab.every lists only %s: the declaration is stale (%s)'
                                    % (p, slots[p], f['name'],
                                       ', '.join(i for i in every if i.endswith('/' + base(slots, p))),
                                       f['lab']['note']))
    record['failures'], record['notes'] = failures, notes
    return failures, notes, record


def bind_manifest(decl, manifest_path, package, record):
    """Failures of the scan against one package's manifest.json: every scanned PE is a manifest row (or the setup
    file) with the SHA256 of the scanned bytes, every PE row is scanned, and the scanned payload paths are the
    declaration's package slots, each once. Adds the binding to the record."""
    mp = pathlib.Path(manifest_path)
    try:
        raw = mp.read_bytes()
        m = json.loads(raw.decode('utf-8-sig'))
        rows = m['files']
        setup = m['setup']
        if not isinstance(rows, list) or not all(isinstance(r, dict) and isinstance(r.get('path'), str) and
                                                 isinstance(r.get('sha256'), str) for r in rows):
            raise ValueError('files rows need path and sha256')
        if not isinstance(setup, dict) or not isinstance(setup.get('file'), str) or not isinstance(setup.get('sha256'), str):
            raise ValueError('setup needs file and sha256')
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise GateError('manifest %s unreadable: %s' % (mp, e))
    top = pathlib.Path(package).resolve()
    root = mp.resolve().parent
    if top != root and top not in root.parents:
        raise GateError('manifest %s is not inside the package %s' % (mp, top))
    expected = {}
    for r in rows:
        f = (root / r['path']).resolve()
        if f.suffix.lower() in PE_SUFFIXES or (f.is_file() and starts_as_pe(f)):
            if f in expected:
                raise GateError('manifest %s: two rows for %s' % (mp, r['path']))
            expected[f] = (r['path'], r['sha256'].upper())
    s = (top / setup['file']).resolve()
    if s in expected:
        raise GateError('manifest %s: the setup file is also a files row' % mp)
    expected[s] = ('setup:' + setup['file'], setup['sha256'].upper())
    failures = []
    scanned = {pathlib.Path(f['path']).resolve(): f for f in record['files']}
    for f, (name, sha) in sorted(expected.items()):
        got = scanned.get(f)
        if got is None:
            failures.append('manifest row %s: not scanned (no file at %s, or it is outside --package)' % (name, f))
        elif got['sha256'] != sha:
            failures.append('manifest row %s: SHA256 %s, the scanned file %s' % (name, sha, got['sha256']))
        else:
            got['manifest'] = name
    for f, got in sorted(scanned.items()):
        if f not in expected:
            failures.append('%s: scanned, but no row of the manifest names it' % f)
    have = {}
    for got in record['files']:
        have.setdefault(got['slot'], []).append(got['path'])
    want = decl['package']['slots']
    for s_ in want:
        if s_ not in have:
            failures.append('package slot %s: no file of the package has this payload path' % s_)
    for s_, paths in sorted(have.items()):
        if s_ not in want:
            failures.append('%s: payload path %s is not a package slot of the declaration' % (', '.join(paths), s_))
        elif len(paths) > 1:
            failures.append('package slot %s: %d files (%s)' % (s_, len(paths), ', '.join(paths)))
    record['manifest'] = {'path': str(mp), 'sha256': hashlib.sha256(raw).hexdigest().upper(),
                          'pe_rows': len(expected), 'failures': len(failures)}
    return failures


def negative_control(decl, flavour, target, slots, blobs, record):
    """Failures of the in-memory negative control on the file at one payload path: a forbidden name and an
    undeclared prefixed name added to its scanned bytes must give the findings the real scan would give."""
    paths = [p for p in blobs if slots.get(p) == target]
    if len(paths) != 1:
        raise GateError('--negative-control %s: %d scanned files have this payload path' % (target, len(paths)))
    path = paths[0]
    forbidden = [f['name'] for f in decl['forbidden']]
    out_of_scope = [f['name'] for f in decl.get('out_of_scope', [])]
    declared = set(forbidden) | set(out_of_scope) | {f['name'] for f in decl['allowed']}
    if NEGATIVE_UNDECLARED in declared:
        raise GateError('the negative control name %s is declared' % NEGATIVE_UNDECLARED)
    name = NEGATIVE_FORBIDDEN if NEGATIVE_FORBIDDEN in forbidden else forbidden[0]
    data = (blobs[path] + b'\0' + name.encode('ascii') + b'\0\0' + NEGATIVE_UNDECLARED.encode('utf-16-le') +
            b'\0\0')
    finding = scan_file(decl, data, forbidden, out_of_scope, declared)
    got = file_failures(decl, flavour, path, finding, data)
    want = [NEGATIVE_UNDECLARED] + ([name] if flavour == 'release' else [])
    found = set(finding['undeclared']) | (set(finding['forbidden']) if flavour == 'release' else set())
    missed = [w for w in want if w not in found or not any(w in g for g in got)]
    record['negative_control'] = {'slot': target, 'path': str(path), 'injected': [name, NEGATIVE_UNDECLARED],
                                  'expected': want, 'reported': len(got), 'missed': missed}
    if missed:
        return ['negative control on %s: the scan did not report %s in bytes that hold it' % (target, ', '.join(missed))]
    return []


def file_sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest().upper()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--flavour', required=True, choices=('release', 'lab'))
    p.add_argument('--package', default='', help='directory: every PE under it is scanned')
    p.add_argument('--kmd', default='', help='the KMD .sys (default: the one file named as kmd.file in --package)')
    p.add_argument('--pe', action='append', default=[], help='a UMD/ICD PE file or a directory of them')
    p.add_argument('--kmd-only', action='store_true', help='a KMD-only set: no UMD/ICD PE is required')
    p.add_argument('--manifest', default='', help="the package's manifest.json: bind the scan to it (needs --package)")
    p.add_argument('--negative-control', default='', help='payload path of one scanned file for the negative control')
    p.add_argument('--declaration', default=str(DECLARATION))
    p.add_argument('--out', default='')
    a = p.parse_args(argv)
    try:
        decl = load_declaration(a.declaration)
        if not a.package and not a.kmd:
            raise GateError('pass --package or --kmd')
        if a.manifest and (not a.package or a.kmd or a.pe or a.kmd_only):
            raise GateError('--manifest judges one whole package: pass --package alone')
        if a.negative_control and a.kmd_only:
            raise GateError('--negative-control needs a UMD/ICD set, not --kmd-only')
        slots, blobs = {}, {}
        kmd, others = collect(a.package, a.kmd, a.pe, decl['kmd']['file'], slots)
        failures, notes, record = check(decl, a.flavour, kmd, others, a.kmd_only, slots, blobs)
        if a.manifest:
            failures += bind_manifest(decl, a.manifest, a.package, record)
            record['declaration_sha256'] = file_sha256(a.declaration)
            record['gate_sha256'] = file_sha256(__file__)
        if a.negative_control:
            failures += negative_control(decl, a.flavour, a.negative_control.lower(), slots, blobs, record)
        record['failures'] = failures
    except GateError as e:
        print('flavour gate: FAIL (closed): %s' % e)
        return 2
    for n in notes:
        print('note: ' + n)
    for f in failures:
        print('FAIL: ' + f)
    if a.out:
        out = pathlib.Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / 'flavour-gate.json').write_text(json.dumps(record, indent=1), encoding='utf-8')
    extra = ''
    if 'manifest' in record:
        extra += ', manifest bound (%d PE rows)' % record['manifest']['pe_rows']
    if 'negative_control' in record:
        extra += ', negative control on %s %s' % (record['negative_control']['slot'],
                                                 'missed' if record['negative_control']['missed'] else 'seen')
    print('flavour gate: %s, flavour %s, %d file(s)%s, %d failure(s)'
          % ('FAIL' if failures else 'PASS', a.flavour, len(record['files']), extra, len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
