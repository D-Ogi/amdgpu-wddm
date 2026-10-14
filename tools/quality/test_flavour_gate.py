"""Self-test of the BD-167 flavour gate (tools/quality/flavour_gate.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Synthetic PE images: a release set must pass as release and fail as lab, a lab set the other way round. One
escape marker, one forbidden name in ASCII or in UTF-16LE, or the wrong flavour line must fail the release set; a
longer identifier that only contains a forbidden name must not, and an out-of-scope name is recorded, not failed.
A lab set fails when one named file lacks one of its lab-only names or is missing, so a build that lost the lab
switch cannot pass in silence. Every input the gate cannot judge must fail closed. The declaration next to the gate must load, and every KMD marker in it must still be a literal of the
driver source, so that a renamed log line fails here and not first on the lab.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest

import flavour_gate as g

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]
DECL = g.load_declaration(g.DECLARATION)
K = DECL["kmd"]


def pe(*strings, utf16=()):
    """A minimal image: MZ header, e_lfanew 0x40, PE signature, then the strings, each NUL-terminated."""
    head = bytearray(b"MZ" + b"\0" * 0x3a) + (0x40).to_bytes(4, "little") + b"PE\0\0"
    body = b"".join(s.encode("ascii") + b"\0" for s in strings)
    body += b"".join(s.encode("utf-16-le") + b"\0\0" for s in utf16)
    return bytes(head) + b"\0" * 16 + body


RELEASE_KMD = (K["flavour_markers"]["release"], "kmd: something every flavour logs")
LAB_KMD = (K["flavour_markers"]["lab"],) + tuple(e["marker"] for e in K["lab_escape_markers"])


def lab_files():
    """{file name: [forbidden names its lab build must hold]} from the declaration's lab.every lists."""
    files = {}
    for f in DECL["forbidden"]:
        for name in f["lab"].get("every", []):
            files.setdefault(name, []).append(f["name"])
    return files


