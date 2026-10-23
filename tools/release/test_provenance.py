"""Self-test of the release provenance gate (tools/release/provenance.py).

    python -m unittest discover -s tools/release -p test_provenance.py   # the 'release-provenance' gate of quick.ps1

Each test makes throwaway git repositories: a recipe repository with a bare "published" remote, a fork with a
submodule, and a workspace root with the payload source files. A complete manifest must pass, and each defect the
gate exists for must fail with its own message: an entry without provenance, a commit that no remote-tracking branch
of the repository's URL holds, a submodule commit that is not published, a source or data file with other bytes, an
"unverified" entry without a reason, a recipe that the recipe commit does not have, an unknown argument placeholder,
a download that is not https, a packaged copy that is not its source, a packaged PE file without the version resource
of its release (--package-version, BD-166), and a re-signed driver whose Authenticode digest changed. The Authenticode digest itself must ignore the checksum, the security directory and the certificate table.
rebuild-check.ps1 must keep the log of a failed build (with pwsh 7 on PATH; the test is skipped without it).
BC250_TEST_OUT, when set, holds the temporary trees (quick.ps1 keeps them off drive C:).
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import provenance

OUT = os.environ.get("BC250_TEST_OUT") or None


def git(cwd, *args):
    r = subprocess.run(["git", "-C", str(cwd), "-c", "user.name=test", "-c", "user.email=test@example.com",
                        "-c", "protocol.file.allow=always", "-c", "core.autocrlf=false", *args],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr}")
    return r.stdout.strip()


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def remove_tree(path):
    """shutil.rmtree that also removes the read-only object files git writes on Windows."""
    def writable_again(func, target, _):
        os.chmod(target, stat.S_IWRITE)
        func(target)
    shutil.rmtree(path, onexc=writable_again)


def fake_pe(body, signature=b""):
    """A PE32+ image as provenance.authenticode_digest reads it: MZ, e_lfanew, PE, optional header magic 0x20B with a
    CheckSum and a security directory entry, then the body; with a signature, a certificate table at the end."""
    pe = 0x40
    optional = pe + 24
    security = optional + 112 + 4 * 8
    head = bytearray(security + 8)
    head[0:2] = b"MZ"
    head[0x3C:0x40] = struct.pack("<I", pe)
    head[pe:pe + 4] = b"PE\0\0"
    head[optional:optional + 2] = struct.pack("<H", 0x20B)
    image = bytes(head) + body
    if not signature:
        return image
    data = bytearray(image)
    data[optional + 64:optional + 68] = struct.pack("<I", 0x1234)  # a signer fills in the checksum
    data[security:security + 8] = struct.pack("<II", len(image), len(signature))
    return bytes(data) + signature


def add_signature(image, signature):
    """A real PE image under a certificate table, as signtool writes it: zero padding to 8 bytes, a checksum, the
    security directory entry and the table at the end."""
    data = bytearray(image + b"\0" * (-len(image) % 8))
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    security = optional + (96 if struct.unpack_from("<H", data, optional)[0] == 0x10B else 112) + 4 * 8
    struct.pack_into("<I", data, optional + 64, 0x1234)
    struct.pack_into("<II", data, security, len(data), len(signature))
    return bytes(data) + signature


class Workspace:
    """A recipe repository 'tool' published to a bare remote, a fork 'engine' with a submodule 'part', and a
    workspace root with the payload files."""

    def __init__(self, base):
        self.base = Path(base)
        self.root = self.base / "root"
        self.root.mkdir()
        self.remotes = self.base / "remotes"
        self.remotes.mkdir()
        # The submodule's repository and its published remote.
        self.part = self.make_repo("part", {"part.txt": b"part 1\n"})
        self.part_commit = git(self.part, "rev-parse", "HEAD")
        # The fork, with the submodule at the published commit.
        self.engine = self.make_repo("engine", {"engine.c": b"int engine;\n"}, push=False)
        git(self.engine, "submodule", "add", str(self.remotes / "part.git"), "sub/part")
        git(self.engine, "commit", "-q", "-m", "submodule")
        git(self.engine, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.engine, "fetch", "-q", "origin")
        self.engine_commit = git(self.engine, "rev-parse", "HEAD")
        # The recipe repository: a recipe, a data file, and the payload's source files under the root.
        catalog = b'AMDST02\0' + struct.pack('<II', 2, 1) + b'1' * 64 + b'\0' * 12
        self.tool = self.make_repo("tool", {"build.ps1": b"# recipe\n", "data/icd.json": b'{"icd": 1}\n',
                                            "driver/state/amdgpu-state.bin": catalog,
                                            "driver/state/catalog.generated.h":
                                                b'#define AMDGPU_STATE_CATALOG_ID "' + b'1' * 64 + b'"\n#define AMDGPU_STATE_COUNT 1u\n#define AMDGPU_STATE_SCHEMA_VERSION 2u\n',
                                            "driver/kmd/bc250kmd_escape.h":
                                                b"#define BC250_KMD_VERSION 0x000700D8u   // revision 216\n"})
        self.tool_commit = git(self.tool, "rev-parse", "HEAD")
        (self.root / "scratch").mkdir()
        self.files = {
            "built.dll": b"built bytes",
            "icd.json": b'{"icd": 1}\n',
            "tool.exe": b"downloaded bytes",
            "engine.dll": b"engine bytes",
            "driver.sys": fake_pe(b"\x01" * 64, b"lab signature"),
            "state.bin": catalog,
        }
        for name, data in self.files.items():
            (self.root / "scratch" / name).write_bytes(data)
        self.manifest_path = self.base / "release-sources.json"
        self.manifest = {
            "schema": 3,
            "recipe_repository": "tool",
            "repositories": {
                "tool": {"url": str(self.remotes / "tool.git"), "checkouts": ["{repo}"]},
                "engine": {"url": str(self.remotes / "engine.git"), "checkouts": [str(self.engine)],
                           "submodules": {"sub/part": "part"}},
                "part": {"url": str(self.remotes / "part.git"), "checkouts": [str(self.part)]},
            },
            "files": [
                self.entry("payload/built.dll", "built.dll", built_from={
                    "repo": "tool", "commit": self.tool_commit, "recipe": "build.ps1",
                    "args": ["-Out", "{out}", "-Kits", "{root}/kits"], "output": "built.dll"}),
                self.entry("payload/icd.json", "icd.json", built_from={
                    "repo": "tool", "commit": self.tool_commit, "path": "data/icd.json"}),
                self.entry("payload/tool.exe", "tool.exe", third_party={
                    "url": "https://example.com/tool.zip", "version": "1.0", "sha256": sha(self.files["tool.exe"])}),
                self.entry("payload/engine.dll", "engine.dll", built_from={
                    "repo": "engine", "commit": self.engine_commit, "recipe_commit": self.tool_commit,
                    "recipe": "build.ps1", "args": ["-Source", "{src}", "-Build", "{out}"], "output": "engine.dll"}),
                self.entry("payload/driver.sys", "driver.sys", built_from={
                    "repo": "tool", "commit": self.tool_commit, "recipe": "build.ps1", "args": [],
                    "output": "driver.sys"},
                    authenticode_sha256=provenance.authenticode_digest(self.root / "scratch" / "driver.sys")),
            ],
        }

    def make_repo(self, name, files, push=True):
        repo = self.base / name
        repo.mkdir()
        git(repo, "init", "-q")
        git(repo, "symbolic-ref", "HEAD", "refs/heads/main")
        # The repository's own setting, so that the gate's `git cat-file --filters` gives the committed bytes here
        # whatever the machine's core.autocrlf is.
        git(repo, "config", "core.autocrlf", "false")
        for rel, data in files.items():
            (repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (repo / rel).write_bytes(data)
        git(repo, "add", "-A")
        git(repo, "commit", "-q", "-m", "first")
        remote = self.remotes / f"{name}.git"
        git(self.remotes, "init", "-q", "--bare", str(remote))
        git(remote, "symbolic-ref", "HEAD", "refs/heads/main")
        git(repo, "remote", "add", "origin", str(remote))
        if push:
            git(repo, "push", "-q", "origin", "HEAD:refs/heads/main")
            git(repo, "fetch", "-q", "origin")
        return repo

    def entry(self, path, name, **provenance_fields):
        e = {"component": "test", "path": path, "source": f"scratch/{name}", "sha256": sha(self.files[name])}
        e.update(provenance_fields)
        return e

    def check(self, manifest=None, package=None, package_version=None, package_flavour="release",
              build_records=None):
        self.manifest_path.write_text(json.dumps(manifest or self.manifest), encoding="utf-8")
        errors, rows, counts = provenance.check(self.manifest_path, self.root, self.tool, package, package_version,
                                                package_flavour, build_records=build_records)
        return errors, rows, counts

    def commits(self, manifest=None):
        """The commit check that runs for a candidate as well (provenance.check_commits)."""
        self.manifest_path.write_text(json.dumps(manifest or self.manifest), encoding="utf-8")
        return provenance.check_commits(self.manifest_path, self.root, self.tool)


class ProvenanceGate(unittest.TestCase):
    """One workspace for the tests that change only the manifest; a test that changes a repository or a file makes
    its own (fresh_workspace)."""

    @classmethod
    def setUpClass(cls):
        cls.shared_tmp = tempfile.mkdtemp(prefix="provenance-", dir=OUT)
        cls.shared = Workspace(cls.shared_tmp)

    @classmethod
    def tearDownClass(cls):
        remove_tree(cls.shared_tmp)

    def setUp(self):
        self.tmp = self.shared_tmp
        self.ws = self.shared
        self.own = None

    def tearDown(self):
        if self.own:
            remove_tree(self.own)

    def fresh_workspace(self):
        self.own = tempfile.mkdtemp(prefix="provenance-", dir=OUT)
        self.tmp = self.own
        self.ws = Workspace(self.own)

    def manifest(self):
        return copy.deepcopy(self.ws.manifest)

    def file(self, m, path):
        return next(f for f in m["files"] if f["path"] == path)

    def assertFails(self, manifest, text, package=None, package_version=None, package_flavour="release"):
        errors, _, _ = self.ws.check(manifest, package, package_version, package_flavour)
        self.assertTrue(any(text in e for e in errors), f"no error with {text!r} in {errors}")

    def test_complete_manifest_passes(self):
        errors, rows, counts = self.ws.check()
        self.assertEqual(errors, [])
        self.assertEqual(counts["files"], 5)
        self.assertEqual(counts["recipe"], 3)
        self.assertEqual(counts["data"], 1)
        self.assertEqual(counts["third_party"], 1)
        self.assertEqual(counts["unverified"], 0)
        self.assertTrue(all(r["branches"] == ["origin/main"] for r in rows if r["kind"] != "third_party"))

    def test_flavour_selection_and_manifest_agree(self):
        manifest = self.manifest()
        manifest['schema'] = 4
        manifest['files'] = [dict(row, flavour=flavour)
                             for row in manifest['files'] for flavour in ('release', 'lab')]
        self.ws.manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        for flavour in ('release', 'lab'):
            errors, rows, counts = provenance.check(self.ws.manifest_path, self.ws.root, self.ws.tool,
                                                   flavour=flavour)
            self.assertEqual(errors, [])
            self.assertEqual(counts['files'], 5)
            self.assertEqual(len(rows), 5)
        with tempfile.TemporaryDirectory(dir=self.tmp) as temp:
            package = Path(temp)
            (package / 'manifest.json').write_text('{"flavour":"lab"}', encoding='utf-8')
            errors, _, _ = provenance.check(self.ws.manifest_path, self.ws.root, self.ws.tool, package,
                                            flavour='release')
            self.assertEqual(errors, ['package and source flavours differ'])

    def test_entry_without_provenance_fails(self):
        m = self.manifest()
        del self.file(m, "payload/built.dll")["built_from"]
        self.assertFails(m, "needs exactly one of 'built_from' and 'third_party'")

    def test_entry_with_both_fails(self):
        m = self.manifest()
        self.file(m, "payload/tool.exe")["built_from"] = self.file(m, "payload/built.dll")["built_from"]
        self.assertFails(m, "needs exactly one of 'built_from' and 'third_party'")

    def test_unpublished_commit_fails(self):
        self.fresh_workspace()
        (self.ws.tool / "late.txt").write_text("late\n")
        git(self.ws.tool, "add", "-A")
        git(self.ws.tool, "commit", "-q", "-m", "not pushed")
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["commit"] = git(self.ws.tool, "rev-parse", "HEAD")
        self.assertFails(m, "is on no remote-tracking branch")

    def test_branch_of_another_url_does_not_publish(self):
        m = self.manifest()
        m["repositories"]["tool"]["url"] = "https://example.com/elsewhere/tool"
        self.assertFails(m, "is on no remote-tracking branch of https://example.com/elsewhere/tool")

    def test_unknown_commit_fails(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["commit"] = "0" * 40
        self.assertFails(m, "is in no checkout")

    def test_short_commit_fails(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["commit"] = self.ws.tool_commit[:8]
        self.assertFails(m, "is not a full 40-digit commit id")

    def test_unpublished_submodule_commit_fails(self):
        self.fresh_workspace()
        (self.ws.part / "part.txt").write_text("part 2\n")
        git(self.ws.part, "commit", "-q", "-am", "not pushed")
        sub = self.ws.engine / "sub" / "part"
        git(sub, "fetch", "-q", str(self.ws.part), "main")
        git(sub, "checkout", "-q", "FETCH_HEAD")
        git(self.ws.engine, "commit", "-q", "-am", "submodule moves to an unpublished commit")
        git(self.ws.engine, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.engine, "fetch", "-q", "origin")
        m = self.manifest()
        self.file(m, "payload/engine.dll")["built_from"]["commit"] = git(self.ws.engine, "rev-parse", "HEAD")
        self.assertFails(m, "submodule sub/part: part")

    def wow64_pair(self, m):
        """A 32-bit row beside payload/engine.dll: the same component, the same bytes, the other architecture."""
        pair = copy.deepcopy(self.file(m, "payload/engine.dll"))
        pair["path"] = "payload/wow64/engine.dll"
        m["files"].append(pair)
        return pair

    def test_a_pair_from_one_commit_passes(self):
        m = self.manifest()
        self.wow64_pair(m)
        errors, _, _ = self.ws.check(m)
        self.assertEqual([e for e in errors if "different sources" in e or "diverged" in e], [])

    def test_a_pair_from_two_commits_needs_a_reason(self):
        """The fault of train b34: the x86 desktop UMD was pinned five commits behind its x64 pair, and nothing
        in the manifest said so. A lag is allowed, a silent one is not."""
        self.fresh_workspace()
        git(self.ws.engine, "commit", "-q", "--allow-empty", "-m", "the x64 build moves on")
        git(self.ws.engine, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.engine, "fetch", "-q", "origin")
        moved = git(self.ws.engine, "rev-parse", "HEAD")
        m = self.manifest()
        pair = self.wow64_pair(m)
        self.file(m, "payload/engine.dll")["built_from"]["commit"] = moved
        self.assertFails(m, "the pair is built from different sources")
        pair["pair_skew"] = "behind"
        self.assertFails(m, "at least 40 characters and 6 words")
        pair["pair_skew"] = ("the 32-bit build is not rebuilt yet, so it stays at the commit of the shipped file "
                             "until train b35 rebuilds it")
        errors, _, _ = self.ws.check(m)
        self.assertEqual([e for e in errors if "different sources" in e or "diverged" in e], [])
        # The recipe is our own build script and moves with every train, so a newer one asks for nothing.
        pair["built_from"]["commit"] = moved
        del pair["pair_skew"]
        git(self.ws.tool, "commit", "-q", "--allow-empty", "-m", "the build script moves on")
        git(self.ws.tool, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.tool, "fetch", "-q", "origin")
        self.file(m, "payload/engine.dll")["built_from"]["recipe_commit"] = git(self.ws.tool, "rev-parse", "HEAD")
        errors, _, _ = self.ws.check(m)
        self.assertEqual([e for e in errors if "different sources" in e or "diverged" in e], [])
        # A build script of another line is a skew again: the two builds then ran different code.
        git(self.ws.tool, "checkout", "-q", "-b", "other", self.ws.tool_commit)
        git(self.ws.tool, "commit", "-q", "--allow-empty", "-m", "another line of the build script")
        git(self.ws.tool, "push", "-q", "origin", "HEAD:refs/heads/other")
        git(self.ws.tool, "fetch", "-q", "origin")
        pair["built_from"]["recipe_commit"] = git(self.ws.tool, "rev-parse", "HEAD")
        self.assertFails(m, "recipe_commit")

    def test_a_diverged_pair_is_no_lag(self):
        """Two sides of a fork mean one build carries work the other has not got, which is no lag. A sentence
        about a lag does not cover it, and the reason that does names both commits, so that it expires with the
        next pin that moves."""
        self.fresh_workspace()
        base = self.ws.engine_commit
        git(self.ws.engine, "checkout", "-q", "-b", "side", base)
        git(self.ws.engine, "commit", "-q", "--allow-empty", "-m", "a commit of another line")
        side = git(self.ws.engine, "rev-parse", "HEAD")
        git(self.ws.engine, "push", "-q", "origin", "HEAD:refs/heads/side")
        git(self.ws.engine, "checkout", "-q", "-")
        git(self.ws.engine, "commit", "-q", "--allow-empty", "-m", "the line the release follows")
        git(self.ws.engine, "push", "-q", "origin", "HEAD:refs/heads/main")
        git(self.ws.engine, "fetch", "-q", "origin")
        head = git(self.ws.engine, "rev-parse", "HEAD")
        m = self.manifest()
        pair = self.wow64_pair(m)
        self.file(m, "payload/engine.dll")["built_from"]["commit"] = head
        pair["built_from"]["commit"] = side
        pair["pair_skew"] = ("the 32-bit build is not rebuilt yet, so it stays at the commit of the shipped file "
                             "until train b35 rebuilds it")
        self.assertFails(m, "are on diverged commits")
        pair["pair_skew"] = (f"the 32-bit file is built from the {side[:8]} line and the 64-bit one from "
                             f"{head[:8]}, two lines the next train brings together")
        errors, _, _ = self.ws.check(m)
        self.assertEqual([e for e in errors if "different sources" in e or "diverged" in e], [])

    def test_fork_build_needs_recipe_commit(self):
        m = self.manifest()
        del self.file(m, "payload/engine.dll")["built_from"]["recipe_commit"]
        self.assertFails(m, "needs 'recipe_commit'")

    def test_source_with_other_bytes_fails(self):
        self.fresh_workspace()
        (self.ws.root / "scratch" / "built.dll").write_bytes(b"other bytes")
        self.assertFails(None, "the entry says")

    def test_data_file_with_other_blob_fails(self):
        m = self.manifest()
        self.file(m, "payload/icd.json")["built_from"]["path"] = "build.ps1"
        self.assertFails(m, "as checked out, the entry says")

    def test_unverified_needs_reason(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["unverified"] = " "
        self.assertFails(m, "'unverified' needs the reason as text")
        # Three words are not a reason either: the gate asks for the sentence that says why these bytes
        # have no bit-identical rebuild, at least 40 characters and 6 words of it.
        self.file(m, "payload/built.dll")["unverified"] = "built before /Brepro"
        self.assertFails(m, "which is not a reason")
        self.file(m, "payload/built.dll")["unverified"] = (
            "These bytes were built before the recipe carried /Brepro, so no rebuild of that commit "
            "can give them again."
        )
        errors, rows, counts = self.ws.check(m)
        self.assertEqual(errors, [])
        self.assertEqual(counts["unverified"], 1)
        self.assertEqual(counts["reproducible"], 2)

    def records(self, trees):
        """A directory of recipe.json records, one per build, as tools/build/build-*.ps1 leave behind. Each
        entry of `trees` is (name, source section)."""
        root = Path(self.tmp) / f"records-{self.id().rsplit('.', 1)[-1]}"
        if root.is_dir():
            remove_tree(str(root))
        for name, source in trees:
            d = root / name
            d.mkdir(parents=True)
            (d / "recipe.json").write_text(json.dumps({"recipe": name, "schema": 1, "source": source}),
                                           encoding="utf-8")
        root.mkdir(parents=True, exist_ok=True)
        return str(root)

    def tree(self, commit, modified=0, untracked=0, **more):
        source = {"path": f"P:/build/{commit[:6]}", "git": True, "commit": commit,
                  "modified_entries": modified, "untracked_entries": untracked,
                  "worktree_diff_sha256": "DE" * 32 if (modified or untracked) else None}
        source.update(more)
        return source

    def test_clean_build_records_pass(self):
        """The release names the tree every component was built from, and each one was clean at a pinned commit."""
        records = self.records([("engine", self.tree(self.ws.engine_commit)),
                                ("tool", self.tree(self.ws.tool_commit))])
        errors, _, counts = self.ws.check(build_records=records)
        self.assertEqual(errors, [])
        self.assertEqual(counts["build_records"], 2)

    def test_a_dirty_build_tree_fails(self):
        """The negative control: one modified tracked file in the tree a component was built from. Those bytes
        are in no commit, so the release must not describe them as coming from one."""
        records = self.records([("engine", self.tree(self.ws.engine_commit, modified=1))])
        errors, _, _ = self.ws.check(build_records=records)
        self.assertTrue(any("was not clean" in e and "1 modified" in e for e in errors), errors)

    def test_an_untracked_file_in_the_build_tree_fails(self):
        records = self.records([("engine", self.tree(self.ws.engine_commit, untracked=2))])
        errors, _, _ = self.ws.check(build_records=records)
        self.assertTrue(any("2 untracked" in e for e in errors), errors)

    def test_a_record_of_a_commit_no_row_pins_fails(self):
        records = self.records([("engine", self.tree("9" * 40))])
        errors, _, _ = self.ws.check(build_records=records)
        self.assertTrue(any("is pinned by no row" in e for e in errors), errors)

    def test_a_record_that_says_nothing_about_cleanliness_fails(self):
        source = {"path": "P:/build/x", "git": True, "commit": self.ws.tool_commit}
        records = self.records([("tool", source)])
        errors, _, _ = self.ws.check(build_records=records)
        self.assertTrue(any("does not say whether the tree was clean" in e for e in errors), errors)

    def test_a_tree_that_is_no_checkout_fails(self):
        records = self.records([("tool", {"path": "P:/build/loose", "git": False})])
        errors, _, _ = self.ws.check(build_records=records)
        self.assertTrue(any("is no git checkout" in e for e in errors), errors)

    def test_no_build_record_at_all_fails(self):
        errors, _, _ = self.ws.check(build_records=self.records([]))
        self.assertTrue(any("no recipe.json under" in e for e in errors), errors)

    def test_missing_recipe_fails(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["recipe"] = "missing.ps1"
        self.assertFails(m, "recipe missing.ps1 does not exist")

    def test_unknown_placeholder_fails(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["args"].append("{input:absent}")
        self.assertFails(m, "unknown placeholder {input:absent}")

    def test_inputs(self):
        m = self.manifest()
        b = self.file(m, "payload/built.dll")["built_from"]
        b["inputs"] = {"engine": {"repo": "engine", "commit": self.ws.engine_commit},
                       "caps": {"inline": {"a": 1}}, "data": {"repo": "tool", "commit": self.ws.tool_commit,
                                                              "paths": ["data"]}}
        b["args"] += ["{input:engine}", "{input:caps}", "{input:data}/icd.json", "{payload:payload/icd.json}"]
        errors, _, _ = self.ws.check(m)
        self.assertEqual(errors, [])
        b["inputs"]["data"]["commit"] = "1" * 40
        self.assertFails(m, "input data: tool 11111111 is in no checkout")

    def test_the_kmd_source_text_starts_with_its_build_commit(self):
        # scratch/m15/native-caps001/release-baseline.py reads the commit at the start of sources.kmd with re.match
        # and the KMD ABI at that commit. Round 9 of tester.27 began the text with a sentence, and the pin-baseline
        # arm refused the installed package on the lab.
        m = self.manifest()
        self.file(m, "payload/driver.sys")["path"] = "payload/kmd/bc250kmd.sys"
        commit = self.ws.tool_commit
        m['files'].append(self.ws.entry('payload/kmd/amdgpu-state.bin', 'state.bin',
            built_from={'repo': 'tool', 'commit': commit, 'path': 'driver/state/amdgpu-state.bin'}))
        m["kmd_abi"] = "0x000700D8"
        m["sources"] = {"kmd": f"bc250-win {commit} (train/b29), the b29 train driver 0.7.216.28. New in round 9."}
        errors, _, _ = self.ws.check(m)
        self.assertEqual(errors, [])
        m["sources"]["kmd"] = f"bc250-win {commit[:8]} (train/b29), the short form release-baseline.py also reads."
        errors, _, _ = self.ws.check(m)
        self.assertEqual(errors, [])
        m["sources"]["kmd"] = f"New in round 9 of this release, the journal (bc250-win {commit[:8]}): more text."
        self.assertFails(m, "sources.kmd: must start with 'bc250-win <commit>'")
        del m["sources"]["kmd"]
        self.assertFails(m, "sources.kmd: must start with 'bc250-win <commit>'")
        m["sources"]["kmd"] = "bc250-win 0123abcd (train/b29), a commit that is not the build of the row."
        self.assertFails(m, "sources.kmd: names bc250-win 0123abcd, but payload/kmd/bc250kmd.sys is built from tool")
        m["sources"]["kmd"] = f"bc250-win {commit} (train/b29)."
        m["kmd_abi"] = "0x000700D7"
        self.assertFails(m, "sources.kmd: kmd_abi 0x000700D7 is not BC250_KMD_VERSION 0x000700D8")
        # A payload without the row has no text for the lab tool to read.
        errors, _, _ = self.ws.check(self.manifest())
        self.assertEqual(errors, [])

    def test_third_party_needs_https(self):
        m = self.manifest()
        self.file(m, "payload/tool.exe")["third_party"]["url"] = "http://example.com/tool.zip"
        self.assertFails(m, "third_party.url must be an https URL")

    def test_package_copies(self):
        self.fresh_workspace()
        package = Path(self.tmp) / "package"
        for f in self.ws.manifest["files"]:
            dst = package / f["path"]
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.ws.root / f["source"], dst)
        errors, _, _ = self.ws.check(package=package)
        self.assertEqual(errors, [])
        # The release signs the driver again: another signature, the same image.
        (package / "payload" / "driver.sys").write_bytes(fake_pe(b"\x01" * 64, b"release signature, longer"))
        errors, _, _ = self.ws.check(package=package)
        self.assertEqual(errors, [])
        # An edit of the packaged data file needs its release_edits field.
        (package / "payload" / "icd.json").write_bytes(b'{"icd": 2}\n')
        self.assertFails(None, "the packaged copy is", package=package)
        m = self.manifest()
        self.file(m, "payload/icd.json")["release_edits"] = "the release changes the version field"
        errors, _, _ = self.ws.check(m, package=package)
        self.assertEqual(errors, [])
        # Another image under the release signature fails.
        (package / "payload" / "driver.sys").write_bytes(fake_pe(b"\x02" * 64, b"release signature"))
        self.assertFails(m, "Authenticode digest of", package=package)

    def test_driverstore_manifest_edit_is_exact(self):
        self.fresh_workspace()
        m = self.manifest()
        data = b'{"ICD":{"library_path":"original.dll","api_version":"1.3.0"}}'
        src = self.ws.root / 'scratch/ds-icd.json'
        src.write_bytes(data)
        m['files'] = [{
            'component': 'vulkan', 'path': 'payload/vulkan/radeon_icd.json',
            'source': 'scratch/ds-icd.json', 'sha256': sha(data),
            'third_party': {'url': 'https://example.com/icd.json', 'version': '1', 'sha256': sha(data)}}]
        package = Path(self.tmp) / 'driverstore'
        (package / 'installer').mkdir(parents=True)
        # An untrusted report cannot redirect the provenance comparison.
        (package / 'installer/driver-package.json').write_text('{"members":[]}')
        dst = package / 'payload/kmd/vulkan/radeon_icd.json'
        dst.parent.mkdir(parents=True)
        expected = json.loads(data)
        expected['ICD']['library_path'] = '.\\vulkan_radeon.dll'
        dst.write_bytes((json.dumps(expected, indent=2) + '\n').encode())
        errors, _, _ = self.ws.check(m, package)
        self.assertEqual(errors, [])
        expected['ICD']['api_version'] = '1.4.0'
        dst.write_bytes((json.dumps(expected, indent=2) + '\n').encode())
        self.assertFails(m, 'unexpected ICD manifest edit', package=package)

    def test_the_commits_of_the_manifest_exist(self):
        errors, notes, checked = self.ws.commits()
        self.assertEqual(errors, [])
        self.assertEqual(notes, [])
        self.assertGreaterEqual(checked, 5)

    def test_a_commit_of_a_field_that_is_no_object_fails(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["commit"] = "0" * 40
        errors, _, _ = self.ws.commits(m)
        self.assertTrue(any("files[0].built_from.commit" in e and "is no object of" in e for e in errors), errors)

    def test_a_commit_inside_a_text_that_is_no_object_fails(self):
        """The fault of train b34: a stale id in prose, which no field repeats and the short form hides."""
        m = self.manifest()
        self.file(m, "payload/built.dll")["role"] = f"the shell (tool {'a' * 40}, train/b34)"
        errors, _, _ = self.ws.commits(m)
        self.assertTrue(any("files[0].role (text)" in e and "a" * 40 in e for e in errors), errors)

    def test_a_commit_inside_a_text_that_is_an_object_passes(self):
        m = self.manifest()
        self.file(m, "payload/built.dll")["role"] = f"the shell (tool {self.ws.tool_commit}, train/b34)"
        self.assertEqual(self.ws.commits(m)[0], [])

    def test_recipe_commit_is_read_as_one_of_the_recipe_repository(self):
        """A fork row's recipe_commit names this repository, never the fork, whatever `repo` the row carries."""
        m = self.manifest()
        self.file(m, "payload/engine.dll")["built_from"]["recipe_commit"] = self.ws.engine_commit
        errors, _, _ = self.ws.commits(m)
        self.assertTrue(any("recipe_commit" in e and "tool" in e for e in errors), errors)

    def test_an_unpublished_commit_passes_the_commit_check(self):
        """A lab candidate builds from a work tree, so nothing it names is published yet. The full gate refuses
        that (test_unpublished_commit_fails); this check asks only whether the object is there."""
        self.fresh_workspace()
        (self.ws.tool / "late.txt").write_text("late\n")
        git(self.ws.tool, "add", "-A")
        git(self.ws.tool, "commit", "-q", "-m", "not pushed")
        m = self.manifest()
        self.file(m, "payload/built.dll")["built_from"]["commit"] = git(self.ws.tool, "rev-parse", "HEAD")
        self.assertEqual(self.ws.commits(m)[0], [])

    def test_a_repository_with_no_checkout_here_is_a_note(self):
        m = self.manifest()
        m["repositories"]["engine"]["checkouts"] = [str(Path(self.tmp) / "nowhere")]
        errors, notes, _ = self.ws.commits(m)
        self.assertEqual(errors, [])
        self.assertTrue(any("engine: no checkout on this computer" in n for n in notes), notes)

    def test_the_release_stamps_nothing(self):
        """BD-166 as the lead and the reviewer settled it: each component build stamps its own output, the manifest pins
        the stamped file, and the release only copies it. So a packaged PE equals its pinned source whatever
        --package-version says, and a release that stamped the package again would fail here. The version and the
        flavour names are still read, because a package may not claim a version that does not map."""
        self.fresh_workspace()
        vs = provenance.load_versionstamp()
        spec = importlib.util.spec_from_file_location("test_versionstamp", Path(vs.__file__).with_name("test_versionstamp.py"))
        images = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(images)
        version = "0.7.216.102-tester.31"
        # The source as a component build leaves it: a PE that already carries the release's version resource.
        engine = images.make_pe(sections=((b".text", b"\xc3" * 64, 0x60000020),))
        source = self.ws.root / "scratch" / "engine.dll"
        source.write_bytes(engine)
        vs.stamp_file(source, version, "amdgpu-wddm", vs.load_table(), file_name="amdgpu_wddm_dxvk.dll")
        stamped = source.read_bytes()
        m = self.manifest()
        row = self.file(m, "payload/engine.dll")
        row["path"], row["sha256"] = "payload/d3d11/amdgpu_wddm_dxvk.dll", sha(stamped)
        package = Path(self.tmp) / "package"
        for f in m["files"]:
            dst = package / f["path"]
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.ws.root / f["source"], dst)
        (package / "payload" / "kmd").mkdir(parents=True, exist_ok=True)
        (package / "payload" / "kmd" / "bc250kmd.inf").write_text('[Strings]\nProvider = "amdgpu-wddm"\n', encoding="ascii")
        packaged = package / "payload" / "d3d11" / "amdgpu_wddm_dxvk.dll"
        errors, _, _ = self.ws.check(m, package=package, package_version=version)
        self.assertEqual(errors, [])
        # A release that stamped the packaged copy again changes bytes the manifest pins, and that is the fault.
        vs.stamp_file(packaged, "0.7.216.103-tester.32", "amdgpu-wddm", vs.load_table())  # the shipped name
        self.assertFails(m, "the packaged copy is", package=package, package_version=version)
        packaged.write_bytes(stamped)
        # The two names are read by the tool that owns them.
        self.assertFails(m, "version stamp", package=package, package_version="tester.31")
        self.assertFails(m, "flavour", package=package, package_version=version, package_flavour="labb")
        # The lab flavour of the same release passes here as well: this gate does not read resources, and
        # versionstamp.py check --signed is what tells a lab file from a release one.
        errors, _, _ = self.ws.check(m, package=package, package_version=version, package_flavour="lab")
        self.assertEqual(errors, [])


