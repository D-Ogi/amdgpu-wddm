"""BD-167 flavour gate: no raw diagnostic escape and no forbidden override name in the released binaries.

    python flavour_gate.py --flavour release --package <dir> [--out <dir>]
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
    (lab.every), or the reason no lab file holds it (lab.none). An item of lab.every is a file name (every copy
    of that file) or a payload path such as d3d11/amdgpu_wddm_radv.dll (that copy only). A file's payload path
    is its path under the last directory named payload, else under the scanned directory, in lower case with
    forward slashes. Paths make the list exact: a copy of a file named by path items that is not listed must
    not hold the name, and lab.note says why those copies lack it (for example another build line);
  - the ALLOWED names, each with the reason it stays, and the OUT-OF-SCOPE names, each with the reason. Neither
    list fails a set; out-of-scope hits are recorded. A name in two lists is an error.

Coverage: every identifier of this driver's own prefixes (BC250_, BC250<letters or digits>_ such as BC250DXVK_,
and AMDGPU_WDDM_) that a scanned file holds, in ASCII or UTF-16LE, must be in one of the three lists. An
undeclared one fails either flavour, so a new lookup cannot ship without a decision. The search has no left
boundary, so that no neighbouring byte can hide a name: a longer identifier that only ends in such a name shows
up as that name and must be declared too.

--flavour release passes when the KMD carries the release line, not the lab line and no escape marker, no
scanned PE (the KMD included) carries a forbidden name as a whole word, in ASCII or UTF-16LE, and every prefixed
identifier is declared.
--flavour lab passes when the KMD carries the lab line, not the release line, and every escape marker, and every
scanned file named in a forbidden entry's lab.every carries that name, with at least one such file in the set (for a
path item: a file at that path), no unlisted copy of a file named by path items carries it, and every prefixed
identifier is declared. So a build that
lost the lab switch, a marker list that no longer matches the handlers, or a path list that no longer matches the
build lines, fails in the lab arm instead of passing in silence. --kmd-only checks the KMD alone. A lab set never passes as release and a release set never
passes as lab.

The gate fails closed: no flavour, a missing file, a file that is not a PE image, no KMD, two KMD files in a
package, no other PE without --kmd-only, a malformed declaration, an entry without a reason, or a name in two
lists. --out writes flavour-gate.json with the SHA256 of every scanned file and every finding.
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


class GateError(Exception):
    """The input cannot be judged: the gate fails closed."""


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
            names.append(e['name'])
        lists[kind] = names
    for a, b in (('forbidden', 'allowed'), ('forbidden', 'out_of_scope'), ('allowed', 'out_of_scope')):
        both = sorted(set(lists[a]) & set(lists[b]))
        if both:
            raise GateError('declaration: in both %s and %s: %s' % (a, b, ', '.join(both)))
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
            not all(isinstance(f, str) and f == f.lower() and '\\' not in f and ':' not in f and
                    all(p not in ('', '.', '..') for p in f.split('/')) and f.endswith(PE_SUFFIXES)
                    for f in files)):
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


def has_text(data, text):
    """The whole C string as it is stored, terminator included: ASCII or UTF-16LE. With the terminator, the
    marker of RUN_DCN is not found inside the marker of RUN_DCNFLIP."""
    return (text + '\0').encode('ascii') in data or (text + '\0').encode('utf-16-le') in data


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


def prefixed_names(data):
    """Every identifier of this driver's prefixes in a file, in ASCII or UTF-16LE (PREFIXED)."""
    names = {m.group(0) for m in PREFIXED.finditer(data.decode('latin-1'))}
    for run in UTF16_RUN.finditer(data):
        names.update(m.group(0) for m in PREFIXED.finditer(run.group(0).decode('utf-16-le')))
    return names


def slot(root, f):
    """The payload path of a file found under a scanned directory: its path under the last directory named payload,
    else under the scanned directory, in lower case with forward slashes."""
    parts = [p.lower() for p in f.relative_to(root).parts]
    if 'payload' in parts[:-1]:
        parts = parts[len(parts) - parts[::-1].index('payload', 1):]
    return '/'.join(parts)


def collect(package, kmd, pes, kmd_name, slots=None):
    """The KMD path and the other PE paths. Fails closed on an ambiguous or empty set. slots, when given, gets the
    payload path of each file: a file named on the command line has only its file name."""
    files = []
    slots = {} if slots is None else slots
    roots = ([package] if package else []) + list(pes or [])
    for root in roots:
        r = pathlib.Path(root)
        if r.is_dir():
            found = sorted(f for f in r.rglob('*') if f.is_file() and f.suffix.lower() in PE_SUFFIXES)
            if not found:
                raise GateError('%s: no PE file (%s) under it' % (r, '/'.join(PE_SUFFIXES)))
            files += found
            for f in found:
                slots.setdefault(f, slot(r, f))
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
            others.append(f)
    return k, others


