#!/usr/bin/env python3
"""Provenance gate of the release payload: every file says where it comes from, and a machine checks it.

Each entry of `files` in release-sources.json carries exactly one of:

  built_from   {"repo", "commit", "recipe", "args", "output", ...} - a build of a named commit of a repository
               in `repositories`, by a recipe of the recipe repository (this one) at "recipe_commit" (default:
               "commit", when the build repository is the recipe repository). rebuild-check.ps1 runs the build.
  built_from   {"repo", "commit", "path"} - a data file: the blob at that path of that commit, as a checkout
               writes it (`git cat-file --filters`). This gate compares the bytes itself.
  third_party  {"url", "version", "sha256"} - a binary of another project, as downloaded.

An entry that cannot be tied to its commit by a bit-identical rebuild says so in "unverified": "<why>". The gate
accepts it only with a reason, and prints the count.

The top level names the repositories: "recipe_repository" (this repository's name in the table) and
"repositories": {name: {"url", "checkouts": [paths under BC250_ROOT, or "{repo}" for this checkout],
"submodules": {path: repository name}}}. A build input is {"repo", "commit", "paths"} (those paths of the commit),
{"repo", "commit"} (a work tree of the commit with its submodules), {"inline": {...}} (a JSON file) or
{"workspace": "<path>"} (a directory of the workspace that no commit pins; the gate lists such entries).

The gate (offline, no network, nothing is built):
  - every payload entry has built_from or third_party, and the fields are well formed;
  - every commit it names (the build commit, the recipe commit, the commit of each pinned input, and the gitlink
    of each mapped submodule) exists in a checkout of its repository, and a remote-tracking branch of a remote with
    the repository's URL contains it (`git branch -r --contains`): the commit is published;
  - the recipe file exists at the recipe commit;
  - the SHA-256 of each source file is the recorded one, a data file is its blob byte for byte, and a recorded
    Authenticode digest is the digest of the source file;
  - with --package, each packaged copy is its source byte for byte, a re-signed file has the recorded Authenticode
    digest, and only an entry with "release_edits": "<what the release changes>" may differ otherwise.

Usage:
  provenance.py check --sources release-sources.json --root <BC250_ROOT> --repo <recipe repository>
                      [--package <package folder>] [--json <report.json>]
  provenance.py digest <file>...     SHA-256 and Authenticode digest of PE files
  provenance.py locate --sources ... --root ... --repo ... <repository name> <commit>
                                     the checkout that holds the commit (used by rebuild-check.ps1)
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9A-F]{64}$")
PLACEHOLDER = re.compile(r"\{([a-z_]+)(?::([^}]+))?\}")
SIMPLE_PLACEHOLDERS = {"src", "out", "root"}


def git(cwd, *args, binary=False):
    r = subprocess.run(["git", "-C", str(cwd), *args], capture_output=True)
    if binary:
        return r.returncode, r.stdout
    return r.returncode, r.stdout.decode("utf-8", "replace").strip()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest().upper()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def authenticode_digest(path):
    """SHA-256 of a PE file without its signature: the CheckSum field, the security directory entry and the
    certificate table are left out, in file order (tools/win/kmd-deploy/kmdcommon.py image_digest, which was
    validated against the digest signtool embeds). A signed file and its unsigned image give the same digest
    when the image needed no padding to an 8-byte boundary; signing with another key keeps the digest."""
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file")
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    optional = pe + 24
    magic = int.from_bytes(data[optional:optional + 2], "little")
    if magic not in (0x10B, 0x20B):
        raise ValueError(f"{path}: unknown optional header magic {magic:#x}")
    security = optional + (96 if magic == 0x10B else 112) + 4 * 8
    table = int.from_bytes(data[security:security + 4], "little")
    size = int.from_bytes(data[security + 4:security + 8], "little")
    end = table or len(data)
    if table and table + size != len(data):
        raise ValueError(f"{path}: data after the certificate table")
    checksum = optional + 64
    h = hashlib.sha256()
    h.update(data[:checksum])
    h.update(data[checksum + 4:security])
    h.update(data[security + 8:end])
    return h.hexdigest().upper()


def normalize_url(url):
    url = url.strip().lower().rstrip("/")
    url = re.sub(r"^[a-z+]+://", "", url)
    url = re.sub(r"^git@([^:]+):", r"\1/", url)
    if url.endswith(".git"):
        url = url[:-4]
    return url


class Repositories:
    """The checkouts of each repository, and which commits they hold and publish. Results are cached: a release
    names the same commit for several files."""

    def __init__(self, table, root, recipe_repo_path):
        self.table = table
        self.root = Path(root)
        self.recipe_repo_path = Path(recipe_repo_path)
        self._found = {}
        self._published = {}

    def checkouts(self, name):
        out = []
        for c in self.table[name].get("checkouts", []):
            p = self.recipe_repo_path if c == "{repo}" else self.root / c
            out.append(p)
        return out

    def find(self, name, commit):
        """The checkout that holds the commit, or None."""
        key = (name, commit)
        if key not in self._found:
            self._found[key] = None
            for c in self.checkouts(name):
                if not c.exists():
                    continue
                code, _ = git(c, "cat-file", "-e", f"{commit}^{{commit}}")
                if code == 0:
                    self._found[key] = c
                    break
        return self._found[key]

    def published(self, name, commit):
        """Remote-tracking branches that contain the commit, of remotes whose URL is the repository's URL."""
        key = (name, commit)
        if key in self._published:
            return self._published[key]
        branches = []
        checkout = self.find(name, commit)
        want = normalize_url(self.table[name]["url"])
        if checkout is not None:
            code, text = git(checkout, "remote")
            remotes = []
            for remote in text.split() if code == 0 else []:
                c2, url = git(checkout, "config", "--get", f"remote.{remote}.url")
                if c2 == 0 and normalize_url(url) == want:
                    remotes.append(remote)
            if remotes:
                code, text = git(checkout, "branch", "-r", "--contains", commit, "--format=%(refname:short)")
                for line in text.splitlines() if code == 0 else []:
                    line = line.strip()
                    if any(line.startswith(r + "/") for r in remotes) and not line.endswith("/HEAD"):
                        branches.append(line)
        self._published[key] = branches
        return branches