class ReleaseFlavour(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)

    def write(self, name, data):
        p = self.dir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def run_gate(self, *args):
        with contextlib.redirect_stdout(io.StringIO()):
            return g.main(list(args))

    def release_set(self, umd=pe("AMDGPU_WDDM_LOG", "DXVK_HUD")):
        kmd = self.write("pkg/bc250kmd.sys", pe(*RELEASE_KMD))
        dll = self.write("pkg/umd/amdgpu_wddm_d3d11.dll", umd)
        return kmd, dll

    def test_release_set_passes_as_release_only(self):
        self.release_set()
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def lab_set(self, drop_name=None, drop_file=None):
        """A lab package: the lab KMD and every file named in a lab.every list, each holding its names, twice (the
        x64 and wow64 trees have one copy each), less one name of one copy or one file name when asked."""
        self.write("pkg/bc250kmd.sys", pe(*LAB_KMD))
        for i, (name, names) in enumerate(sorted(lab_files().items())):
            if name == drop_file:
                continue
            for tree in ("x64", "wow64"):
                held = [n for n in names if drop_name != (tree, name, n)]
                self.write("pkg/%s/%d/%s" % (tree, i, name), pe("AMDGPU_WDDM_LOG", *held))

    def test_lab_set_passes_as_lab_only(self):
        self.lab_set()
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 0)
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_lab_file_without_one_lab_name_fails(self):
        # One build of the set lost the lab switch for one lookup (or the lookup moved): the lab arm must fail.
        for file_name, names in sorted(lab_files().items()):
            for n in names:
                with self.subTest(file=file_name, name=n):
                    self.setUp()
                    self.lab_set(drop_name=("wow64", file_name, n))
                    self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_lab_set_without_a_named_file_fails(self):
        for file_name in sorted(lab_files()):
            with self.subTest(file=file_name):
                self.setUp()
                self.lab_set(drop_file=file_name)
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_lab_kmd_only_skips_the_umd_names(self):
        kmd = self.write("bc250kmd.sys", pe(*LAB_KMD))
        self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(kmd), "--kmd-only"), 0)

    def test_every_forbidden_name_has_a_lab_expectation(self):
        for f in DECL["forbidden"]:
            with self.subTest(name=f["name"]):
                self.assertTrue(f["lab"].get("every") or f["lab"].get("none", "").strip())

    def test_out_of_scope_name_is_recorded_not_failed(self):
        self.assertTrue(DECL.get("out_of_scope"), "the declaration has no out-of-scope entry to test with")
        name = DECL["out_of_scope"][0]["name"]
        self.release_set(pe("AMDGPU_WDDM_LOG", name))
        out = self.dir / "out"
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"), "--out", str(out)), 0)
        record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
        self.assertIn(name, [n for f in record["files"] for n in f["out_of_scope"]])

    def test_one_escape_marker_fails_release(self):
        for e in K["lab_escape_markers"]:
            with self.subTest(marker=e["marker"]):
                kmd = self.write("bc250kmd.sys", pe(*RELEASE_KMD, e["marker"]))
                self.assertEqual(self.run_gate("--flavour", "release", "--kmd", str(kmd), "--kmd-only"), 1)

    def test_lab_without_one_marker_fails(self):
        for e in K["lab_escape_markers"]:
            with self.subTest(marker=e["marker"]):
                kmd = self.write("bc250kmd.sys", pe(*[s for s in LAB_KMD if s != e["marker"]]))
                self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(kmd), "--kmd-only"), 1)

    def test_marker_prefix_is_not_the_marker(self):
        # "raw escape RUN_DCN" is a prefix of "raw escape RUN_DCNFLIP": only the whole C string counts.
        markers = [e["marker"] for e in K["lab_escape_markers"]]
        longer = [(a, b) for a in markers for b in markers if a != b and b.startswith(a)]
        self.assertTrue(longer, "the declaration no longer has a marker that prefixes another one")
        for short, long_ in longer:
            data = pe(*RELEASE_KMD, long_)
            self.assertFalse(g.has_text(data, short))
            self.assertTrue(g.has_text(data, long_))

    def test_forbidden_name_fails_release_in_either_encoding(self):
        for f in DECL["forbidden"]:
            with self.subTest(name=f["name"]):
                self.release_set(pe("AMDGPU_WDDM_LOG", f["name"]))
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
                self.release_set(pe("AMDGPU_WDDM_LOG", utf16=(f["name"],)))
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_name_inside_a_message_counts(self):
        self.release_set(pe("* WARNING: RADV_DEBUG=hang is costly *"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_longer_identifier_is_not_the_name(self):
        self.release_set(pe("MY_RADV_DEBUG_LEVEL", "XYGALLIUM_DRIVER", utf16=("RADV_DEBUGGER",)))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)

    def test_stray_neighbour_byte_does_not_hide_a_name(self):
        # One alignment byte that happens to be a letter, or the tail of an ASCII string right before a UTF-16 one.
        data = pe("x") + b"\x01A" + b"RADV_DEBUG\0" + b"LOG\0" + "ZINK_DEBUG".encode("utf-16-le") + b"\0\0"
        self.assertEqual(g.has_name(data, "RADV_DEBUG"), (1, 0))
        self.assertEqual(g.has_name(data, "ZINK_DEBUG"), (0, 1))

    def test_forbidden_name_in_the_kmd_fails_release(self):
        self.write("pkg/bc250kmd.sys", pe(*RELEASE_KMD, "GALLIUM_DRIVER"))
        self.write("pkg/a.dll", pe("x"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_out_records_hashes_and_findings(self):
        self.release_set(pe("DXVK_CONFIG_FILE"))
        out = self.dir / "out"
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"), "--out", str(out)), 1)
        record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
        self.assertEqual(len(record["files"]), 2)
        self.assertTrue(all(len(f["sha256"]) == 64 for f in record["files"]))
        self.assertIn("DXVK_CONFIG_FILE", [n for f in record["files"] for n in f["forbidden"]])

    def test_fails_closed(self):
        kmd, dll = self.release_set()
        text = self.write("pkg/readme.dll", b"not a PE image at all, only text")
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 2)
        text.unlink()
        cases = {
            "no KMD": ["--kmd", str(self.dir / "missing.sys"), "--pe", str(dll)],
            "no UMD without --kmd-only": ["--kmd", str(kmd)],
            "empty directory": ["--kmd", str(kmd), "--pe", str(self.write("empty/x.txt", b"").parent)],
            "no input": [],
            "two KMD files": ["--package", str(self.dir)],
        }
        self.write("other/bc250kmd.sys", pe(*RELEASE_KMD))
        for name, args in cases.items():
            with self.subTest(case=name):
                self.assertEqual(self.run_gate("--flavour", "release", *args), 2)
        with self.assertRaises(SystemExit):
            self.run_gate("--package", str(self.dir / "pkg"))          # no flavour named
        with self.assertRaises(SystemExit):
            self.run_gate("--flavour", "beta", "--package", str(self.dir / "pkg"))

    def test_malformed_declaration_fails_closed(self):
        self.release_set()
        good = json.loads(g.DECLARATION.read_text(encoding="utf-8"))
        def broken(change):
            d = json.loads(json.dumps(good))
            change(d)
            p = self.write("decl.json", json.dumps(d).encode("utf-8"))
            return self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"), "--declaration", str(p))
        cases = {
            "name in both lists": lambda d: d["allowed"].append(dict(d["forbidden"][0], reason="x")),
            "no reason": lambda d: d["allowed"][0].update(reason=" "),
            "no does": lambda d: d["forbidden"][0].pop("does"),
            "duplicate name": lambda d: d["forbidden"].append(d["forbidden"][0]),
            "empty forbidden list": lambda d: d.update(forbidden=[]),
            "no escape markers": lambda d: d["kmd"].update(lab_escape_markers=[]),
            "same flavour lines": lambda d: d["kmd"]["flavour_markers"].update(lab=d["kmd"]["flavour_markers"]["release"]),
            "bad name": lambda d: d["forbidden"][0].update(name="radv_debug"),
            "schema": lambda d: d.update(schema=2),
            "no lab field": lambda d: d["forbidden"][0].pop("lab"),
            "lab with both keys": lambda d: d["forbidden"][0].update(lab={"every": ["a.dll"], "none": "x"}),
            "lab.every empty": lambda d: d["forbidden"][0].update(lab={"every": []}),
            "lab.every with a path": lambda d: d["forbidden"][0].update(lab={"every": ["d3d12/a.dll"]}),
            "lab.every upper case": lambda d: d["forbidden"][0].update(lab={"every": ["A.dll"]}),
            "lab.none without reason": lambda d: d["forbidden"][0].update(lab={"none": " "}),
            "forbidden and out of scope": lambda d: d["out_of_scope"].append(
                {"name": d["forbidden"][0]["name"], "where": "x", "reason": "x"}),
        }
        for name, change in cases.items():
            with self.subTest(case=name):
                self.assertEqual(broken(change), 2)
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"),
                                       "--declaration", str(self.write("bad.json", b"{"))), 2)

    def test_kmd_markers_are_literals_of_the_driver(self):
        kmd = REPO / "driver" / "kmd"
        text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted(kmd.glob("*.c")))
        for marker in list(K["flavour_markers"].values()) + [e["marker"] for e in K["lab_escape_markers"]]:
            with self.subTest(marker=marker):
                self.assertIn('"%s"' % marker, text)


if __name__ == "__main__":
    unittest.main()