def check(decl, flavour, kmd_path, others, kmd_only=False, slots=None):
    """(failures, notes, record). slots: {path: payload path} from collect(); a path without one has only its file
    name."""
    if flavour not in ('release', 'lab'):
        raise GateError('flavour must be release or lab, not %r' % flavour)
    if not others and not kmd_only:
        raise GateError('no UMD/ICD PE to scan; pass --pe/--package, or --kmd-only for a KMD-only set')
    failures, notes = [], []
    k = decl['kmd']
    other = 'lab' if flavour == 'release' else 'release'
    kmd = read_pe(kmd_path)
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

    slots = dict(slots or {})
    for p in [kmd_path] + list(others):
        slots.setdefault(p, pathlib.Path(p).name.lower())
    forbidden = [f['name'] for f in decl['forbidden']]
    out_of_scope = [f['name'] for f in decl.get('out_of_scope', [])]
    declared = set(forbidden) | set(out_of_scope) | {f['name'] for f in decl['allowed']}
    held = {}   # file -> set of forbidden names it holds
    for path in [kmd_path] + list(others):
        data = kmd if path == kmd_path else read_pe(path)
        hits, outside = {}, {}
        for names, into in ((forbidden, hits), (out_of_scope, outside)):
            for name in names:
                a, u = has_name(data, name)
                if a or u:
                    into[name] = {'ascii': a, 'utf16': u}
        held[path] = set(hits)
        undeclared = sorted(prefixed_names(data) - declared)
        record['files'].append({'path': str(path), 'slot': slots[path], 'sha256': hashlib.sha256(data).hexdigest().upper(),
                                'forbidden': hits, 'out_of_scope': outside, 'undeclared': undeclared})
        if flavour == 'release':
            for name in sorted(hits):
                failures.append('%s: %s (ascii %d, utf16 %d)' % (path, name, hits[name]['ascii'], hits[name]['utf16']))
        for name in undeclared:
            failures.append('%s: %s is not declared: add it to the forbidden, allowed or out_of_scope list of the '
                            'declaration (BD-167 coverage)' % (path, name))
        for name in sorted(outside):
            notes.append('%s: %s (out of scope, recorded)' % (path, name))

    if flavour == 'lab' and not kmd_only:
        # A lab build that lost the switch has none of its lab-only names: each named file must be in the set, and
        # each copy of it must hold each name that names it. A path item names one copy; the other copies of that
        # file must not hold the name, or the list (and its note) no longer matches the build lines.
        scanned = [kmd_path] + list(others)

        def copies(item):
            return [p for p in scanned if (slots[p] if '/' in item else pathlib.Path(p).name.lower()) == item]

        for item in sorted({n for f in decl['forbidden'] for n in f['lab'].get('every', [])}):
            if not copies(item):
                failures.append('lab set has no %s%s, which a forbidden entry names in lab.every'
                                % ('file at ' if '/' in item else '', item))
        for f in decl['forbidden']:
            every = f['lab'].get('every')
            if not every:
                continue
            for p in [p for item in every for p in copies(item)]:
                if f['name'] not in held[p]:
                    failures.append('%s: the lab build lacks %s (%s): it was built without the lab switch, or the '
                                    'lookup is gone and the declaration is stale' % (p, f['name'], f['where']))
            listed = {i.rsplit('/', 1)[1] for i in every if '/' in i}
            for p in scanned:
                if pathlib.Path(p).name.lower() in listed and slots[p] not in every and f['name'] in held[p]:
                    failures.append('%s (%s): holds %s, but lab.every lists only %s: the declaration is stale (%s)'
                                    % (p, slots[p], f['name'],
                                       ', '.join(i for i in every if i.endswith('/' + pathlib.Path(p).name.lower())),
                                       f['lab']['note']))
    record['failures'], record['notes'] = failures, notes
    return failures, notes, record


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--flavour', required=True, choices=('release', 'lab'))
    p.add_argument('--package', default='', help='directory: every .dll/.sys/.exe under it is scanned')
    p.add_argument('--kmd', default='', help='the KMD .sys (default: the one file named as kmd.file in --package)')
    p.add_argument('--pe', action='append', default=[], help='a UMD/ICD PE file or a directory of them')
    p.add_argument('--kmd-only', action='store_true', help='a KMD-only set: no UMD/ICD PE is required')
    p.add_argument('--declaration', default=str(DECLARATION))
    p.add_argument('--out', default='')
    a = p.parse_args(argv)
    try:
        decl = load_declaration(a.declaration)
        if not a.package and not a.kmd:
            raise GateError('pass --package or --kmd')
        slots = {}
        kmd, others = collect(a.package, a.kmd, a.pe, decl['kmd']['file'], slots)
        failures, notes, record = check(decl, a.flavour, kmd, others, a.kmd_only, slots)
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
    print('flavour gate: %s, flavour %s, %d file(s), %d failure(s)'
          % ('FAIL' if failures else 'PASS', a.flavour, len(record['files']), len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
