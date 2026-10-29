"""Keep the commits of docs/testing/THIRD-PARTY.md equal to the release source pins.

The document names, for each payload file, the commit that built it and the commits of the
submodules in that build. tools/release/release-sources.json (written by build-release-set.ps1
-Register from the release manifest) records the commit of every payload file. This tool compares
the two and fails on each difference:

- a file of the package that no table row names, or a table row file that the package does not have;
- a row that does not name the commit that built its files (both flavours);
- a commit in a row that is not the build commit of its files, an input of that build, a submodule
  commit of that build (git gitlinks, recursive, in the workspace checkouts) or a licence or firmware
  pin of release-sources.json;
- a submodule commit of a built engine that the document does not name (unless the build does not
  use that submodule: NOT_BUILT below);
- a short hash that is not the prefix of one of these commits;
- a branch name: the pins are commits, and a branch moves;
- a licence text of a repository that builds payload files, when it is not the same file at every
  build commit of that repository in the release.

--write replaces each stale commit of a cell by the missing commit of the same repository, when there
is exactly one, and then checks again. Run it after -Register, then read the diff.
"""
import argparse
import functools
import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_flavour import select_sources

FULL = re.compile(r'(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])')
SHORT = re.compile(r'`([0-9a-f]{7,39})`')
TICK = re.compile(r'`([^`]+)`')
BRANCH = re.compile(r'\bbranch(es)? `')
FILE_HEADER = 'File (`payload\\`)'

# Submodules that the release build of the parent repository does not compile or include.
NOT_BUILT = {
    ('dxvk', 'include/native/directx'): 'the DirectX headers of native (non-Windows) DXVK builds',
    ('dxil-spirv', 'third_party/SPIRV-Cross'): 'not in the dxil-spirv meson build',
    ('dxil-spirv', 'third_party/SPIRV-Tools'): 'not in the dxil-spirv meson build',
}


def git(checkout, *args):
    result = subprocess.run(['git', '-C', str(checkout), *args], capture_output=True, text=True,
                            encoding='utf-8', errors='replace')
    return result.stdout if result.returncode == 0 else None


def url_key(url):
    url = url.strip().lower()
    if url.endswith('.git'):
        url = url[:-4]
    return url.rstrip('/').split('://')[-1]


class Repos:
    """The checkouts of the release repositories, and the commits they hold."""

    def __init__(self, repositories, repo_root, workspace):
        self.dirs, self.by_url, self.submodules = {}, {}, {}
        for name, entry in repositories.items():
            dirs = []
            for item in entry.get('checkouts', []):
                path = Path(repo_root) if item == '{repo}' else Path(workspace) / item
                if (path / '.git').exists():
                    dirs.append(path)
            self.dirs[name] = dirs
            if entry.get('url'):
                self.by_url[url_key(entry['url'])] = name
            self.submodules[name] = entry.get('submodules', {})
        self.cache = {}

    def checkout(self, name, commit):
        for path in self.dirs.get(name, []):
            key = (path, commit)
            if key not in self.cache:
                self.cache[key] = git(path, 'cat-file', '-e', f'{commit}^{{commit}}') is not None
            if self.cache[key]:
                return path
        return None

    def owner(self, commit):
        for name in self.dirs:
            if self.checkout(name, commit):
                return name
        return None

    def gitlinks(self, name, commit, seen=None):
        """[(parent repo, path, url, commit, repo or None)] of a build, recursive."""
        seen = set() if seen is None else seen
        if (name, commit) in seen:
            return []
        seen.add((name, commit))
        path = self.checkout(name, commit)
        if path is None:
            raise LookupError(f'{name} {commit[:8]}: no workspace checkout holds this commit')
        modules = {}
        text = git(path, 'show', f'{commit}:.gitmodules') or ''
        current = None
        for line in text.splitlines():
            line = line.strip()
            if line.startswith('path'):
                current = line.split('=', 1)[1].strip()
            elif line.startswith('url') and current:
                modules[current] = line.split('=', 1)[1].strip()
        found = []
        for line in (git(path, 'ls-tree', '-r', commit) or '').splitlines():
            meta, _, sub_path = line.partition('\t')
            mode, kind, sub = meta.split()
            if kind != 'commit' or (name, sub_path) in NOT_BUILT:
                continue
            url = modules.get(sub_path, '')
            sub_repo = self.submodules.get(name, {}).get(sub_path) or self.by_url.get(url_key(url))
            if sub_repo is None:
                tail = url_key(url).rsplit('/', 1)[-1]
                sub_repo = next((repo for repo in self.dirs if repo.lower() == tail), None)
            if sub_repo is not None and self.checkout(sub_repo, sub) is None:
                sub_repo = None
            found.append((name, sub_path, url, sub, sub_repo))
            if sub_repo is not None:
                found.extend(self.gitlinks(sub_repo, sub, seen))
        return found


