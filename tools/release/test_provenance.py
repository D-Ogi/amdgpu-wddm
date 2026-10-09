"""Self-test of the release provenance gate (tools/release/provenance.py).

    python -m unittest discover -s tools/release -p test_provenance.py   # the 'release-provenance' gate of quick.ps1

Each test makes throwaway git repositories: a recipe repository with a bare "published" remote, a fork with a
submodule, and a workspace root with the payload source files. A complete manifest must pass, and each defect the
gate exists for must fail with its own message: an entry without provenance, a commit that no remote-tracking branch
of the repository's URL holds, a submodule commit that is not published, a source or data file with other bytes, an
"unverified" entry without a reason, a recipe that the recipe commit does not have, an unknown argument placeholder,
a download that is not https, a packaged copy that is not its source, and a re-signed driver whose Authenticode digest
changed. The Authenticode digest itself must ignore the checksum, the security directory and the certificate table.
BC250_TEST_OUT, when set, holds the temporary trees (quick.ps1 keeps them off drive C:).
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest

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
        self.tool = self.make_repo("tool", {"build.ps1": b"# recipe\n", "data/icd.json": b'{"icd": 1}\n'})
        self.tool_commit = git(self.tool, "rev-parse", "HEAD")
        (self.root / "scratch").mkdir()
        self.files = {
            "built.dll": b"built bytes",
            "icd.json": b'{"icd": 1}\n',
            "tool.exe": b"downloaded bytes",
            "engine.dll": b"engine bytes",
            "driver.sys": fake_pe(b"\x01" * 64, b"lab signature"),
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

    def check(self, manifest=None, package=None):
        self.manifest_path.write_text(json.dumps(manifest or self.manifest), encoding="utf-8")
        errors, rows, counts = provenance.check(self.manifest_path, self.root, self.tool, package)
        return errors, rows, counts


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

    def assertFails(self, manifest, text, package=None):
        errors, _, _ = self.ws.check(manifest, package)
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
        self.file(m, "payload/built.dll")["unverified"] = "built before /Brepro"
        errors, rows, counts = self.ws.check(m)
        self.assertEqual(errors, [])
        self.assertEqual(counts["unverified"], 1)
        self.assertEqual(counts["reproducible"], 2)

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


if __name__ == "__main__":
    unittest.main()
