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
    digest or, inside the driver package, the same PE image identity, and only an entry with
    "release_edits": "<what the release changes>" may differ otherwise. The version resources are already in the
    pinned sources, because each component build stamps its own output (BD-166), so --package-version and
    --package-flavour are read here as a version and a flavour name and checked by versionstamp.py;
  - each text of "sources" that a lab tool reads as data (PARSED_SOURCES) starts with "bc250-win <commit>", that
    commit is the build commit of its payload row, and the kmd_abi of the KMD is BC250_KMD_VERSION at it.

Usage:
  provenance.py check --sources release-sources.json --root <BC250_ROOT> --repo <recipe repository>
                      [--package <package folder> [--package-version <release version> [--package-flavour lab]]]
                      [--json <report.json>]
  provenance.py commits --sources release-sources.json --root <BC250_ROOT> --repo <recipe repository>
                      every full commit id the manifest names, in a field or inside a text, is an object of a
                      checkout of its repository. This one runs for a lab candidate as well, where the whole gate
                      above is off: it asks whether the id exists, never whether it is published.
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
# A full commit id inside a prose text. The texts of `role` and of `sources` name commits that no field of the
# manifest repeats, so a stale one is invisible to every other check: the b34 candidates named
# 97b0f39e3bb99ba22a06c59cf5bd2f7a02e3a6a2 24 times, which is no object of this repository, while the head was
# 97b0f39ebc04f8c23c1b2110b9ee0d15034713f6. Both read 97b0f39e in the short form, and the gate below is off for a
# candidate, so a shipping build would have been the first to refuse it.
PROSE_COMMIT = re.compile(r"\b[0-9a-f]{40}\b")
# Commit ids of projects that the `repositories` table does not name and no checkout here holds: the firmware
# project and the licence refs of upstream projects. The manifest names them, and commits_ok does not look for them.
FOREIGN_COMMIT_PATHS = ("firmware.", "sources.firmware", "licenses[")
PLACEHOLDER = re.compile(r"\{([a-z_]+)(?::([^}]+))?\}")
# {tmp} is the build's own temporary directory (a recipe's -Temp): rebuild-check.ps1 and build-release-set.ps1 give each
# build one, so that parallel builds share no TEMP. It names a place, never an input, so it changes no byte.
SIMPLE_PLACEHOLDERS = {"src", "out", "root", "tmp"}
# The texts of `sources` that a lab tool reads as data, each with the payload row whose build it must name.
# scratch/m15/native-caps001/release-baseline.py (the pin-baseline arm of the release set validation) takes the commit at
# the start of sources.kmd with re.match(r'bc250-win ([0-9a-f]{8,40})\b'), reads BC250_KMD_VERSION of
# driver/kmd/bc250kmd_escape.h at that commit, and refuses a manifest whose kmd_abi differs from it. Round 9 of
# tester.27 put a sentence in front of that commit, and the arm refused the installed package on the lab.
PARSED_SOURCES = {"kmd": "payload/kmd/bc250kmd.sys"}
SOURCE_COMMIT = re.compile(r"bc250-win ([0-9a-f]{8,40})\b")
KMD_VERSION = re.compile(r"^#define BC250_KMD_VERSION (0x[0-9A-Fa-f]{8})u", re.M)


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
    return authenticode_digest_bytes(Path(path).read_bytes(), path)


def is_signed(path):
    """True when the PE file has a certificate table (its security directory entry is set)."""
    data = Path(path).read_bytes()
    pe = int.from_bytes(data[0x3C:0x40], "little") if data[:2] == b"MZ" else 0
    if not pe or data[pe:pe + 4] != b"PE\0\0":
        return False
    optional = pe + 24
    security = optional + (96 if int.from_bytes(data[optional:optional + 2], "little") == 0x10B else 112) + 4 * 8
    return int.from_bytes(data[security:security + 4], "little") != 0


def authenticode_digest_bytes(data, path="image"):
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