def check(sources_path, root, repo, package=None):
    """Returns (errors, rows, summary). rows: one dict per payload file."""
    sources = json.loads(Path(sources_path).read_text(encoding="utf-8-sig"))
    errors = []
    rows = []
    table = sources.get("repositories")
    recipe_repo = sources.get("recipe_repository")
    if not isinstance(table, dict) or not table:
        return [f"{sources_path}: no 'repositories' table"], rows, {}
    if recipe_repo not in table:
        return [f"{sources_path}: 'recipe_repository' {recipe_repo!r} is not in 'repositories'"], rows, {}
    for name, spec in table.items():
        if not isinstance(spec, dict) or not isinstance(spec.get("url"), str) or not spec.get("checkouts"):
            errors.append(f"repositories.{name}: needs 'url' and 'checkouts'")
    if errors:
        return errors, rows, {}
    repos = Repositories(table, root, repo)
    payload_paths = {f.get("path") for f in sources.get("files", [])}

    def commit_ok(where, name, commit):
        """The commit exists in a checkout of the repository and a remote-tracking branch publishes it."""
        if name not in table:
            errors.append(f"{where}: repository {name!r} is not in 'repositories'")
            return None
        if not isinstance(commit, str) or not HEX40.match(commit):
            errors.append(f"{where}: commit {commit!r} is not a full 40-digit commit id")
            return None
        if repos.find(name, commit) is None:
            tried = ", ".join(str(c) for c in repos.checkouts(name))
            errors.append(f"{where}: {name} {commit[:8]} is in no checkout ({tried})")
            return None
        branches = repos.published(name, commit)
        if not branches:
            errors.append(f"{where}: {name} {commit[:8]} is on no remote-tracking branch of {table[name]['url']} "
                          f"(push the branch that carries it, then fetch)")
            return None
        return branches

    def submodules_ok(where, name, commit):
        """Each submodule that the repository maps to a repository of the table (a fork of this project) is
        published at the commit the gitlink names."""
        checkout = repos.find(name, commit) if name in table and isinstance(commit, str) else None
        if checkout is None:
            return
        for sub_path, sub_repo in (table[name].get("submodules") or {}).items():
            code, text = git(checkout, "ls-tree", commit, sub_path)
            parts = text.split()
            if code != 0 or len(parts) < 3 or parts[1] != "commit":
                errors.append(f"{where}: {name} {commit[:8]} has no submodule {sub_path}")
                continue
            commit_ok(f"{where} submodule {sub_path}", sub_repo, parts[2])

    counts ={"files": 0, "recipe": 0, "data": 0, "third_party": 0, "unverified": 0, "reproducible": 0}
    for f in sources.get("files", []):
        counts["files"] += 1
        path = f.get("path", "?")
        row = {"path": path, "kind": None, "unverified": f.get("unverified")}
        rows.append(row)
        sha = f.get("sha256", "")
        if not HEX64.match(sha or ""):
            errors.append(f"{path}: sha256 {sha!r} is not 64 upper-case hex digits")
        src = Path(root) / f.get("source", "").replace("/", "\\")
        if not src.is_file():
            errors.append(f"{path}: source {f.get('source')} does not exist")
        else:
            have = sha256_file(src)
            if have != sha:
                errors.append(f"{path}: source {f.get('source')} is {have[:8]}, the entry says {sha[:8]}")
        if "unverified" in f and (not isinstance(f["unverified"], str) or not f["unverified"].strip()):
            errors.append(f"{path}: 'unverified' needs the reason as text")
        for key in ("unsigned_sha256", "authenticode_sha256"):
            if key in f and not HEX64.match(f[key] or ""):
                errors.append(f"{path}: {key} {f[key]!r} is not 64 upper-case hex digits")
        packaged = Path(package) / path.replace("/", "\\") if package else None
        if "release_edits" in f and (not isinstance(f["release_edits"], str) or not f["release_edits"].strip()):
            errors.append(f"{path}: 'release_edits' needs the edits as text")
        if packaged is not None:
            # The packaged copy is the source byte for byte, unless the release signs it again (then the
            # Authenticode digest below decides) or edits it on purpose ("release_edits" says what).
            if not packaged.is_file():
                errors.append(f"{path}: not in the package {package}")
            elif "authenticode_sha256" not in f and "release_edits" not in f and sha256_file(packaged) != sha:
                errors.append(f"{path}: the packaged copy is {sha256_file(packaged)[:8]}, the entry says {sha[:8]}")
        if "authenticode_sha256" in f:
            # With --package also the packaged copy: the release signs the driver again with its own key, and the
            # digest proves that the image under the new signature is the recorded one.
            targets = [src] + ([packaged] if packaged is not None else [])
            for t in targets:
                if t.is_file():
                    try:
                        digest = authenticode_digest(t)
                    except ValueError as exc:
                        errors.append(f"{path}: {exc}")
                        continue
                    if digest != f["authenticode_sha256"]:
                        errors.append(f"{path}: Authenticode digest of {t} is {digest[:8]}, the entry says "
                                      f"{f['authenticode_sha256'][:8]}")
        built = f.get("built_from")
        third = f.get("third_party")
        if (built is None) == (third is None):
            errors.append(f"{path}: needs exactly one of 'built_from' and 'third_party'")
            continue
        if third is not None:
            row["kind"] = "third_party"
            counts["third_party"] += 1
            if not isinstance(third, dict):
                errors.append(f"{path}: third_party must be an object")
                continue
            if not str(third.get("url", "")).startswith("https://"):
                errors.append(f"{path}: third_party.url must be an https URL")
            if not third.get("version"):
                errors.append(f"{path}: third_party.version is empty")
            if third.get("sha256") != sha:
                errors.append(f"{path}: third_party.sha256 is not the entry's sha256")
            row["origin"] = f"{third.get('url')} {third.get('version')}"
            continue
        if not isinstance(built, dict):
            errors.append(f"{path}: built_from must be an object")
            continue
        name, commit = built.get("repo"), built.get("commit")
        where = f"{path} built_from"
        branches = commit_ok(where, name, commit)
        if branches:
            submodules_ok(where, name, commit)
        row["origin"] = f"{name} {str(commit)[:8]}"
        row["branches"] = branches or []
        if ("recipe" in built) == ("path" in built):
            errors.append(f"{where}: needs exactly one of 'recipe' (a build) and 'path' (a data file)")
            continue
        if "path" in built:
            row["kind"] = "data"
            counts["data"] += 1
            checkout = repos.find(name, commit) if name in table else None
            if checkout is not None:
                code, blob = git(checkout, "cat-file", "--filters", f"{commit}:{built['path']}", binary=True)
                if code != 0:
                    errors.append(f"{where}: {built['path']} does not exist at {commit[:8]}")
                elif sha256_bytes(blob) != sha:
                    errors.append(f"{where}: {built['path']} at {commit[:8]} is {sha256_bytes(blob)[:8]} as "
                                  f"checked out, the entry says {sha[:8]}")
            row["origin"] += f" {built['path']}"
            continue
        row["kind"] = "recipe"
        counts["recipe"] += 1
        counts["unverified" if "unverified" in f else "reproducible"] += 1
        recipe_commit = built.get("recipe_commit", commit if name == recipe_repo else None)
        if recipe_commit is None:
            errors.append(f"{where}: a build of {name} needs 'recipe_commit' (the {recipe_repo} commit of the recipe)")
        elif recipe_commit != commit or name != recipe_repo:
            commit_ok(f"{where} recipe_commit", recipe_repo, recipe_commit)
        if recipe_commit and HEX40.match(recipe_commit):
            checkout = repos.find(recipe_repo, recipe_commit)
            if checkout is not None:
                code, _ = git(checkout, "cat-file", "-e", f"{recipe_commit}:{built['recipe']}")
                if code != 0:
                    errors.append(f"{where}: recipe {built['recipe']} does not exist at {recipe_commit[:8]}")
        row["origin"] += f" {built['recipe']}"
        if not isinstance(built.get("output"), str) or not built["output"]:
            errors.append(f"{where}: 'output' names no file of the build")
        args = built.get("args", [])
        if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
            errors.append(f"{where}: 'args' must be a list of strings")
            args = []
        inputs = built.get("inputs", {})
        if not isinstance(inputs, dict):
            errors.append(f"{where}: 'inputs' must be an object")
            inputs = {}
        for iname, spec in inputs.items():
            iw = f"{where} input {iname}"
            if not isinstance(spec, dict):
                errors.append(f"{iw}: must be an object")
            elif "inline" in spec:
                if not isinstance(spec["inline"], dict):
                    errors.append(f"{iw}: 'inline' must be an object")
            elif "repo" in spec:
                # With "paths": those paths of the commit (git archive). Without: a work tree of the commit with
                # its submodules.
                if commit_ok(iw, spec["repo"], spec.get("commit")) and "paths" not in spec:
                    submodules_ok(iw, spec["repo"], spec["commit"])
                paths = spec.get("paths", [])
                if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
                    errors.append(f"{iw}: 'paths' must be a list of strings")
            elif "workspace" in spec:
                if not (Path(root) / spec["workspace"]).exists():
                    errors.append(f"{iw}: workspace path {spec['workspace']} does not exist")
                row.setdefault("unpinned", []).append(iname)
            else:
                errors.append(f"{iw}: needs 'repo' and 'commit', 'inline', or 'workspace'")
        for a in args:
            for m in PLACEHOLDER.finditer(a):
                kind, arg = m.group(1), m.group(2)
                if arg is None and kind in SIMPLE_PLACEHOLDERS:
                    continue
                if kind == "input" and arg in inputs:
                    continue
                if kind == "payload" and arg in payload_paths:
                    continue
                if kind == "tool" and arg:
                    continue
                errors.append(f"{where}: argument {a!r} has an unknown placeholder {m.group(0)}")
    return errors, rows, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check")
    c.add_argument("--sources", required=True)
    c.add_argument("--root", required=True)
    c.add_argument("--repo", required=True)
    c.add_argument("--package")
    c.add_argument("--json")
    d = sub.add_parser("digest")
    d.add_argument("files", nargs="+")
    loc = sub.add_parser("locate")
    loc.add_argument("--sources", required=True)
    loc.add_argument("--root", required=True)
    loc.add_argument("--repo", required=True)
    loc.add_argument("name")
    loc.add_argument("commit")
    args = parser.parse_args()
    if args.command == "digest":
        for f in args.files:
            print(f"{sha256_file(f)}  {authenticode_digest(f)}  {f}")
        return 0
    if args.command == "locate":
        # The checkout of a repository that holds the commit (rebuild-check.ps1 makes its work trees from it).
        sources = json.loads(Path(args.sources).read_text(encoding="utf-8-sig"))
        table = sources.get("repositories", {})
        if args.name not in table:
            print(f"repository {args.name!r} is not in 'repositories'", file=sys.stderr)
            return 1
        found = Repositories(table, args.root, args.repo).find(args.name, args.commit)
        if found is None:
            print(f"{args.name} {args.commit} is in no checkout", file=sys.stderr)
            return 1
        print(found)
        return 0
    errors, rows, counts = check(args.sources, args.root, args.repo, args.package)
    for r in rows:
        tag = {"recipe": "BUILD", "data": "DATA", "third_party": "THIRD"}.get(r["kind"], "?")
        if r.get("unverified"):
            tag += " (unverified)"
        branch = f"  [{r['branches'][0]}{' +%d' % (len(r['branches']) - 1) if len(r['branches']) > 1 else ''}]" \
            if r.get("branches") else ""
        print(f"  {tag:<20} {r['path']}  {r.get('origin', '')}{branch}")
    if counts:
        print(f"provenance: {counts['files']} payload files: {counts['recipe']} built by a recipe "
              f"({counts['reproducible']} claim a bit-identical rebuild, {counts['unverified']} unverified), "
              f"{counts['data']} data files from a commit, {counts['third_party']} third-party")
    unverified = [r for r in rows if r.get("unverified")]
    if unverified:
        print(f"provenance: {len(unverified)} unverified entries:")
        for r in unverified:
            print(f"  {r['path']}: {r['unverified']}")
    if args.json:
        Path(args.json).write_text(json.dumps({"errors": errors, "counts": counts, "files": rows}, indent=1),
                                   encoding="utf-8")
    for e in errors:
        print(f"FAIL {e}")
    print(f"provenance: {'FAIL, ' + str(len(errors)) + ' errors' if errors else 'PASS'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
