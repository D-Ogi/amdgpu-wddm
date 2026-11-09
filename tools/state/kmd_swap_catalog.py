"""The state catalog of a KMD image, for a lab kit that swaps the KMD image only (the host half of the swap check).

A KMD starts only on the state catalog that it compiles. DriverStateInit (driver/kmd/state_kernel.c) compares the
CatalogId of Parameters\\DefaultsV2, which the INF of the installed package writes, with AMDGPU_STATE_CATALOG_ID, and
refuses DriverEntry with STATUS_REVISION_MISMATCH (0xC0000365) on a difference. The active generation of StateV2 has
the same catalog, and the KMD refuses a generation of another catalog without the handoff of a package install
(docs/design/driver-state.md). So a kit that copies a KMD image over the installed one is valid only when the image
compiles the installed catalog. A catalog change needs the install of a package, which writes the new defaults and
moves the store to the new catalog in one transaction. A release KMD keeps this refusal: it never runs on a catalog
that it does not know. (A1 s3 of BD-223, 2026-11-07: candidate FE9BBDB6 on catalog 02c7eed4, installed catalog
b9c5c1b8, DriverEntry failed at both starts.)

Commands:

    image <sys> --repo <root>
        The catalog id that the KMD image compiles: the one known id (driver/state/catalog.generated.h and
        catalog_history.generated.h of <root>) in the bytes of the image. Exit 1 when there is not exactly one.
    pin <sys> --catalog <id> --repo <root>
        Exit 0 when the image compiles <id>. A kit freeze writes the catalog of its candidate into its pins file with
        this check, and the lab half (tools/win/kmd-swap/catalog-precheck.ps1) compares it with the installed one.
    delta --installed <id> --candidate <id> --repo <root>
        The setting names that differ between two catalogs of driver/state (catalog.json or history/<id>.json), for
        the refusal text of a kit. Exit 0 when the two ids are equal, 3 when they differ.

Exit 2 on a usage or input error.
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'release'))
from generate import identity_of  # noqa: E402
from state_catalog_bytes import ids_in, known_ids  # noqa: E402

ID = re.compile(r'^[0-9a-f]{64}$')


class Refusal(Exception):
    """An input that the check cannot accept (exit 1)."""


def image_catalog(sys_path, repo):
    """The one known catalog id that the bytes of sys_path carry."""
    data = pathlib.Path(sys_path).read_bytes()
    known = known_ids(repo)
    found = sorted(ids_in(data) & known)
    if len(found) != 1:
        raise Refusal(f'{sys_path} carries {len(found)} known catalog ids, not 1: '
                      f'{", ".join(i[:8] for i in found) or "none"}')
    return found[0]


def catalog_json(repo, catalog):
    """The catalog.json of id catalog: the current one of repo or a file of its history."""
    root = pathlib.Path(repo) / 'driver' / 'state'
    for path in [root / 'catalog.json', root / 'history' / f'{catalog}.json']:
        if path.is_file():
            data = json.loads(path.read_text(encoding='utf-8'))
            if identity_of(data) == catalog:
                return data
    raise Refusal(f'the catalog {catalog} is neither driver/state/catalog.json nor a file of driver/state/history')


def delta(repo, installed, candidate):
    """Lines that name each setting that the two catalogs declare differently."""
    a = {(v['path'], v['name']): v for v in catalog_json(repo, installed)['values']}
    b = {(v['path'], v['name']): v for v in catalog_json(repo, candidate)['values']}
    lines = []
    for key in sorted(a.keys() | b.keys()):
        name = '\\'.join(p for p in key if p)
        if key not in a:
            lines.append(f'only in the candidate: {name}')
        elif key not in b:
            lines.append(f'only in the installed catalog: {name}')
        elif a[key] != b[key]:
            fields = sorted(k for k in a[key].keys() | b[key].keys() if a[key].get(k) != b[key].get(k))
            lines.append(f'changed: {name} ({", ".join(fields)})')
    return lines


def checked_id(text, what):
    text = text.strip().lower()
    if not ID.match(text):
        raise argparse.ArgumentTypeError(f'{what} is not 64 hex digits: {text!r}')
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('image')
    p.add_argument('sys')
    p.add_argument('--repo', required=True)
    p = sub.add_parser('pin')
    p.add_argument('sys')
    p.add_argument('--catalog', required=True, type=lambda t: checked_id(t, '--catalog'))
    p.add_argument('--repo', required=True)
    p = sub.add_parser('delta')
    p.add_argument('--installed', required=True, type=lambda t: checked_id(t, '--installed'))
    p.add_argument('--candidate', required=True, type=lambda t: checked_id(t, '--candidate'))
    p.add_argument('--repo', required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'image':
            print(image_catalog(a.sys, a.repo))
            return 0
        if a.cmd == 'pin':
            found = image_catalog(a.sys, a.repo)
            if found != a.catalog:
                print(f'REFUSED: {a.sys} compiles the catalog {found}, not the pinned {a.catalog}')
                return 1
            print(f'CATALOG PIN MATCH {found}')
            return 0
        if a.installed == a.candidate:
            print(f'CATALOG MATCH {a.installed}')
            return 0
        print(f'CATALOG MISMATCH installed {a.installed} candidate {a.candidate}')
        for line in delta(a.repo, a.installed, a.candidate):
            print(f'  {line}')
        print('  A KMD-only swap cannot start on this install: build the candidate on the installed catalog, '
              'or install a package of the candidate.')
        return 3
    except Refusal as e:
        print(f'REFUSED: {e}')
        return 1
    except (OSError, ValueError) as e:
        print(f'ERROR: {e}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