def load_versionstamp():
    """tools/build/versionstamp.py of this checkout (BD-166), loaded by path: tools/ is not a package."""
    import importlib.util
    path = Path(__file__).resolve().parents[1] / "build" / "versionstamp.py"
    spec = importlib.util.spec_from_file_location("versionstamp", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def state_catalog_inputs_ok(sources, repos):
    """Known state consumers must compile the catalog shipped by this flavour.

    This checks pinned transitive source inputs, not binary contents or runtime
    state. The compiled CLI/worker package check and provenance checks still apply.
    """
    catalog_path = "payload/kmd/amdgpu-state.bin"
    consumers = {
        "payload/kmd/bc250kmd.sys", "payload/tools/bc250kmd_cli.exe",
        "payload/tools/amdgpu-state-installer.exe", "payload/control/bc250control.dll",
        "payload/tools/bc250control.dll", "payload/desktop/bc250d3d_router.dll",
        "payload/wow64/desktop/bc250d3d_router.dll",
    }
    files = sources.get("files", [])
    selected = [row for row in files if row.get("path", "").casefold() in consumers | {catalog_path}]
    if not selected:
        return []
    anchors = [row for row in selected if row.get("path", "").casefold() == catalog_path]
    if len(anchors) != 1:
        return ["state catalog: exactly one flavour-selected catalog row is required"]
    errors, cache = [], {}

    def identity(row):
        build = row.get("built_from") or {}
        name, commit = build.get("repo"), build.get("commit")
        if name != sources.get("recipe_repository") or not isinstance(commit, str) or not HEX40.fullmatch(commit):
            raise ValueError("needs a full build commit in the recipe repository")
        key = (name, commit)
        if key not in cache:
            checkout = repos.find(name, commit)
            if checkout is None:
                raise ValueError("build commit is unavailable")
            code, header = git(checkout, "show", commit + ":driver/state/catalog.generated.h")
            if code:
                raise ValueError("generated catalog header is unavailable at the build commit")
            fields = []
            for macro, pattern in (("CATALOG_ID", r'"([0-9a-f]{64})"'),
                                   ("SCHEMA_VERSION", r'([0-9]+)u'), ("COUNT", r'([0-9]+)u')):
                values = re.findall(r"^#define AMDGPU_STATE_" + macro + r"[ \t]+" + pattern + r"[ \t]*$", header, re.M)
                if len(values) != 1:
                    raise ValueError("missing or ambiguous generated catalog " + macro)
                fields.append(values[0])
            cache[key] = tuple(fields)
        return cache[key]

    try:
        expected = identity(anchors[0])
    except ValueError as exc:
        return [f"state catalog: {exc}"]
    for row in selected:
        if row is anchors[0]:
            continue
        try:
            actual = identity(row)
            if actual != expected:
                errors.append(f"state catalog: {row['path']} build uses {actual[0]} ({actual[2]} entries), "
                              f"package requires {expected[0]} ({expected[2]} entries); rebuild this consumer")
        except ValueError as exc:
            errors.append(f"state catalog: {row['path']}: {exc}")
    return errors


def check(sources_path, root, repo, package=None, package_version=None, package_flavour="release",
          flavour=None, build_records=None):
    """Returns (errors, rows, summary). rows: one dict per payload file. `flavour` picks the manifest rows of
    that flavour of the release. `package_version` and `package_flavour` are the release the package claims to
    be: they are read as a version and a flavour name here, and the version resources themselves are checked by
    `versionstamp.py check --signed`, because the release stamps nothing (BD-166). `build_records` is a directory
    of the `recipe.json` records that the component builds leave behind: with it the gate also refuses a build
    whose source tree was not clean (`build_records_ok`)."""
    sources = json.loads(Path(sources_path).read_text(encoding="utf-8-sig"))
    from release_flavour import select_sources
    try:
        sources = select_sources(sources, flavour or sources.get('flavour', 'release'))
    except (KeyError, TypeError, ValueError) as error:
        return [f"{sources_path}: flavour selection: {error}"], [], {}
    if package and (Path(package) / 'manifest.json').is_file():
        manifest = json.loads((Path(package) / 'manifest.json').read_text(encoding='utf-8-sig'))
        if manifest.get('flavour', 'release') != sources['flavour']:
            return ['package and source flavours differ'], [], {}
    driver_paths = {}
    if package and (Path(package) / "installer/driver-package.json").is_file():
        # Derive the mapping from this source contract, never from an unchecked
        # report supplied with a candidate. Signed DLLs must retain code identity.
        from driver_package import members
        driver_paths = {r["source"]: r for r in members(sources)}
    errors = []
    rows = []
    if package and package_version:
        # The version and the flavour the package claims, read by the tool that owns both (BD-166). The resources
        # in the files are the business of `versionstamp.py check --signed`, which runs after the last signing
        # step: this script compares a packaged copy with its pinned source and nothing else.
        vs = load_versionstamp()
        try:
            vs.check_flavour(package_flavour)
            vs.amd_version(package_version)
        except (vs.StampError, OSError) as exc:
            return [f"version stamp: {exc}"], rows, {}
    by_path = {f.get("path"): f for f in sources.get("files", [])}
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
    errors.extend(state_catalog_inputs_ok(sources, repos))
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
        if "unverified" in f:
            # The reason is the only provenance these bytes have, so it must read as a sentence a person can use.
            # One word, an initial or a placeholder is not a reason: b27 shipped "T" here and this gate passed it.
            reason = f["unverified"] if isinstance(f["unverified"], str) else ""
            words = [w for w in reason.split() if any(c.isalpha() for c in w)]
            if not isinstance(f["unverified"], str) or not reason.strip():
                errors.append(f"{path}: 'unverified' needs the reason as text")
            elif len(reason.strip()) < 40 or len(words) < 6:
                errors.append(
                    f"{path}: 'unverified' is {reason.strip()!r}, which is not a reason: write the sentence that "
                    f"says why these bytes have no bit-identical rebuild (at least 40 characters and 6 words)"
                )
        for key in ("unsigned_sha256", "authenticode_sha256"):
            if key in f and not HEX64.match(f[key] or ""):
                errors.append(f"{path}: {key} {f[key]!r} is not 64 upper-case hex digits")
        destination = driver_paths[path]["path"] if path in driver_paths else path
        packaged = Path(package) / destination.replace("/", "\\") if package else None
        if "release_edits" in f and (not isinstance(f["release_edits"], str) or not f["release_edits"].strip()):
            errors.append(f"{path}: 'release_edits' needs the edits as text")
        if packaged is not None:
            # The packaged copy is the source byte for byte, unless the release signs it again (then the
            # Authenticode digest below, or the image identity of a DriverStore member, decides) or edits it on
            # purpose ("release_edits" says what).
            if not packaged.is_file():
                errors.append(f"{path}: not in the package {package}")
            elif path in driver_paths and path.endswith(".json"):
                expected = json.loads(src.read_text(encoding="utf-8-sig"))
                expected["ICD"]["library_path"] = ".\\" + ("vulkan_radeon32.dll" if driver_paths[path]["arch"] == "x86" else "vulkan_radeon.dll")
                if packaged.read_bytes() != (json.dumps(expected, indent=2) + "\n").encode("utf-8"):
                    errors.append(f"{path}: unexpected ICD manifest edit in DriverStore package")
            elif path in driver_paths and path.endswith('.config'):
                from driver_package import signing_safe_config
                pair = [next((item for item in sources['files'] if item['path'] ==
                              str(Path(path).with_name(name)).replace('\\', '/')), None)
                        for name in ('amdgpu_wddm_dxvk.dll', 'amdgpu_wddm_radv.dll')]
                try:
                    if not all(pair):
                        raise ValueError('missing original backend pair')
                    expected = signing_safe_config(src.read_bytes(), *[Path(root) / item['source'] for item in pair])
                    if packaged.read_bytes() != expected:
                        errors.append(f'{path}: unexpected caps or image-identity edit')
                except (OSError, ValueError) as exc:
                    errors.append(f'{path}: {exc}')
            elif path in driver_paths and packaged.suffix.lower() in ('.dll', '.exe'):
                from image_identity import image_digest
                try:
                    if image_digest(src) != image_digest(packaged):
                        errors.append(f'{path}: signing changed the PE image identity')
                except (OSError, ValueError) as exc:
                    errors.append(f'{path}: {exc}')
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
    sources_ok(sources, recipe_repo, repos, errors)
    pairs_ok(sources, recipe_repo, repos, errors)
    if build_records is not None:
        counts["build_records"] = build_records_ok(build_records, sources, errors)
    return errors, rows, counts


BUILD_RECORD = "recipe.json"


def build_records_ok(records_dir, sources, errors):
    """Every build record under this directory describes a clean source tree at a commit the release pins.

    `tools/build/build-*.ps1` write a `recipe.json` beside each build, whose `source` section comes from
    `Get-SourceIdentity` in `tools/build/common.ps1`: the commit of the tree the build read, how many entries
    of it were modified or untracked, and the hash of `git diff --binary HEAD`. A build from a tree with
    uncommitted work ships bytes that no commit holds; the rc4 manifest of tester.31 had to say exactly that
    about itself, and one engine tree on this workstation was found carrying six modified files from another
    piece of work. With the records in hand the release refuses such a build instead of describing it.
    """
    root = Path(records_dir)
    if not root.is_dir():
        errors.append(f"build records: {records_dir} is not a directory")
        return 0
    pinned = {b["commit"] for b in (f.get("built_from") or {} for f in sources.get("files", []))
              if isinstance(b.get("commit"), str)}
    records = sorted(root.rglob(BUILD_RECORD))
    if not records:
        errors.append(f"build records: no {BUILD_RECORD} under {records_dir}; a release says which tree each "
                      f"component was built from")
        return 0
    for path in records:
        where = f"build record {path.relative_to(root).as_posix()}"
        try:
            record = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as exc:
            errors.append(f"{where}: unreadable ({exc})")
            continue
        source = record.get("source")
        if not isinstance(source, dict):
            errors.append(f"{where}: no 'source' section")
            continue
        if not source.get("git"):
            errors.append(f"{where}: {source.get('path')} is no git checkout, so its bytes come from no commit")
            continue
        commit = source.get("commit")
        if not isinstance(commit, str) or not HEX40.match(commit):
            errors.append(f"{where}: source commit {commit!r} is not a full 40-digit commit id")
            continue
        modified, untracked = source.get("modified_entries"), source.get("untracked_entries")
        if modified is None or untracked is None:
            errors.append(f"{where}: the record does not say whether the tree was clean (no modified_entries "
                          f"or untracked_entries)")
        elif modified or untracked:
            errors.append(f"{where}: the build tree {source.get('path')} was not clean at {commit[:8]} "
                          f"({modified} modified, {untracked} untracked, diff "
                          f"{str(source.get('worktree_diff_sha256'))[:8]}); commit the work or build from a "
                          f"clean checkout")
        if pinned and commit not in pinned:
            errors.append(f"{where}: its source commit {commit[:8]} is pinned by no row of this release")
    return len(records)


def pairs_ok(sources, recipe_repo, repos, errors):
    """The x64 and the x86 build of one component come from one commit, or the row says why they do not.

    `payload/wow64/<rest>` is the 32-bit build of `payload/<rest>`: the same component, the same source, two
    architectures. Nothing made them move together, and a pin that lags is invisible in a diff of the manifest:
    the x86 desktop UMD of train b34 was pinned at a commit five behind its x64 pair, and the only way anyone
    noticed was a reproducibility measurement of the two files. A game that loads the 32-bit DLL then runs code
    that no 64-bit test covered.

    So the two rows carry the same `built_from.commit`, or the wow64 row carries `pair_skew`, a sentence that
    says why. A pair whose commits are two sides of a fork is no lag at all, so its sentence must name both
    commits in their short form. The reason then stops covering the pair as soon as either pin moves, which is
    what a known difference deserves and a blanket excuse does not.

    A differing `recipe_commit` is a different matter: the build script is ours and moves with every train. It
    passes on its own while one recipe commit is an ancestor of the other, which is a newer script and not
    another source, and it asks for the same sentence when the two have forked. Ancestry is only asked of a
    checkout that holds both commits, so a computer without that repository reports nothing instead of guessing.
    """
    by_path = {f.get("path"): f for f in sources.get("files", []) if isinstance(f, dict)}
    for path, row in sorted(by_path.items()):
        if not isinstance(path, str) or not path.startswith("payload/wow64/"):
            continue
        sibling_path = "payload/" + path[len("payload/wow64/"):]
        sibling = by_path.get(sibling_path)
        if sibling is None:
            continue                      # a 32-bit file with no 64-bit counterpart is not a pair
        mine = row.get("built_from") if isinstance(row.get("built_from"), dict) else {}
        theirs = sibling.get("built_from") if isinstance(sibling.get("built_from"), dict) else {}
        differ = [f for f in ("commit", "recipe_commit")
                  if mine.get(f) is not None or theirs.get(f) is not None
                  if mine.get(f) != theirs.get(f)]
        if "recipe_commit" in differ and lineage(repos, recipe_repo, mine.get("recipe_commit"),
                                                 theirs.get("recipe_commit")) != "forked":
            differ.remove("recipe_commit")
        if not differ:
            continue
        said = row.get("pair_skew")
        said = said.strip() if isinstance(said, str) else ""
        where = f"{path} and {sibling_path}"
        what = ", ".join(f"{f} {str(mine.get(f))[:8]} against {str(theirs.get(f))[:8]}" for f in differ)
        if len(said) < 40 or len(said.split()) < 6:
            errors.append(f"{where}: the pair is built from different sources ({what}). Pin both at one commit, "
                          f"or write in 'pair_skew' of the wow64 row why the two differ (a sentence: at least 40 "
                          f"characters and 6 words)")
            continue
        if "commit" not in differ or lineage(repos, mine.get("repo") if mine.get("repo") == theirs.get("repo")
                                             else None, mine.get("commit"), theirs.get("commit")) != "forked":
            continue
        short = [str(c)[:8] for c in (mine.get("commit"), theirs.get("commit"))]
        if not all(s in said for s in short):
            errors.append(f"{where}: {short[0]} and {short[1]} are on diverged commits of {mine.get('repo')}, "
                          f"neither behind the other, so one of the two builds carries work the other has not "
                          f"got. A 'pair_skew' for a fork names both commits ({short[0]} and {short[1]}), so "
                          f"that it stops covering the pair when either pin moves")


def lineage(repos, repo, mine, theirs):
    """"lag" while one of the two commits is an ancestor of the other, "forked" while neither is, and "unknown"
    without the two commits in one checkout of `repo`."""
    if not repo or not isinstance(mine, str) or not isinstance(theirs, str):
        return "unknown"
    checkout = repos.find(repo, mine)
    if checkout is None or repos.find(repo, theirs) is None:
        return "unknown"
    behind = git(checkout, "merge-base", "--is-ancestor", mine, theirs)[0] == 0
    ahead = git(checkout, "merge-base", "--is-ancestor", theirs, mine)[0] == 0
    return "lag" if behind or ahead else "forked"


def sources_ok(sources, recipe_repo, repos, errors):
    """Each text of PARSED_SOURCES starts with 'bc250-win <commit>', the commit is the build commit of its payload
    row, and for the KMD the manifest's kmd_abi is BC250_KMD_VERSION at that commit. A payload without the row
    has nothing for the lab tool to read."""
    texts = sources.get("sources") if isinstance(sources.get("sources"), dict) else {}
    for key, row_path in PARSED_SOURCES.items():
        row = next((f for f in sources.get("files", []) if f.get("path") == row_path), None)
        if row is None:
            continue
        where = f"sources.{key}"
        text = texts.get(key)
        found = SOURCE_COMMIT.match(text) if isinstance(text, str) else None
        if not found:
            errors.append(f"{where}: must start with 'bc250-win <commit>' (release-baseline.py reads the commit there "
                          f"with re.match), and it starts {str(text)[:48]!r}")
            continue
        built = row.get("built_from") if isinstance(row.get("built_from"), dict) else {}
        commit = built.get("commit")
        if built.get("repo") != recipe_repo or not isinstance(commit, str) or not commit.startswith(found.group(1)):
            errors.append(f"{where}: names bc250-win {found.group(1)[:8]}, but {row_path} is built from "
                          f"{built.get('repo')} {str(commit)[:8]}")
            continue
        if key != "kmd":
            continue
        checkout = repos.find(recipe_repo, commit)
        if checkout is None:
            continue  # the built_from check of the row already says that the commit is in no checkout
        code, header = git(checkout, "show", f"{commit}:driver/kmd/bc250kmd_escape.h")
        versions = KMD_VERSION.findall(header) if code == 0 else []
        abi = "0x" + versions[0][2:].upper() if len(versions) == 1 else None
        if abi is None or abi != str(sources.get("kmd_abi")):
            errors.append(f"{where}: kmd_abi {sources.get('kmd_abi')} is not BC250_KMD_VERSION {abi} of "
                          f"driver/kmd/bc250kmd_escape.h at {commit[:8]}")


def check_commits(sources_path, root, repo):
    """Every full commit id the manifest names is an object of a checkout of its repository.

    Two kinds, and this check runs for a lab candidate as well, where the gate above is off:
      - a field: `commit` and `recipe_commit`, with the `repo` of the object that holds it (or of the payload row
        around it);
      - a text: a 40-digit id inside any other text, that is `role` and the texts of `sources`. Such an id names
        this project or one of its forks, so it has to be an object of a checkout of some repository of the table.
    Publication is not asked here: that is the gate above, and a candidate has nothing published yet. A repository
    with no checkout on this computer is named in the notes, never guessed at.
    Returns (errors, notes, checked).
    """
    sources = json.loads(Path(sources_path).read_text(encoding="utf-8-sig"))
    table = sources.get("repositories") if isinstance(sources.get("repositories"), dict) else {}
    recipe_repo = sources.get("recipe_repository")
    errors, notes, found = [], [], []
    if not table or recipe_repo not in table:
        return [f"{sources_path}: no 'repositories' table with its 'recipe_repository'"], notes, 0
    repos = Repositories(table, root, repo)
    present = [name for name in table if any(c.exists() for c in repos.checkouts(name))]
    for name in table:
        if name not in present:
            notes.append(f"{name}: no checkout on this computer, so the commits it names were not looked for")
    if not present:
        return errors, notes, 0

    def walk(node, where, scope):
        if isinstance(node, dict):
            scope = node["repo"] if isinstance(node.get("repo"), str) else scope
            for key, value in node.items():
                walk(value, f"{where}.{key}" if where else key, scope)
        elif isinstance(node, list):
            for index, value in enumerate(node):
                walk(value, f"{where}[{index}]", scope)
        elif isinstance(node, str):
            if where.startswith(FOREIGN_COMMIT_PATHS):
                return
            key = where.split(".")[-1].split("[")[0]
            if HEX40.match(node):
                # `commit` is a commit of the repository in scope; `recipe_commit` is always one of the recipe
                # repository, whatever is built from it (the docstring above).
                owner = {"commit": scope, "recipe_commit": recipe_repo}.get(key)
                found.append((where, owner, node))
                return
            for one in dict.fromkeys(PROSE_COMMIT.findall(node)):
                found.append((f"{where} (text)", None, one))

    walk(sources, "", None)
    for where, scope, commit in found:
        if scope is not None:
            if scope not in table:
                errors.append(f"{where}: repository {scope!r} is not in 'repositories'")
            elif scope in present and repos.find(scope, commit) is None:
                tried = ", ".join(str(c) for c in repos.checkouts(scope))
                errors.append(f"{where}: {scope} {commit} is no object of {tried}")
            continue
        if not any(repos.find(name, commit) for name in present):
            errors.append(f"{where}: {commit} is no object of any checkout of {', '.join(present)} "
                          f"(its short form {commit[:8]} can read like the right one)")
    return errors, notes, len(found)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check")
    c.add_argument("--sources", required=True)
    c.add_argument("--root", required=True)
    c.add_argument("--repo", required=True)
    c.add_argument("--package")
    c.add_argument("--package-version", help="the release version whose version resources the packaged PE files carry")
    c.add_argument("--package-flavour", choices=("release", "lab"), default="release",
                   help="the flavour of those version resources")
    c.add_argument("--json")
    c.add_argument('--flavour', choices=('release', 'lab'))
    c.add_argument("--build-records",
                   help="a directory holding the recipe.json records of the builds that made this release: with "
                        "it the gate refuses a component built from a tree that was not clean, and a record "
                        "whose source commit no row pins")
    cm = sub.add_parser("commits")
    cm.add_argument("--sources", required=True)
    cm.add_argument("--root", required=True)
    cm.add_argument("--repo", required=True)
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
    if args.command == "commits":
        errors, notes, checked = check_commits(args.sources, args.root, args.repo)
        for n in notes:
            print(f"  note: {n}")
        for e in errors:
            print(f"FAIL {e}")
        print(f"commits: {checked} commit id(s) named, "
              f"{'FAIL, ' + str(len(errors)) + ' of them in no checkout' if errors else 'PASS'}")
        return 1 if errors else 0
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
    errors, rows, counts = check(args.sources, args.root, args.repo, args.package, args.package_version,
                                 args.package_flavour, flavour=args.flavour,
                                 build_records=args.build_records)
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
