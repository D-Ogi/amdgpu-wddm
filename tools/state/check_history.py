"""The state catalogs of the released packages against driver/state/history (the 'driver-state-history' gate of
quick.ps1 on the tree, and the 'state-history' gate of build_release_set.py on each state worker row).

The uninstall of a release leaves its final driver state in StateV2 under the state catalog of that release. The
installer worker of a later release reads such a record by setting name only when driver/state/history holds the
catalog.json of that release (catalog_history.generated.h); a record of any other catalog moves aside, and the
user loses the settings (BD-262). This gate reads one commit of the repository, through git only:

- every file driver/state/history/<id>.json at the commit is named by the catalog identity of its contents (the
  SHA-256 of the canonical JSON, as tools/state/generate.py makes it);
- for every published tester tag (git tag v*-tester.*) with a version below the release version, the catalog
  identity of driver/state/catalog.json at the tag is the catalog at the commit or a file of the history. A user
  can uninstall any of those releases before the install of this one.

Without --version the gate checks every published tester tag: a tree is the source of the next release, and that
release is above every published one. The tree gate of quick.ps1 runs this way, so a catalog change after a release
fails at once, not only at the build of the next release (BD-229: the tester.35 catalog was missing for two days).
build_release_set.py passes the version of each row.

No tag below the release version is a failure: a checkout without its tags cannot prove the history. A tag without
driver/state/catalog.json is from before the state store and needs no history. Exit 1 on any failure.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate import identity_of  # noqa: E402

HISTORY = 'driver/state/history'
CATALOG = 'driver/state/catalog.json'
TAG = re.compile(r'^v(\d+)\.(\d+)\.(\d+)\.(\d+)-tester\.\S+$')
NAME = re.compile(r'^[0-9a-f]{64}\.json$')


def git(repo, *args):
    r = subprocess.run(['git', '-C', str(repo)] + list(args), capture_output=True, text=True, encoding='utf-8')
    return r.returncode, r.stdout


def numbers(version):
    """The four numbers at the start of a package version ("0.7.216.106-tester.34" -> (0, 7, 216, 106))."""
    m = re.match(r'^v?(\d+)\.(\d+)\.(\d+)\.(\d+)(?:-|$)', version)
    if not m:
        raise ValueError(f'not a package version: {version!r}')
    return tuple(int(x) for x in m.groups())


def catalog_at(repo, rev):
    """The catalog identity of driver/state/catalog.json at a revision, or None when the file is not there."""
    code, text = git(repo, 'show', f'{rev}:{CATALOG}')
    return identity_of(json.loads(text)) if code == 0 else None


def check(repo, commit, version):
    """(failures, lines) for one commit of the repository and the version of the release that builds it. A version
    of None is the next release: every published tester tag is below it."""
    failures, lines = [], []
    code, text = git(repo, 'rev-parse', '--verify', f'{commit}^{{commit}}')
    if code:
        return [f'{commit} is not a commit of {repo}'], lines
    commit = text.strip()
    current = catalog_at(repo, commit)
    if current is None:
        return [f'{commit[:8]} has no {CATALOG}'], lines
    code, text = git(repo, 'ls-tree', '--name-only', f'{commit}:{HISTORY}')
    names = text.split() if code == 0 else []
    history = set()
    for n in names:
        if not NAME.match(n):
            failures.append(f'{HISTORY}/{n}: the name is not <64 hexadecimal digits>.json')
            continue
        code, body = git(repo, 'show', f'{commit}:{HISTORY}/{n}')
        identity = identity_of(json.loads(body)) if code == 0 else None
        if identity != n[:-5]:
            failures.append(f'{HISTORY}/{n}: the catalog identity of its contents is {identity}')
        else:
            history.add(identity)
    lines.append(f'{commit[:8]}: catalog {current[:8]}, history {", ".join(sorted(h[:8] for h in history)) or "empty"}')
    below = numbers(version) if version is not None else None
    code, text = git(repo, 'tag', '--list', 'v*-tester.*')
    tags = sorted((t for t in text.split() if TAG.match(t) and (below is None or numbers(t) < below)),
                  key=lambda t: (numbers(t), t))
    if not tags:
        failures.append(f'no tag v*-tester.* below {version or "the next release"} in {repo}: fetch the tags, '
                        'then run again')
        return failures, lines
    without = []
    for tag in tags:
        identity = catalog_at(repo, tag)
        if identity is None:
            without.append(tag)
        elif identity == current:
            lines.append(f'{tag}: catalog {identity[:8]} is the current catalog')
        elif identity in history:
            lines.append(f'{tag}: catalog {identity[:8]} is in {HISTORY}')
        else:
            failures.append(f'{tag}: catalog {identity} is not in {HISTORY} of {commit[:8]}: add the catalog.json of '
                            f'that tag as {HISTORY}/{identity}.json and run tools/state/generate.py')
    if without:
        lines.append(f'{len(without)} tags without {CATALOG} (before the state store), nothing to read: '
                     f'{without[0]} to {without[-1]}')
    return failures, lines


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--repo', default=str(Path(__file__).resolve().parents[2]))
    p.add_argument('--commit', default='HEAD')
    p.add_argument('--version', help='the version of the release; default: the next release, above every '
                   'published tester tag')
    a = p.parse_args()
    if a.version:
        numbers(a.version)
    failures, lines = check(a.repo, a.commit, a.version or None)
    for line in lines:
        print(line)
    for f in failures:
        print('FAIL ' + f)
    print(f'state-history {a.version or "next release"}: {"FAIL" if failures else "PASS"}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
