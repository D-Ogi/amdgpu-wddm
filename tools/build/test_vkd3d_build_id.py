"""The vkd3d build id gate: an engine whose git describe failed must not build, and the release must not take it.

Each case makes a small git repository with the vcs_tag block of vkd3d-proton's meson.build. The negative
controls are the failure of 2026-10-28 (a submodule whose .git file names a missing gitdir, so git describe
--dirty stops), a dirty tree, and a vkd3d_build.h that holds the fallback 0x12345678.
"""
import contextlib
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "release"))

import vkd3d_build_id as g  # noqa: E402
from provenance import vkd3d_build_id_ok  # noqa: E402

OUT = os.environ.get("BC250_TEST_OUT") or None
# The block of vkd3d-proton meson.build (amdgpu-wddm/main 2474e369), as the gate reads it there.
MESON = """project('probe', 'c')
vkd3d_build = vcs_tag(
  command : ['git', 'describe', '--always', '--exclude=*', '--abbrev=15', '--dirty=0'],
  input   : 'vkd3d_build.h.in',
  output  : 'vkd3d_build.h', fallback : '12345678')
"""
ENV = dict(os.environ, GIT_AUTHOR_NAME="probe", GIT_AUTHOR_EMAIL="probe@example.com",
           GIT_COMMITTER_NAME="probe", GIT_COMMITTER_EMAIL="probe@example.com", GIT_CONFIG_NOSYSTEM="1")


def git(tree, *args):
    r = subprocess.run(["git", "-C", str(tree), "-c", "protocol.file.allow=always", "-c", "core.autocrlf=false"]
                       + list(args), capture_output=True, text=True, env=ENV)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr}")
    return r.stdout.strip()


def header(path, value):
    Path(path).write_text(f"#include <stdint.h>\n\nstatic const uint64_t vkd3d_build = 0x{value};\n")


class BuildIdGate(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="vkd3d-build-id-", dir=OUT))
        self.src = self.dir / "src"
        self.src.mkdir()
        git(self.src, "init", "-q")
        (self.src / "meson.build").write_text(MESON)
        git(self.src, "add", "meson.build")
        git(self.src, "commit", "-q", "-m", "probe")
        self.commit = git(self.src, "rev-parse", "HEAD")

    def tearDown(self):
        # git writes its object files read-only, and Windows does not delete a read-only file.
        def writable(func, path, _exc):
            os.chmod(path, stat.S_IWRITE)
            func(path)
        shutil.rmtree(self.dir, onexc=writable)

    def test_vcs_tag_block_of_the_fork(self):
        tag = g.read_vcs_tag(MESON)
        self.assertEqual(tag["fallback"], "12345678")
        self.assertEqual(tag["dirty_mark"], "0")
        self.assertEqual(tag["command"][:2], ["git", "describe"])

    def test_clean_tree_gives_the_commit_prefix(self):
        e = g.expect(self.src)
        self.assertFalse(e["dirty"])
        self.assertEqual(len(e["build_id"]), 15)
        self.assertTrue(self.commit.startswith(e["build_id"]))

    def test_dirty_tree_is_refused_unless_allowed(self):
        (self.src / "meson.build").write_text(MESON + "# change\n")
        with self.assertRaisesRegex(g.BuildIdError, "has changes"):
            g.expect(self.src)
        e = g.expect(self.src, allow_dirty=True)
        self.assertTrue(e["dirty"])
        self.assertEqual(e["build_id"], self.commit[:15] + "0")

    def test_broken_submodule_link_is_refused(self):
        # Negative control: the dev engine trees of 2026-10-28 had submodule .git files that named a gitdir which
        # no longer existed. git describe --dirty then exits 128 and vcs_tag writes the fallback id.
        sub = self.dir / "sub"
        sub.mkdir()
        git(sub, "init", "-q")
        (sub / "f").write_text("x")
        git(sub, "add", "f")
        git(sub, "commit", "-q", "-m", "sub")
        git(self.src, "submodule", "add", "-q", str(sub), "subprojects/sub")
        git(self.src, "commit", "-q", "-m", "submodule")
        self.assertTrue(self.src.joinpath("subprojects", "sub", ".git").is_file())
        g.expect(self.src)  # positive control: the same tree with a working link
        link = self.src / "subprojects" / "sub" / ".git"
        link.unlink()  # git makes the file hidden on Windows, and a hidden file cannot be opened for writing
        link.write_text("gitdir: ../../../gone/modules/sub\n")
        with self.assertRaisesRegex(g.BuildIdError, "fallback build id 0x12345678"):
            g.expect(self.src)

    def test_header_gate(self):
        e = g.expect(self.src)
        h = self.dir / "vkd3d_build.h"
        header(h, e["build_id"])
        dll = self.dir / "engine.dll"
        dll.write_bytes(b"MZ" + struct.pack("<Q", int(e["build_id"], 16)) * 2)
        r = g.check(h, e, dll, require_in_dll=True)
        self.assertEqual(r["build_id"], e["build_id"])
        self.assertEqual(r["dll_occurrences"], 2)
        # Negative controls: the fallback, another commit's id, and a DLL without the id.
        header(h, "12345678")
        with self.assertRaisesRegex(g.BuildIdError, "fallback"):
            g.check(h, e)
        header(h, "0123456789abcde")
        with self.assertRaisesRegex(g.BuildIdError, "not 0x"):
            g.check(h, e)
        header(h, e["build_id"])
        dll.write_bytes(b"MZ" + struct.pack("<I", 0x12345678))
        with self.assertRaisesRegex(g.BuildIdError, "holds no 64-bit constant"):
            g.check(h, e, dll, require_in_dll=True)

    def test_cli_fails_on_fallback_header(self):
        e = g.expect(self.src)
        exp = self.dir / "expect.json"
        exp.write_text(json.dumps(e))
        h = self.dir / "vkd3d_build.h"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            header(h, "12345678")
            self.assertEqual(g.main(["check", "--header", str(h), "--expect", str(exp)]), 1)
            header(h, e["build_id"])
            self.assertEqual(g.main(["check", "--header", str(h), "--expect", str(exp)]), 0)


class ReleaseRecordGate(unittest.TestCase):
    COMMIT = "d33faf3cccfd4cf9f2ae8fd3789c911546314735"

    def errors(self, build_id):
        errors = []
        record = {"recipe": "vkd3d"}
        if build_id is not None:
            record["build_id"] = build_id
        vkd3d_build_id_ok("record", record, self.COMMIT, errors)
        return errors

    def test_release_record(self):
        self.assertEqual(self.errors({"built": "d33faf3cccfd4cf", "dirty": False, "fallback": "12345678"}), [])
        self.assertTrue(self.errors(None))
        self.assertTrue(self.errors({"built": "12345678", "dirty": False, "fallback": "12345678"}))
        self.assertTrue(self.errors({"built": "d33faf3cccfd4cf0", "dirty": True, "fallback": "12345678"}))
        self.assertTrue(self.errors({"built": "0123456789abcde", "dirty": False, "fallback": "12345678"}))


if __name__ == "__main__":
    unittest.main()