class RebuildCheckLogs(unittest.TestCase):
    """rebuild-check.ps1 on two recipes: one gives the recorded bytes, one fails. The failed build keeps its log at
    the path that its FAIL line names; the work trees of both builds go, and so does the directory of the match."""

    def setUp(self):
        if not shutil.which("pwsh"):
            self.skipTest("pwsh 7 is not on PATH")
        self.tmp = tempfile.mkdtemp(prefix="rebuild-", dir=OUT)
        self.ws = Workspace(self.tmp)

    def tearDown(self):
        remove_tree(self.tmp)

    def test_a_failed_build_keeps_its_log(self):
        ws = self.ws
        (ws.tool / "fail.ps1").write_bytes(b"Write-Output 'recipe ran: it fails on purpose'\nexit 1\n")
        (ws.tool / "make.ps1").write_bytes(b"param([string]$Out)\n[IO.File]::WriteAllBytes((Join-Path $Out "
                                           b"'built.dll'), [Text.Encoding]::ASCII.GetBytes('built bytes'))\n")
        git(ws.tool, "add", "-A")
        git(ws.tool, "commit", "-q", "-m", "recipes")
        commit = git(ws.tool, "rev-parse", "HEAD")
        m = copy.deepcopy(ws.manifest)
        # The script passes its own repository for "{repo}", so the test names the checkout.
        m["repositories"]["tool"]["checkouts"] = [str(ws.tool)]
        built = next(f for f in m["files"] if f["path"] == "payload/built.dll")
        built["built_from"] = {"repo": "tool", "commit": commit, "recipe": "make.ps1", "args": ["-Out", "{out}"],
                               "output": "built.dll"}
        broken = copy.deepcopy(built)
        broken["path"] = "payload/broken.dll"
        broken["built_from"]["recipe"] = "fail.ps1"
        m["files"].append(broken)
        ws.manifest_path.write_text(json.dumps(m), encoding="utf-8")
        work = Path(self.tmp) / "rb"
        r = subprocess.run(["pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("rebuild-check.ps1")),
                            "-Sources", str(ws.manifest_path), "-Root", str(ws.root),
                            "-Path", "payload/built.dll,payload/broken.dll", "-Work", str(work)],
                           capture_output=True, text=True, timeout=300)
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 1, out)
        self.assertTrue(any(l.startswith("OK   match") and "payload/built.dll" in l for l in r.stdout.splitlines()), out)
        fail = [l for l in r.stdout.splitlines() if l.startswith("FAIL build-failed") and "payload/broken.dll" in l]
        self.assertEqual(len(fail), 1, out)
        log = Path(fail[0].split("(see ", 1)[1].rstrip(")"))
        self.assertIn("recipe ran: it fails on purpose", log.read_text(encoding="utf-8-sig", errors="replace"))
        self.assertEqual([p.name for p in log.parent.iterdir()], ["build-first.log"])
        self.assertEqual(list(work.iterdir()), [log.parent])
        self.assertEqual(len(git(ws.tool, "worktree", "list").splitlines()), 1)