def parse_tables(lines):
    """Rows of the payload tables: (line index, files, text). A row with no file continues the row above."""
    rows, in_table = [], False
    for index, line in enumerate(lines):
        if not line.startswith('|'):
            in_table = False
            continue
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if cells and cells[0] == FILE_HEADER:
            in_table = True
            continue
        if not in_table or set(line.replace('|', '').strip()) <= {'-'}:
            continue
        files = [tick for tick in TICK.findall(cells[0])]
        rows.append({'line': index, 'files': files, 'text': ' | '.join(cells[1:]), 'cells': cells})
    return rows


def payload_path(token):
    return 'payload/' + token.replace('\\', '/')


def check(doc_path, sources, repo_root, workspace, write=False):
    lines = Path(doc_path).read_text(encoding='utf-8').splitlines()
    errors, notes = [], []
    repos = Repos(sources.get('repositories', {}), repo_root, workspace)

    built, inputs = {}, {}
    for flavour in ('release', 'lab'):
        for row in select_sources(sources, flavour)['files']:
            path = row['path'].casefold()
            built.setdefault(path, set())
            inputs.setdefault(path, set())
            origin = row.get('built_from')
            if origin and origin.get('commit'):
                built[path].add((origin['repo'], origin['commit']))
            for value in (origin or {}).get('inputs', {}).values():
                if isinstance(value, dict) and value.get('commit'):
                    inputs[path].add((value.get('repo'), value['commit']))

    pins = set()
    for licence in sources.get('licenses', []):
        pins.update(FULL.findall(str(licence.get('ref', ''))))
    pins.update(FULL.findall(json.dumps(sources.get('firmware', {}))))

    links = {}
    for commits in built.values():
        for name, commit in commits:
            if (name, commit) not in links and repos.submodules.get(name) is not None:
                try:
                    links[(name, commit)] = repos.gitlinks(name, commit)
                except LookupError as error:
                    errors.append(str(error))
                    links[(name, commit)] = []

    rows = parse_tables(lines)
    named = set()
    groups, parent = [], None
    for row in rows:
        if row['files']:
            parent = {'row': row, 'more': []}
            groups.append(parent)
        elif parent is not None:
            parent['more'].append(row)
        else:
            errors.append(f"line {row['line'] + 1}: a continuation row with no file row above it")
    everything = set(pins)
    for commits in list(built.values()) + list(inputs.values()):
        everything.update(commit for _, commit in commits)
    for found in links.values():
        everything.update(link[3] for link in found)

    replacements, file_commits = {}, {}
    for group in groups:
        row = group['row']
        paths = []
        for token in row['files']:
            if '*' in token:
                continue
            path = payload_path(token).casefold()
            if path in built:
                paths.append(path)
                named.add(path)
            elif re.search(r'\.(dll|exe|sys|json|config|bin|inf)$', token, re.I):
                errors.append(f"line {row['line'] + 1}: `{token}` is not a file of the package")
        own = set().union(*(built[path] for path in paths)) if paths else set()
        own_inputs = set().union(*(inputs[path] for path in paths)) if paths else set()
        sub = set()
        for key in own:
            sub.update((link[4] or link[2], link[3]) for link in links.get(key, []))
        allowed = {c for _, c in own} | {c for _, c in own_inputs} | {c for _, c in sub} | pins
        cells = [(row, {c for _, c in own}, {c for _, c in own_inputs})]
        cells += [(more, set(), set()) for more in group['more']]
        group_commits = set()
        for cell_row, required, optional in cells:
            present = set(FULL.findall(cell_row['text']))
            group_commits |= present
            if cell_row is row:
                for path in paths:
                    file_commits.setdefault(path, {'line': row['line'], 'named': set()})['named'] |= present
            stale = present - allowed
            missing = required - present
            for commit in sorted(stale):
                errors.append(f"line {cell_row['line'] + 1}: {commit[:8]} is not a commit of this build")
            if write and stale:
                candidates = (missing | optional | {c for _, c in sub}) - present
                for commit in stale:
                    owner = repos.owner(commit)
                    same = [c for c in candidates if owner and repos.owner(c) == owner]
                    if len(same) == 1:
                        replacements.setdefault(cell_row['line'], {})[commit] = same[0]
                    else:
                        notes.append(f"line {cell_row['line'] + 1}: no single replacement for {commit[:8]}")
        if group['more'] or any(links.get(key) for key in own):
            needed = {(link[1], link[3]) for key in own for link in links.get(key, [])}
            group_text = ' '.join([row['text']] + [more['text'] for more in group['more']])
            reference = re.search(r'commits? of `([^`]+)`', group_text)
            if needed and not group['more'] and reference:
                continue
            for sub_path, commit in sorted(needed):
                if commit not in group_commits:
                    errors.append(f"line {row['line'] + 1}: does not name submodule {sub_path} @ {commit[:8]}")

    for path in sorted(set(built) - named):
        errors.append(f'{path}: no row of the payload tables names this file')
    # A file can have more than one row (our code and the third-party code in it): one of them names each commit.
    for path, seen in sorted(file_commits.items()):
        for commit in sorted({c for _, c in built[path]} - seen['named']):
            errors.append(f"line {seen['line'] + 1}: no row of {path} names {commit[:8]}, the build commit of the file")

    for index, line in enumerate(lines):
        if BRANCH.search(line):
            errors.append(f'line {index + 1}: names a branch; name the commit only')
        for short in SHORT.findall(line):
            if any(c.isdigit() for c in short) and not any(full.startswith(short) for full in everything):
                errors.append(f'line {index + 1}: `{short}` is not the prefix of a commit of this build')

    for licence in sources.get('licenses', []):
        name = repos.by_url.get(url_key('github.com/' + str(licence.get('repository', ''))))
        ref, path = licence.get('ref'), licence.get('path')
        if name is None or not path or ref is None:
            continue
        commits = {c for values in built.values() for repo, c in values if repo == name}
        if not commits:
            continue
        base = repos.checkout(name, ref)
        if base is None:
            errors.append(f"licence {licence['file']}: no checkout of {name} holds {ref[:8]}")
            continue
        want = (git(base, 'rev-parse', f'{ref}:{path}') or '').strip()
        for commit in sorted(commits):
            where = repos.checkout(name, commit)
            have = (git(where, 'rev-parse', f'{commit}:{path}') or '').strip() if where else ''
            if have != want:
                errors.append(f"licence {licence['file']}: {name}/{path} at {commit[:8]} is not the text of {ref[:8]}")

    if write and replacements:
        short = {}
        for index, pairs in replacements.items():
            for old, new in pairs.items():
                lines[index] = lines[index].replace(old, new)
                short[f'`{old[:8]}`'] = f'`{new[:8]}`'
        # The short form of a replaced commit, in the text below the tables.
        lines = [functools.reduce(lambda text, pair: text.replace(*pair), short.items(), line) for line in lines]
        Path(doc_path).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
        errors, later, count = check(doc_path, sources, repo_root, workspace, write=False)
        return errors, notes + later, count
    return errors, notes, len(groups)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    parser.add_argument('--repo', type=Path, default=repo)
    parser.add_argument('--workspace', type=Path,
                        default=Path(os.environ['BC250_ROOT']) if os.environ.get('BC250_ROOT') else repo.parent)
    parser.add_argument('--doc', type=Path)
    parser.add_argument('--sources', type=Path)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    doc = args.doc or args.repo / 'docs/testing/THIRD-PARTY.md'
    sources = json.loads((args.sources or args.repo / 'tools/release/release-sources.json').read_text(encoding='utf-8'))
    errors, notes, count = check(doc, sources, args.repo, args.workspace, args.write)
    for note in notes:
        print(f'NOTE {note}')
    for error in errors:
        print(f'FAIL {error}')
    print(f"third-party-doc {'FAIL' if errors else 'PASS'}: {count} file rows, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
