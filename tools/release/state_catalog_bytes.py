"""The state catalog of a staged package, read from its bytes (BD-322).

Every state reader opens the store only with the catalog it was compiled with (driver/state/windows.c OpenState
refuses another one with AMDGPU_STATE_SCHEMA). A package whose desktop routers compile another catalog than its KMD
and amdgpu-state.bin sends DWM to the CPU route with no message (kit-cap-22631-r1c). provenance.py checks the source
inputs of a release, but a lab candidate built with -NoProvenance skips that gate. This check reads the bytes of the
staged payload, for every flavour and also with -NoProvenance:

- payload/kmd/amdgpu-state.bin holds exactly one catalog id: the catalog of the package.
- The known ids are that one and those of driver/state/catalog_history.generated.h and catalog.generated.h of the
  build tree. A PE file (.sys, .dll, .exe) that carries a known id must carry the catalog of the package. The state
  worker also carries older ids (its migration history); that is allowed next to the package catalog.
- Every named state reader that the payload holds must carry the catalog of the package, so a reader that lost the
  id from its bytes is a finding too.

The ids are in the binaries as 64 lowercase hex digits, in ASCII (AMDGPU_STATE_CATALOG_ID is a string literal); the
UTF-16 form is read too.

    python -I state_catalog_bytes.py --payload <package>/payload --repo <repository root>
"""
import argparse
import pathlib
import re
import sys

HEX64 = re.compile(rb'(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])')
PE_SUFFIXES = ('.sys', '.dll', '.exe')
# The binaries that compile a state catalog. A payload holds each name once or more (bc250control.dll is in two
# folders); each copy must carry the catalog of the package.
READERS = ('bc250kmd.sys', 'bc250kmd_cli.exe', 'amdgpu-state-installer.exe', 'bc250control.dll',
           'bc250d3d_router.dll', 'bc250d3d_router32.dll')
HISTORY_FILES = ('driver/state/catalog_history.generated.h', 'driver/state/catalog.generated.h')


def ids_in(data):
    """The 64-digit lowercase hex strings of data, in ASCII and in UTF-16LE."""
    found = {m.group(0).decode('ascii') for m in HEX64.finditer(data)}
    wide = data.replace(b'\x00', b'') if b'\x00' in data else b''
    if wide:
        for m in HEX64.finditer(wide):
            text = m.group(0).decode('ascii')
            if text.encode('utf-16-le') in data:
                found.add(text)
    return found


def known_ids(repo):
    ids = set()
    for rel in HISTORY_FILES:
        path = pathlib.Path(repo) / rel
        if not path.is_file():
            raise FileNotFoundError(f'{rel} is not in {repo}')
        ids |= ids_in(path.read_bytes())
    if not ids:
        raise ValueError(f'no catalog id in {", ".join(HISTORY_FILES)} of {repo}')
    return ids


def check(payload, known):
    """Return (catalog, findings, lines) for the staged payload directory."""
    payload = pathlib.Path(payload)
    findings, lines = [], []
    state_bin = payload / 'kmd' / 'amdgpu-state.bin'
    if not state_bin.is_file():
        return None, [f'{state_bin} is missing'], lines
    in_bin = ids_in(state_bin.read_bytes())
    if len(in_bin) != 1:
        return None, [f'payload/kmd/amdgpu-state.bin holds {len(in_bin)} catalog ids, not 1: {sorted(in_bin)}'], lines
    catalog = next(iter(in_bin))
    if catalog not in known:
        findings.append(f'the catalog {catalog} of amdgpu-state.bin is not in the catalog history of the build tree')
    known = set(known) | {catalog}
    readers_seen = {}
    for path in sorted(p for p in payload.rglob('*') if p.is_file() and p.suffix.lower() in PE_SUFFIXES):
        rel = path.relative_to(payload.parent).as_posix()
        ids = ids_in(path.read_bytes()) & known
        name = path.name.lower()
        if name in READERS:
            readers_seen[name] = readers_seen.get(name, 0) + 1
        if not ids and name not in READERS:
            continue
        has = catalog in ids
        others = sorted(ids - {catalog})
        lines.append(f'{"cat" if has else "BAD"} {rel}' +
                     (f' ({"also " if has else "only "}{", ".join(i[:8] for i in others)})' if others else ''))
        if not has:
            if ids:
                findings.append(f'{rel} compiles the catalog {", ".join(i[:8] for i in others)}, not {catalog[:8]} '
                                f'of amdgpu-state.bin: it cannot open the store')
            else:
                findings.append(f'{rel} is a state reader and carries no catalog id')
    for name in READERS:
        if name not in readers_seen:
            lines.append(f'note: no {name} in this payload')
    return catalog, findings, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--payload', required=True, help='the payload directory of the staged package')
    ap.add_argument('--repo', required=True, help='the repository root the package was built from')
    a = ap.parse_args(argv)
    try:
        known = known_ids(a.repo)
    except (OSError, ValueError) as e:
        print(f'state catalog bytes: FAIL: {e}')
        return 1
    catalog, findings, lines = check(a.payload, known)
    for line in lines:
        print(f'  {line}')
    if findings:
        for f in findings:
            print(f'FAIL {f}')
        print(f'state catalog bytes: FAIL, {len(findings)} finding(s)')
        return 1
    print(f'state catalog bytes: PASS, catalog {catalog}, {sum(1 for l in lines if l.startswith("cat "))} reader(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