class AuthenticodeDigest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="authenticode-", dir=OUT))

    def tearDown(self):
        remove_tree(self.tmp)

    def digest(self, data):
        p = self.tmp / "image.sys"
        p.write_bytes(data)
        return provenance.authenticode_digest(p)

    def test_signature_and_checksum_do_not_count(self):
        body = bytes(range(256)) * 4
        unsigned = self.digest(fake_pe(body))
        self.assertEqual(self.digest(fake_pe(body, b"signature one")), unsigned)
        self.assertEqual(self.digest(fake_pe(body, b"another, longer signature")), unsigned)
        self.assertNotEqual(self.digest(fake_pe(body[:-1] + b"\x00")), unsigned)

    def test_not_a_pe_file(self):
        with self.assertRaises(ValueError):
            self.digest(b"plain text")


class StateCatalogInputsTests(unittest.TestCase):
    def setUp(self):
        self.paths = ['payload/tools/bc250kmd_cli.exe', 'payload/tools/amdgpu-state-installer.exe',
                      'payload/control/bc250control.dll', 'payload/tools/bc250control.dll',
                      'payload/desktop/bc250d3d_router.dll', 'payload/wow64/desktop/bc250d3d_router.dll',
                      'payload/kmd/bc250kmd.sys']
        self.sources = {'recipe_repository': 'driver', 'files': [self.row(p) for p in
                        ['payload/kmd/amdgpu-state.bin'] + self.paths]}
        self.repos = Mock()
        self.repos.find.return_value = Path('fixture')
        self.headers = {'a' * 40: self.header('1', 182), 'b' * 40: self.header('2', 178),
                        'c' * 40: self.header('1', 182)}

    def row(self, path):
        return {'path': path, 'built_from': {'repo': 'driver', 'commit': 'a' * 40}}

    @staticmethod
    def header(value, count):
        return f'#define AMDGPU_STATE_CATALOG_ID "{value * 64}"\n#define AMDGPU_STATE_COUNT {count}u\n#define AMDGPU_STATE_SCHEMA_VERSION 2u'

    def check(self):
        def read(checkout, verb, spec):
            self.assertEqual(verb, 'show')
            commit, path = spec.split(':', 1)
            self.assertEqual(path, 'driver/state/catalog.generated.h')
            return (0, self.headers[commit]) if commit in self.headers else (1, '')
        with patch.object(provenance, 'git', side_effect=read):
            return provenance.state_catalog_inputs_ok(self.sources, self.repos)

    def test_same_catalog_across_different_commits(self):
        self.sources['files'][1]['built_from']['commit'] = 'c' * 40
        self.assertEqual(self.check(), [])

    def test_each_consumer_with_stale_header_is_named(self):
        for row in self.sources['files'][1:]:
            row['built_from']['commit'] = 'b' * 40
        errors = self.check()
        self.assertEqual(len(errors), len(self.paths))
        for path in self.paths:
            self.assertEqual(sum(path in error for error in errors), 1)

    def test_rebuilt_cli_does_not_hide_old_router(self):
        self.sources['files'][-2]['built_from']['commit'] = 'b' * 40
        self.assertEqual(len(self.check()), 1)
        self.assertIn('payload/wow64/desktop/bc250d3d_router.dll', self.check()[0])

    def test_windows_path_case_cannot_hide_stale_consumer(self):
        self.sources['files'][0]['path'] = self.sources['files'][0]['path'].upper()
        self.sources['files'][1]['path'] = self.sources['files'][1]['path'].upper()
        self.sources['files'][1]['built_from']['commit'] = 'b' * 40
        errors = self.check()
        self.assertEqual(len(errors), 1)
        self.assertIn(self.sources['files'][1]['path'], errors[0])

    def test_missing_or_duplicate_anchor_refused(self):
        anchor = self.sources['files'].pop(0)
        self.assertTrue(self.check())
        self.sources['files'] += [anchor, anchor]
        self.assertTrue(self.check())

    def test_missing_origin_or_foreign_repository_refused(self):
        self.sources['files'][1].pop('built_from')
        self.assertTrue(self.check())
        self.sources['files'][1]['built_from'] = {'repo': 'other', 'commit': 'a' * 40}
        self.assertTrue(self.check())

    def test_missing_commit_or_header_refused(self):
        self.repos.find.return_value = None
        self.assertTrue(self.check())
        self.repos.find.return_value = Path('fixture')
        self.headers.clear()
        self.assertTrue(self.check())

    def test_malformed_or_duplicate_header_refused(self):
        for text in ['', self.header('1', 182) + '\n#define AMDGPU_STATE_COUNT 182u']:
            self.headers['a' * 40] = text
            self.assertTrue(self.check())

    def test_same_id_different_count_or_schema_refused(self):
        self.sources['files'][1]['built_from']['commit'] = 'c' * 40
        self.headers['c' * 40] = self.header('1', 178)
        self.assertTrue(self.check())
        self.headers['c' * 40] = self.header('1', 182).replace('VERSION 2u', 'VERSION 3u')
        self.assertTrue(self.check())

    def test_unrelated_payload_has_no_state_requirement(self):
        self.sources['files'] = [self.row('payload/tool.exe')]
        self.assertEqual(self.check(), [])
        self.repos.find.assert_not_called()


if __name__ == "__main__":
    unittest.main()
