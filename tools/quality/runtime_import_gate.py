"""BD-389 runtime import gate: no PE file of a release imports a C or C++ runtime DLL that Windows does not ship.

    python runtime_import_gate.py --scan <dir|file> [--scan ...] [--out <dir>]
    python runtime_import_gate.py --scan <package top> --negative-control <path under it> [--out <dir>]

tools/release/build-release.ps1 runs it on the whole unpacked package (setup.exe and files\\, the driver package
files\\payload\\kmd with its wow64 tree included), with the negative control. docs/build.md ("C runtime imports")
gives the rule, the reason and the sources.

The rule, per DLL name in the import directory and in the delay-import directory of every scanned PE file, in any
letter case (the Windows loader compares DLL names without case):
  - REFUSED: a DLL of the Visual C++ runtime libraries that an application must deploy itself (the Visual C++
    Redistributable, central or local): vcruntime*, msvcp<version>*, msvcr<version>*, concrt*, vccorlib*, vcomp*,
    vcamp*, mfc<version>* and mfcm<version>*, and the debug runtimes, which may not be redistributed at all
    (ucrtbased.dll and every *d.dll form of the names above). A clean Windows does not have them, and a module
    that imports one does not load there (BD-389: dwm.exe could not start on our device);
  - ALLOWED: the Universal CRT, ucrtbase.dll and the api-ms-win-crt-*.dll API sets, a component of Windows 10 and
    later;
  - every other DLL name is outside this rule.
The Windows DLLs whose names start like a refused family stay outside it: msvcrt.dll (no version digits),
msvcp60.dll and msvcp_win.dll (a version below 70 or no version), mfc42.dll and mfc42u.dll (a version below 100).

A directory scan takes every file with a PE suffix (.dll, .sys, .exe) and every other file whose first 4 KiB hold
the MZ header and the PE signature (a renamed PE). The gate fails closed: no PE file, a file with a PE suffix that is not a PE image, or an import table that
cannot be read. --negative-control <path> rewrites, in memory, the name of one import of that file (the first one
of 12 or more characters) to MSVCP140.dll, and the per-file check must refuse it; a check that does not refuse it
fails the set. --out writes runtime-import-gate.json with the SHA256 of this gate, and of every scanned file with
its CRT imports and findings.

Exit code: 0 pass, 1 a refused import or a failed negative control, 2 the input cannot be judged.
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pe_image  # noqa: E402

PE_SUFFIXES = ('.dll', '.sys', '.exe')
# The families of the Visual C++ runtime libraries (docs/build.md, "C runtime imports"), as full DLL names in
# lower case. <version> is two or three digits; a name may go on after it (vcruntime140_1.dll,
# msvcp140_atomic_wait.dll, msvcp140d.dll).
REFUSED = (
    ('vcruntime', re.compile(r'vcruntime\d{2,3}[a-z0-9_]*\.dll')),
    ('msvcp', re.compile(r'msvcp(?:[7-9]\d|\d{3})[a-z0-9_]*\.dll')),
    ('msvcr', re.compile(r'msvcr(?:[7-9]\d|\d{3})[a-z0-9_]*\.dll')),
    ('concrt', re.compile(r'concrt\d{2,3}[a-z0-9_]*\.dll')),
    ('vccorlib', re.compile(r'vccorlib\d{2,3}[a-z0-9_]*\.dll')),
    ('vcomp', re.compile(r'vcomp\d{2,3}[a-z0-9_]*\.dll')),
    ('vcamp', re.compile(r'vcamp\d{2,3}[a-z0-9_]*\.dll')),
    ('mfc', re.compile(r'mfcm?\d{3}[a-z0-9_]*\.dll|mfcmifc80\.dll')),
    ('debug ucrt', re.compile(r'ucrtbased\.dll')),
)
UCRT = re.compile(r'ucrtbase\.dll|api-ms-win-crt-[a-z0-9-]+\.dll')
NEGATIVE_NAME = 'MSVCP140.dll'


class GateError(Exception):
    """The input cannot be judged: the gate fails closed."""


def classify(dll):
    """('refused', family), ('ucrt', None) or (None, None) for one DLL name of an import table."""
    name = dll.lower()
    for family, pattern in REFUSED:
        if pattern.fullmatch(name):
            return 'refused', family
    if UCRT.fullmatch(name):
        return 'ucrt', None
    return None, None


def judge(data):
    """The record of one PE image: its architecture, its UCRT imports and one finding per refused import."""
    try:
        tables = pe_image.import_dlls(data)
        arch = pe_image.architecture(data)
    except pe_image.PeError as e:
        raise GateError(str(e))
    record = {'arch': arch, 'ucrt': [], 'findings': []}
    for table, kind in (('import', 'imports'), ('delay', 'delay-imports')):
        for dll in tables[table]:
            verdict, family = classify(dll)
            if verdict == 'refused':
                record['findings'].append('%s %s (%s: a Visual C++ runtime DLL that Windows does not ship)'
                                          % (kind, dll, family))
            elif verdict == 'ucrt':
                record['ucrt'].append(dll)
    return record


def negative_control(data):
    """The image with the first import name of 12 or more characters rewritten to NEGATIVE_NAME, in memory."""
    try:
        names = pe_image.name_offsets(data)
    except pe_image.PeError as e:
        raise GateError(str(e))
    for dll, at in names:
        if len(dll) >= len(NEGATIVE_NAME):
            out = bytearray(data)
            out[at:at + len(dll)] = NEGATIVE_NAME.encode('ascii') + b'\0' * (len(dll) - len(NEGATIVE_NAME))
            return bytes(out), dll
    raise GateError('the negative control file has no import name of %d or more characters' % len(NEGATIVE_NAME))


def is_pe_candidate(path):
    """A file with a PE suffix, or another file whose first 4 KiB hold the MZ header and the PE signature. A data
    file that only starts with the two letters MZ (a firmware image, for example) is not a PE image."""
    if path.suffix.lower() in PE_SUFFIXES:
        return True
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
    except OSError as e:
        raise GateError('%s: unreadable: %s' % (path, e))
    try:
        pe_image.check(head)
    except pe_image.PeError:
        return False
    return True


def collect(scans):
    """[(label, path)] of every PE candidate under the scanned paths, the label relative to its scan root."""
    found = []
    for scan in scans:
        root = pathlib.Path(scan)
        if root.is_file():
            found.append((root.name, root))
        elif root.is_dir():
            for p in sorted(root.rglob('*')):
                if p.is_file() and is_pe_candidate(p):
                    found.append((p.relative_to(root).as_posix(), p))
        else:
            raise GateError('%s: no such file or directory' % root)
    return found


def sha256(data):
    return hashlib.sha256(data).hexdigest().upper()


def run(scans, control=None):
    """(report, refused) for the scanned paths; GateError when the input cannot be judged."""
    files = collect(scans)
    if not files:
        raise GateError('no PE file under %s' % ', '.join(str(s) for s in scans))
    report = {'gate_sha256': sha256(pathlib.Path(__file__).read_bytes()), 'files': [], 'negative_control': None}
    refused = 0
    blobs = {}
    for label, path in files:
        try:
            data = pe_image.read(path)
        except pe_image.PeError as e:
            raise GateError(str(e))
        blobs[label] = data
        record = dict(path=label, sha256=sha256(data), **judge(data))
        report['files'].append(record)
        refused += bool(record['findings'])
    if control is not None:
        key = pathlib.PurePosixPath(str(control).replace('\\', '/')).as_posix()
        if key not in blobs:
            raise GateError('the negative control %s is not a scanned PE file' % key)
        mutated, original = negative_control(blobs[key])
        findings = judge(mutated)['findings']
        seen = any(NEGATIVE_NAME in f for f in findings)
        report['negative_control'] = {'path': key, 'rewritten': original, 'to': NEGATIVE_NAME, 'refused': seen}
        if not seen:
            refused += 1
    return report, refused


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--scan', action='append', required=True, help='a directory or a PE file')
    ap.add_argument('--negative-control', help='a scanned file, by its path under its --scan directory')
    ap.add_argument('--out', help='directory for runtime-import-gate.json')
    args = ap.parse_args(argv)
    try:
        if args.negative_control and len(args.scan) != 1:
            raise GateError('--negative-control takes exactly one --scan directory')
        report, refused = run(args.scan, args.negative_control)
    except GateError as e:
        print('runtime import gate: CANNOT JUDGE: %s' % e)
        return 2
    for f in report['files']:
        for finding in f['findings']:
            print('REFUSE %s: %s' % (f['path'], finding))
    nc = report['negative_control']
    if nc and not nc['refused']:
        print('REFUSE negative control %s: the import %s rewritten to %s was not refused' % (nc['path'], nc['rewritten'], nc['to']))
    if args.out:
        out = pathlib.Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / 'runtime-import-gate.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    bad = sum(1 for f in report['files'] if f['findings'])
    ucrt = sum(1 for f in report['files'] if f['ucrt'])
    print('runtime import gate: %s, %d PE file(s), %d refused, %d with UCRT imports%s'
          % ('FAIL' if refused else 'pass', len(report['files']), bad, ucrt,
             ', negative control refused (%s)' % nc['path'] if nc and nc['refused'] else ''))
    return 1 if refused else 0


if __name__ == '__main__':
    sys.exit(main())
