"""Self-test of the BD-167 flavour gate (tools/quality/flavour_gate.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Synthetic PE images: a release set must pass as release and fail as lab, a lab set the other way round. One
escape marker, one forbidden name in ASCII or in UTF-16LE (each declared name in turn, with or without the driver's
prefixes, and the record names it), or the wrong flavour line must fail the release set; a
longer identifier that only contains a forbidden name must not, and an out-of-scope name is recorded, not failed.
A lab set fails when one named file lacks one of its lab-only names or is missing, so a build that lost the lab
switch cannot pass in silence. An item of lab.every with a payload path names that copy only, and an unlisted copy of
the same file that holds the name fails the lab set. Every input the gate cannot judge must fail closed. The declaration next to the gate must load, and every KMD marker in it must still be a literal of the
driver source, so that a renamed log line fails here and not first on the lab. Coverage: one identifier of the
driver's own prefixes that no list declares fails either flavour (the negative control), in ASCII, in UTF-16LE and
in the KMD, and a declared one passes.

Letter case: a forbidden name or a prefixed identifier as a whole C string in another case counts, inside prose it
does not, and file names match in any case. A lab.none name in a lab file fails the lab set. The release rule of the
game-log sink and an open decision fail a release set. A second .sys, two files with one payload path and a file with
a PE header but no PE suffix are handled (fail closed, or scanned). A DriverStore member has its input slot's payload
path. A whole package bound to its manifest passes in both layouts and both flavours; a missing, extra, changed or
unlisted PE fails it; the negative control is seen, and a scan that cannot see it fails. The package slots of the
declaration are the PE rows of release-sources.json, and build-release.ps1 runs the gate on the payload and on the
bound package. Every Mesa, DXVK and vkd3d-proton config that a release row builds defines NDEBUG in its release
flavour, and a planted config without it fails. A PE with a live assert (an import of an assert function or from
a debug C runtime, or the static C runtime's assert message) fails a release set and not a lab set, and the
import directory of a real system DLL reads.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

import flavour_gate as g

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]
DECL = g.load_declaration(g.DECLARATION)
K = DECL["kmd"]


def pe(*strings, utf16=()):
    """A minimal image: MZ header, e_lfanew 0x40, PE signature, an empty COFF header (no optional header, so no
    imports), then the strings, each NUL-terminated."""
    head = bytearray(b"MZ" + b"\0" * 0x3a) + (0x40).to_bytes(4, "little") + b"PE\0\0"
    body = b"".join(s.encode("ascii") + b"\0" for s in strings)
    body += b"".join(s.encode("utf-16-le") + b"\0\0" for s in utf16)
    return bytes(head) + b"\0" * 20 + body


def pe_importing(*imports, strings=()):
    """A PE32+ image whose one section holds an import directory of (dll, function) pairs, then the strings."""
    rva, dlls = 0x1000, {}
    for dll, fn in imports:
        dlls.setdefault(dll, []).append(fn)
    section, cur = bytearray(0x800), 20 * (len(dlls) + 1)
    for i, (dll, fns) in enumerate(dlls.items()):
        thunks, cur = cur, cur + 8 * (len(fns) + 1)
        name, cur = cur, cur + len(dll) + 1
        section[name:cur] = dll.encode("ascii") + b"\0"
        for j, fn in enumerate(fns):
            struct.pack_into("<Q", section, thunks + 8 * j, rva + cur)
            section[cur:cur + len(fn) + 3] = b"\0\0" + fn.encode("ascii") + b"\0"
            cur += len(fn) + 3
        struct.pack_into("<IIIII", section, 20 * i, rva + thunks, 0, 0, rva + name, rva + thunks)
    head = bytearray(0x200)
    head[0:2], head[0x3c:0x40], head[0x40:0x44] = b"MZ", (0x40).to_bytes(4, "little"), b"PE\0\0"
    struct.pack_into("<HHIIIHH", head, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    struct.pack_into("<H", head, 0x58, 0x20b)
    struct.pack_into("<I", head, 0x58 + 108, 16)
    struct.pack_into("<II", head, 0x58 + 112 + 8, rva, 20 * (len(dlls) + 1))
    struct.pack_into("<8sIIIIIIHHI", head, 0x58 + 240, b".idata", 0x800, rva, 0x800, 0x200, 0, 0, 0, 0, 0xC0000040)
    return bytes(head) + bytes(section) + b"".join(s.encode("ascii") + b"\0" for s in strings)


RELEASE_KMD = (K["flavour_markers"]["release"], "kmd: something every flavour logs")
KMD_FILE = K["file"].lower()
# The forbidden names that only the lab KMD holds (lab.every names the KMD file): the synthetic lab KMD holds them,
# and lab_items() leaves the KMD out, because every set has its KMD apart from the lab_slots() copies.
KMD_NAMES = tuple(sorted(f["name"] for f in DECL["forbidden"] if KMD_FILE in f["lab"].get("every", [])))
# How the KMD source spells them: REG_QWORD and REG_DWORD value names, UTF-16LE strings in the SYS (BD-235).
KMD_SPELLING = {"PAGINGRINGSTARTBYTES": "PagingRingStartBytes", "PAGINGRINGSTARTNONCE": "PagingRingStartNonce",
                "PAGINGBOUNDARYMODE": "PagingBoundaryMode", "LABPAGINGRINGSTARTNONCE": "LabPagingRingStartNonce",
                "ENABLECOMPUTENODE": "EnableComputeNode", "LABGDIHWACCEL": "LabGdiHwAccel"}
LAB_KMD = (K["flavour_markers"]["lab"],) + tuple(e["marker"] for e in K["lab_escape_markers"]) + KMD_NAMES
# A file that reads the game-log sink, as the release rule of AMDGPU_WDDM_LOG wants it: the marker name and the folder.
LOG = ("AMDGPU_WDDM_LOG", "AMDGPU_WDDM_GAMELOG", "amdgpu-wddm\\logs\\games")
MEMBER_OF = {source: member.split("/", 1)[1] for member, source in g.driverstore_members().items()}


def lab_items():
    """{lab.every item (file name or payload path): [forbidden names that item names]}."""
    items = {}
    for f in DECL["forbidden"]:
        for item in f["lab"].get("every", []):
            if item != KMD_FILE:
                items.setdefault(item, []).append(f["name"])
    return items


def lab_slots():
    """{payload path: [forbidden names its lab build holds]}: two copies of each file named by file name (the x64
    and wow64 trees), one file at each payload path, each holding the names that name it."""
    items = lab_items()
    slots = {}
    for i, item in enumerate(sorted(items)):
        for s in ([item] if "/" in item else ["x64/%d/%s" % (i, item), "wow64/%d/%s" % (i, item)]):
            slots[s] = []
    for s in slots:
        base = s.rsplit("/", 1)[1]
        slots[s] = sorted({n for item, names in items.items() if item in (s, base) for n in names})
    return slots


def slot_of(item):
    """The payload path in lab_slots() of one copy that an item names (the wow64 copy of a file name)."""
    return item if "/" in item else "wow64/%d/%s" % (sorted(lab_items()).index(item), item)


# The file that build-release.ps1 gives the negative control: an x86 DriverStore member.
NEGATIVE = "wow64/d3d12/amdgpu_wddm_vkd3d.dll"


def package_names(flavour):
    """{package slot: [forbidden names its build holds]}: none in a release package; in a lab package, the names of
    the lab.every items that name the slot by payload path or by file name."""
    items = lab_items()
    result = {}
    for s in DECL["package"]["slots"]:
        b = s.rsplit("/", 1)[-1]
        result[s] = [] if flavour == "release" else sorted({n for i, names in items.items() if i in (s, b) for n in names})
    return result


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


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

    def release_set(self, umd=pe(*LOG, "DXVK_HUD")):
        kmd = self.write("pkg/bc250kmd.sys", pe(*RELEASE_KMD))
        dll = self.write("pkg/umd/amdgpu_wddm_d3d11.dll", umd)
        return kmd, dll

    def test_release_set_passes_as_release_only(self):
        self.release_set()
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_adl_test_exports_fail_both_flavours_including_wow64_decoration(self):
        hooks = ("Bc250AdlTest_SetSnapshot", "_Bc250AdlTest_Discover@4", "Bc250AdlTest_LiveBlocks",
                 "_bc250adltest_futurehook@12")
        self.lab_set()
        for flavour in ("release", "lab"):
            for hook in hooks:
                for utf16 in (False, True):
                    with self.subTest(flavour=flavour, hook=hook, utf16=utf16):
                        blob = pe(utf16=(hook,)) if utf16 else pe(hook)
                        if flavour == "release":
                            kmd, dll = self.release_set(blob)
                            failures, _, _ = g.check(DECL, flavour, kmd, [dll])
                            dll.unlink()
                        else:
                            self.write("pkg/adl/atiadlxy.dll", blob)
                            slots = {}
                            kmd, dlls = g.collect(str(self.dir / "pkg"), "", [], K["file"], slots)
                            failures, _, _ = g.check(DECL, flavour, kmd, dlls, slots=slots)
                        self.assertTrue(any("ADL test hook" in failure for failure in failures), failures)

    def test_adl_public_exports_are_not_test_hooks(self):
        names = ("ADL_Main_Control_Create", "ADL2_Main_Control_Create", "ADL2_Display_Modes_Get",
                 "_ADL2_Adapter_AdapterInfo_Get@12", "bc250kmd")
        kmd, dll = self.release_set(pe(*names))
        self.assertEqual(g.check(DECL, "release", kmd, [dll])[0], [])
        dll.unlink()  # The release-only D3D11 fixture must not become part of the lab set.
        self.lab_set(extra={"adl/atiadlxx.dll": list(names), "wow64/adl/atiadlxy.dll": list(names)})
        slots = {}
        kmd, dlls = g.collect(str(self.dir / "pkg"), "", [], K["file"], slots)
        self.assertEqual(g.check(DECL, "lab", kmd, dlls, slots=slots)[0], [])

    def test_adl_test_service_marker_fails_release_and_lab(self):
        marker = "BC250_ADL_TEST_SERVICE: host adapter discovery fixture\n"
        kmd, dll = self.release_set(pe(marker))
        self.assertTrue(any("BC250_ADL_TEST_SERVICE" in failure
                            for failure in g.check(DECL, "release", kmd, [dll])[0]))
        dll.unlink()
        self.lab_set(extra={"adl/atiadlxx.dll": [marker]})
        slots = {}
        kmd, dlls = g.collect(str(self.dir / "pkg"), "", [], K["file"], slots)
        self.assertTrue(any("BC250_ADL_TEST_SERVICE" in failure
                            for failure in g.check(DECL, "lab", kmd, dlls, slots=slots)[0]))

    def test_adl_test_service_marker_is_retained_only_in_fixture_branch(self):
        source = (REPO / "driver/umd/adl/adl-discover.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r'#if defined\(BC250_ADL_TEST_SERVICE\)\s*'
                                r'//[^\n]*\n\s*OutputDebugStringA\("BC250_ADL_TEST_SERVICE:')

    def lab_set(self, drop_name=None, drop_file=None, extra=None, upper=False):
        """A lab package (lab_slots() under pkg/files/payload): the lab KMD and every file a lab.every item names,
        each holding its names, less one name of one copy ((payload path, name)) or every copy an item names when
        asked; extra adds {payload path: names}; upper writes every file name in upper case."""
        self.write("pkg/BC250KMD.SYS" if upper else "pkg/bc250kmd.sys", pe(*LAB_KMD))
        for s, names in sorted(dict(lab_slots(), **(extra or {})).items()):
            if drop_file in (s, s.rsplit("/", 1)[1]):
                continue
            held = [n for n in names if drop_name != (s, n)]
            self.write("pkg/files/payload/%s" % (s.upper() if upper else s), pe(*LOG, *held))

    def package(self, flavour, layout="driverstore", drop=None, extra=None):
        """A whole package under pkg: setup.exe in the top folder, every other package slot under files/payload (a
        DriverStore member under payload/kmd when layout is driverstore), each file holding the names of its flavour,
        a document, and files/manifest.json over all of them. drop leaves one slot out, extra adds {path under pkg:
        bytes} before the manifest is written. Returns (pkg, manifest path)."""
        top = self.dir / "pkg"
        kmd = RELEASE_KMD if flavour == "release" else LAB_KMD
        for s, names in sorted(package_names(flavour).items()):
            if s == drop:
                continue
            if s == "setup.exe":
                rel = "setup.exe"
            elif layout == "driverstore" and s in MEMBER_OF:
                rel = "files/payload/kmd/" + MEMBER_OF[s]
            else:
                rel = "files/payload/" + s
            data = pe(*kmd) if s.endswith(".sys") else pe(*LOG, *names, "slot " + s)
            self.write("pkg/" + rel, data)
        self.write("pkg/files/INSTALL.md", b"how to install\n")
        for rel, data in (extra or {}).items():
            self.write("pkg/" + rel, data)
        rows = [{"path": p.relative_to(top / "files").as_posix(), "sha256": sha(p.read_bytes()), "size": p.stat().st_size}
                for p in sorted((top / "files").rglob("*")) if p.is_file()]
        setup = top / "setup.exe"
        manifest = {"schema": 1, "files": rows,
                    "setup": {"file": "setup.exe", "sha256": sha(setup.read_bytes()) if setup.is_file() else "0" * 64,
                              "size": setup.stat().st_size if setup.is_file() else 0}}
        path = self.write("pkg/files/manifest.json", json.dumps(manifest, indent=1).encode("utf-8"))
        return top, path

    def run_package(self, flavour, top, manifest, *extra):
        return self.run_gate("--flavour", flavour, "--package", str(top), "--manifest", str(manifest),
                             "--negative-control", NEGATIVE, *extra)

    def test_whole_package_passes_bound_to_its_manifest(self):
        for flavour in ("release", "lab"):
            for layout in ("driverstore", "plain"):
                with self.subTest(flavour=flavour, layout=layout):
                    self.setUp()
                    top, manifest = self.package(flavour, layout)
                    out = self.dir / "out"
                    self.assertEqual(self.run_package(flavour, top, manifest, "--out", str(out)), 0)
                    record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
                    self.assertEqual(sorted(f["slot"] for f in record["files"]), sorted(DECL["package"]["slots"]))
                    self.assertTrue(all("manifest" in f for f in record["files"]))
                    self.assertEqual(record["manifest"]["pe_rows"], len(DECL["package"]["slots"]))
                    self.assertEqual(record["manifest"]["sha256"], sha(manifest.read_bytes()))
                    self.assertEqual(record["declaration_sha256"], sha(g.DECLARATION.read_bytes()))
                    self.assertEqual(record["gate_sha256"], sha(Path(g.__file__).read_bytes()))
                    self.assertEqual(record["negative_control"]["slot"], NEGATIVE)
                    self.assertEqual(record["negative_control"]["missed"], [])
                    if layout == "driverstore":
                        nc = Path(record["negative_control"]["path"]).as_posix()
                        self.assertTrue(nc.endswith("payload/kmd/" + MEMBER_OF[NEGATIVE]), nc)
                    other = "lab" if flavour == "release" else "release"
                    self.assertEqual(self.run_package(other, top, manifest), 1)

    def test_package_that_does_not_match_its_manifest_fails(self):
        member = "files/payload/kmd/" + MEMBER_OF["wow64/vulkan/vulkan_radeon.dll"]
        cases = {
            "a slot left out": ({"drop": "wow64/vulkan/vulkan_radeon.dll"}, None),
            "the setup file left out": ({"drop": "setup.exe"}, None),
            "an extra PE in the manifest": ({"extra": {"files/payload/tools/extra.exe": pe("x")}}, None),
            "a PE outside the manifest": ({}, lambda top: (top / "files/payload/tools/stray.dll").write_bytes(pe("x"))),
            "a PE header without a PE suffix outside the manifest":
                ({}, lambda top: (top / "files/payload/tools/hidden.bin").write_bytes(pe("x"))),
            "a changed file": ({}, lambda top: (top / member).write_bytes(pe("changed after the manifest"))),
            "a row without its file": ({}, lambda top: (top / member).unlink()),
            "a changed setup file": ({}, lambda top: (top / "setup.exe").write_bytes(pe("changed setup"))),
        }
        for name, (kwargs, after) in cases.items():
            with self.subTest(case=name):
                self.setUp()
                top, manifest = self.package("release", **kwargs)
                if after:
                    after(top)
                self.assertEqual(self.run_package("release", top, manifest), 1)

    def test_package_inputs_that_cannot_be_judged_fail_closed(self):
        top, manifest = self.package("release")
        elsewhere = self.write("elsewhere/manifest.json", manifest.read_bytes())
        cases = {
            "manifest outside the package": ["--package", str(top), "--manifest", str(elsewhere)],
            "manifest with --pe": ["--package", str(top), "--manifest", str(manifest), "--pe", str(top)],
            "negative control on no file": ["--package", str(top), "--manifest", str(manifest),
                                            "--negative-control", "tools/none.dll"],
            "unreadable manifest": ["--package", str(top), "--manifest", str(self.write("pkg/bad/manifest.json", b"{"))],
            "manifest without setup": ["--package", str(top), "--manifest",
                                       str(self.write("pkg/files/m2.json", json.dumps({"files": []}).encode("utf-8")))],
        }
        for name, args in cases.items():
            with self.subTest(case=name):
                self.assertEqual(self.run_gate("--flavour", "release", *args), 2)

    def test_a_blind_scan_fails_the_negative_control(self):
        # The real scan of a release package finds nothing either way; the negative control must catch a scan that
        # cannot see what it looks for.
        top, manifest = self.package("release")
        out = self.dir / "out"
        with mock.patch.object(g, "has_name", lambda data, name: (0, 0)), \
                mock.patch.object(g, "prefixed_names", lambda data: set()), \
                mock.patch.object(g, "whole_strings", lambda data: {}):
            self.assertEqual(self.run_package("release", top, manifest, "--out", str(out)), 1)
        record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
        self.assertEqual(len(record["negative_control"]["expected"]), 2)
        self.assertEqual(sorted(record["negative_control"]["missed"]), sorted(record["negative_control"]["expected"]))

    def test_forbidden_name_in_another_case_fails_release(self):
        name = g.NEGATIVE_FORBIDDEN
        self.assertIn(name, [f["name"] for f in DECL["forbidden"]])
        for case, data in (("lower case", pe(*LOG, name.lower())),
                           ("mixed case, UTF-16LE", pe(*LOG, utf16=(name.title(),))),
                           ("after a stray byte", pe(*LOG) + b"\x01a" + name.lower().encode("ascii") + b"\0")):
            with self.subTest(case=case):
                self.release_set(data)
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
        # Prose: the name in another case inside a longer text, as in an assertion message.
        self.release_set(pe(*LOG, "assert(x || %s & 1)" % name.lower(), utf16=("see %s here" % name.lower(),)))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)

    def test_prefixed_whole_string_in_another_case_needs_declaring(self):
        for data in (pe(*LOG, "bc250_not_declared_anywhere"), pe(*LOG, utf16=("Amdgpu_Wddm_Not_Declared",))):
            self.release_set(data)
            self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
        self.release_set(pe(*LOG, "the bc250_not_declared_anywhere switch", "amdgpu_wddm_control", "amdgpu_wddm_setup"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)

    def test_release_rule_of_the_game_log_sink(self):
        rule = {e["name"]: e for e in DECL["allowed"]}["AMDGPU_WDDM_LOG"]["release_rule"]
        self.assertEqual(rule["requires"], ["AMDGPU_WDDM_GAMELOG", "amdgpu-wddm\\logs\\games"])
        self.assertEqual(rule["refuses"], ["stderr"])
        cases = {
            "no marker, no folder": (pe("AMDGPU_WDDM_LOG"), 1),
            "no folder": (pe("AMDGPU_WDDM_LOG", "AMDGPU_WDDM_GAMELOG"), 1),
            "no marker": (pe("AMDGPU_WDDM_LOG", "amdgpu-wddm\\logs\\games"), 1),
            "a longer marker name only": (pe("AMDGPU_WDDM_LOG", "AMDGPU_WDDM_GAMELOGX", "amdgpu-wddm\\logs\\games"), 1),
            "the stderr string": (pe(*LOG, "stderr"), 1),
            "the stderr string in UTF-16LE and upper case": (pe(*LOG, utf16=("STDERR",)), 1),
            "stderr in prose": (pe(*LOG, "write to stderr now"), 0),
            "the whole sink": (pe(*LOG), 0),
            "stderr in a file without the sink": (pe("stderr", "DXVK_HUD"), 0),
        }
        for name, (data, code) in cases.items():
            with self.subTest(case=name):
                self.release_set(data)
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), code)

    def test_open_decision_fails_release_only(self):
        self.assertFalse([e["name"] for e in DECL["allowed"] if e["reason"].startswith(g.OPEN_DECISION)])
        d = json.loads(g.DECLARATION.read_text(encoding="utf-8"))
        d["allowed"][0]["reason"] = g.OPEN_DECISION + ", pending a lead decision"
        decl = self.write("open.json", json.dumps(d).encode("utf-8"))
        self.release_set()
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"),
                                       "--declaration", str(decl)), 1)
        self.setUp()
        decl = self.write("open.json", json.dumps(d).encode("utf-8"))
        self.lab_set()
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg"),
                                       "--declaration", str(decl)), 0)

    def test_lab_none_is_verified(self):
        nones = [f["name"] for f in DECL["forbidden"] if "none" in f["lab"]]
        self.assertTrue(nones, "the declaration has no lab.none entry to test with")
        for name in nones:
            with self.subTest(name=name):
                self.setUp()
                self.lab_set(extra={"extra/a.dll": [name]})
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)
        self.setUp()
        self.lab_set(extra={"extra/a.dll": []})
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 0)

    def test_file_names_match_in_any_case(self):
        self.lab_set(upper=True)
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 0)

    def test_driverstore_members_have_their_input_slot(self):
        members = g.driverstore_members()
        self.assertEqual(members["kmd/d3d11/amdgpu_wddm_radv_d3d11.dll"], "d3d11/amdgpu_wddm_radv.dll")
        self.assertEqual(members["kmd/wow64/d3d12/amdgpu_wddm_vkd3d32.dll"], "wow64/d3d12/amdgpu_wddm_vkd3d.dll")
        self.assertIn(NEGATIVE, MEMBER_OF)
        self.assertIn(NEGATIVE, DECL["package"]["slots"])
        # One lab name less in one DriverStore copy fails the lab package, as in the plain layout.
        s, names = next((s, n) for s, n in sorted(package_names("lab").items()) if s in MEMBER_OF and n)
        top, manifest = self.package("lab")
        (top / "files/payload/kmd" / MEMBER_OF[s]).write_bytes(pe(*LOG, *names[1:], "slot " + s))
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(top)), 1)

    def test_second_kernel_driver_fails_closed(self):
        self.release_set()
        self.write("pkg/umd/filter.sys", pe("x"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 2)
        self.assertEqual(self.run_gate("--flavour", "release", "--kmd", str(self.dir / "pkg" / "bc250kmd.sys"),
                                       "--pe", str(self.dir / "pkg" / "umd")), 2)

    def test_two_files_with_one_payload_path_fail_closed(self):
        self.write("pkg/files/payload/kmd/bc250kmd.sys", pe(*RELEASE_KMD))
        self.write("pkg/files/payload/kmd/" + MEMBER_OF["d3d11/amdgpu_wddm_radv.dll"], pe("x"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)
        self.write("pkg/files/payload/d3d11/amdgpu_wddm_radv.dll", pe("y"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 2)

    def test_pe_header_without_pe_suffix_is_scanned(self):
        self.release_set()
        hidden = self.write("pkg/umd/payload.bin", pe(*LOG, g.NEGATIVE_FORBIDDEN))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
        hidden.write_bytes(b"MZ but no PE image")
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 2)
        hidden.write_bytes(b"plain data")
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)

    def test_package_slots_follow_the_release_sources(self):
        rows = json.loads((REPO / "tools" / "release" / "release-sources.json").read_text(encoding="utf-8-sig"))["files"]
        pes = {r["path"].lower().split("/", 1)[1] for r in rows if r["path"].lower().endswith(g.PE_SUFFIXES)}
        self.assertEqual(set(DECL["package"]["slots"]), pes | {"setup.exe"})
        sys.path.insert(0, str(REPO / "tools" / "release"))
        try:
            import driver_package
        finally:
            sys.path.pop(0)
        fixed = {src.lower() for src, _ in driver_package.NATIVE + driver_package.WOW if src.endswith(g.PE_SUFFIXES)}
        self.assertLessEqual(fixed, set(DECL["package"]["slots"]))

    def test_release_configs_define_ndebug(self):
        # Every Mesa, DXVK and vkd3d-proton config that a release row builds defines NDEBUG in its release flavour.
        # The CPU and Zink UMDs and the DXVK engine take it only there (release_options), so their lab flavour keeps
        # its asserts, and each of the three recipes adds release_options to a release-flavour build.
        mesa, dxvk, vkd3d = ("tools/build/build-%s.ps1" % n for n in ("mesa", "dxvk", "vkd3d"))
        self.assertEqual(set(g.NDEBUG_RECIPES), {mesa, dxvk, vkd3d})
        configs = {recipe: json.loads((REPO / path).read_text(encoding="utf-8-sig"))["configs"]
                   for recipe, path in g.NDEBUG_RECIPES.items()}
        rows = json.loads((REPO / "tools" / "release" / "release-sources.json").read_text(encoding="utf-8-sig"))["files"]
        self.assertEqual(g.release_without_ndebug(configs, rows), [])
        built = {(r["built_from"]["recipe"], r["built_from"]["args"][r["built_from"]["args"].index("-Config") + 1])
                 for r in rows if (r.get("built_from") or {}).get("recipe") in g.NDEBUG_RECIPES}
        self.assertLessEqual({(mesa, "llvmpipe-umd"), (mesa, "zink-umd"), (mesa, "radv-mt"), (mesa, "radv-system"),
                              (mesa, "zink-gl-icd"), (dxvk, "ddi-engine"), (vkd3d, "ddi-engine-lto")}, built)
        for recipe, name in ((mesa, "llvmpipe-umd"), (mesa, "zink-umd"), (dxvk, "ddi-engine")):
            self.assertNotIn("-Db_ndebug=true", configs[recipe][name]["options"], name + ": the lab flavour keeps asserts")
            self.assertEqual(configs[recipe][name]["release_options"], ["-Db_ndebug=true"], name)
        # The release flavour's if may also look the key up before the read: under the Set-StrictMode 3.0 of
        # common.ps1 a config without release_options cannot read it.
        added = re.compile(r"if \(\$forkFlavour -eq 'release'[^{\r\n]*\) \{\s*\$options \+= @\(\$recipeConfig\.release_options")
        for recipe in g.NDEBUG_RECIPES:
            text = (REPO / recipe).read_text(encoding="utf-8")
            self.assertRegex(text, added, recipe)
            self.assertIn("flavour         = $forkFlavour", text, recipe)
        # Planted controls: any release config without it, or with a later false, fails; so does a config, or a
        # recipe's whole file, that the gate does not have.
        for recipe, name in sorted(built):
            planted = json.loads(json.dumps(configs))
            for key in ("options", "release_options"):
                planted[recipe][name][key] = [o for o in planted[recipe][name].get(key, []) if not o.startswith("-Db_ndebug=")]
            self.assertIn((recipe, name), {(r, c) for r, c, _ in g.release_without_ndebug(planted, rows)}, name)
        planted = json.loads(json.dumps(configs))
        planted[dxvk]["ddi-engine"]["release_options"].append("-Db_ndebug=false")
        self.assertEqual({(r, c) for r, c, _ in g.release_without_ndebug(planted, rows)}, {(dxvk, "ddi-engine")})
        del planted[mesa]["zink-umd"]
        with self.assertRaises(g.GateError):
            g.release_without_ndebug(planted, rows)
        planted = json.loads(json.dumps(configs))
        del planted[vkd3d]
        with self.assertRaises(g.GateError):
            g.release_without_ndebug(planted, rows)

    def test_release_recipes_ndebug_and_optimised(self):
        # Every other recipe of a release row is classified, and each file that compiles shipped objects spells the
        # release NDEBUG define and an optimisation level for its compiler. The tables name exactly the recipes that
        # the rows use.
        rows = json.loads((REPO / "tools" / "release" / "release-sources.json").read_text(encoding="utf-8-sig"))["files"]

        def read(path, edits=None):
            if edits and path in edits:
                return edits[path]
            p = REPO / path
            return p.read_text(encoding="utf-8-sig") if p.is_file() else None

        self.assertEqual(g.release_recipe_findings(rows, read), [])
        used = {(r.get("built_from") or {}).get("recipe") for r in rows} - {None}
        tables = (g.RELEASE_COMPILES, g.NDEBUG_RECIPES, g.NOT_COMPILED, g.NDEBUG_PENDING)
        self.assertEqual(set().union(*tables), used)
        self.assertEqual(sum(len(t) for t in tables), len(used), "a recipe in two tables")
        self.assertTrue(all(g.NOT_COMPILED.values()) and all(r for _, r in g.NDEBUG_PENDING.values()),
                        "an entry without a reason")
        # build-rebuilt-controls.ps1 has bc250hipblas.dll compiled by build-hipblas.ps1, so both files are checked.
        self.assertIn(("compute/hipblas/build-hipblas.ps1", "clang"),
                      g.RELEASE_COMPILES["compute/hip/build-rebuilt-controls.ps1"])
        # The two shells compile with one flag set, in code and not only in a comment: the D3D11 shell built at /Od
        # until tester.31.
        for shell in ("tools/build/build-umd-d3d12.ps1", "tools/build/build-umd-dxvk.ps1"):
            for flag in ("/O2", "/GL", "/arch:AVX2", "/LTCG"):
                self.assertRegex(g.recipe_code(read(shell)), r"(?<![\w/:])%s(?![\w:])" % re.escape(flag), shell)
        # The optimisation levels, by this test's own patterns: a gate pattern that also took /Od, /Ob1 or /OPT:REF
        # would pass a recipe that compiles at /Od.
        levels = {"cl": r"(?<![\w/])/O[12x](?!\w)", "clang": r"(?<![\w-])-O[123s](?!\w)"}
        self.assertEqual(set(g.RELEASE_OPTIMISE), set(levels))
        self.assertIsNone(g.RELEASE_OPTIMISE["cl"].search("cl /Od /Ob1 /Oi /link /OPT:REF /OPT:ICF '/Od'"))
        self.assertIsNone(g.RELEASE_OPTIMISE["clang"].search("clang -O0 -Og -Ofast-math -fno-O2"))
        for good in ("cl /O2 x", "@('/O1')", "/Ox"):
            self.assertIsNotNone(g.RELEASE_OPTIMISE["cl"].search(good), good)
        # Planted controls: a recipe that no table names; each file with its define removed, with the define only in a
        # comment, and with its optimisation level removed; a pending recipe that spells the define, and one without
        # its optimisation level.
        planted = rows + [{"path": "payload/tools/new.exe", "built_from": {"recipe": "tools/build/build-new.ps1"}}]
        self.assertEqual([r for r, _ in g.release_recipe_findings(planted, read)], ["tools/build/build-new.ps1"])
        for recipe, files in sorted(g.RELEASE_COMPILES.items()):
            for path, compiler in files:
                with self.subTest(recipe=recipe, path=path):
                    text, define = read(path), g.RELEASE_DEFINE[compiler]
                    self.assertIn(define, text)
                    gone = (recipe, "%s does not spell %s" % (path, define))
                    for planted_text in (text.replace(define, "'/DNOTHING'"),
                                         text.replace(define, "'/DNOTHING'") + "\n    # %s\n<# %s #>\n" % (define, define)):
                        self.assertEqual(g.release_recipe_findings(rows, lambda p: read(p, {path: planted_text})), [gone])
                    edits = {path: re.sub(levels[compiler], "", text)}
                    self.assertEqual([w.split(" spells ")[0] for _, w in g.release_recipe_findings(
                        rows, lambda p: read(p, edits))], [path])
        for recipe, (compiler, _) in sorted(g.NDEBUG_PENDING.items()):
            text = read(recipe)
            self.assertNotIn(g.RELEASE_DEFINE[compiler], text)
            edits = {recipe: text + "\n$x = %s\n" % g.RELEASE_DEFINE[compiler]}
            self.assertEqual([r for r, _ in g.release_recipe_findings(rows, lambda p: read(p, edits))], [recipe])
            edits = {recipe: re.sub(levels[compiler], "", text)}
            self.assertEqual([r for r, _ in g.release_recipe_findings(rows, lambda p: read(p, edits))], [recipe])

    def test_live_assert_fails_release_only(self):
        # An image built without NDEBUG: it imports an assert function or a debug C runtime, or it links the static
        # C runtime's assert code, whose message it then holds. A release set fails on each, a lab set keeps them.
        crt = "api-ms-win-crt-runtime-l1-1-0.dll"
        clean = pe_importing(("KERNEL32.dll", "GetLastError"), (crt, "_initterm"), strings=LOG)
        cases = {
            "_wassert": pe_importing(("KERNEL32.dll", "GetLastError"), (crt, "_wassert"), strings=LOG),
            "_assert": pe_importing((crt, "_initterm"), (crt, "_assert"), strings=LOG),
            "RtlAssert": pe_importing(("ntoskrnl.exe", "RtlAssert"), strings=LOG),
            "debug runtime": pe_importing(("ucrtbased.dll", "malloc"), strings=LOG),
            "static message": pe(*LOG, g.CRT_ASSERT_TEXT + "\n"),
            "static message utf16": pe(*LOG, utf16=(g.CRT_ASSERT_TEXT + "\n",)),
        }
        self.assertEqual(g.pe_imports(clean), [("KERNEL32.dll", "GetLastError"), (crt, "_initterm")])
        kmd, dll = self.release_set(clean)
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)
        for name, umd in cases.items():
            with self.subTest(case=name):
                dll.write_bytes(umd)
                self.assertTrue(g.live_asserts(umd), name)
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(g.main(["--flavour", "release", "--package", str(self.dir / "pkg")]), 1)
                self.assertIn("a live assert", out.getvalue())
                failures, _, record = g.check(DECL, "lab", kmd, [dll])
                self.assertFalse([f for f in failures if "live assert" in f], name)
                self.assertTrue([f for f in record["files"] if f["asserts"]], name + ": a lab set records it")

    def test_import_directory_is_read(self):
        # The reader against images of the real toolchain: a 64-bit and a 32-bit system DLL import by name, and none
        # of them is built without NDEBUG. An import directory outside every section cannot be judged.
        system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
        for p in (system / "System32" / "kernel32.dll", system / "SysWOW64" / "kernel32.dll"):
            if not p.is_file():
                continue
            with self.subTest(image=str(p)):
                data = g.read_pe(p)
                self.assertGreater(len(g.pe_imports(data)), 100)
                self.assertEqual(g.live_asserts(data), [])
        broken = bytearray(pe_importing(("KERNEL32.dll", "GetLastError")))
        struct.pack_into("<I", broken, 0x58 + 112 + 8, 0x9000)
        with self.assertRaises(g.GateError):
            g.pe_imports(bytes(broken))
        self.release_set(bytes(broken))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 2)

    def test_build_release_runs_the_gate(self):
        # On the whole payload before the submission CAB, and on the package bound to manifest.json before the zip.
        text = (REPO / "tools" / "release" / "build-release.ps1").read_text(encoding="utf-8")
        cab = text.index("if ($SubmissionCab) {")
        manifest = text.index("[IO.File]::WriteAllText((Join-Path $pkg 'manifest.json')")
        archive = text.index("Compress-Archive -Path $pkgRoot")
        self.assertEqual(text.count("'tools\\quality\\flavour_gate.py'"), 2,
                         "one path to the gate in Invoke-FlavourGate and one in the source-pin run")
        runs = [i for i in range(len(text)) if text.startswith("Invoke-FlavourGate ", i)]
        self.assertEqual(len(runs), 2, "two runs of the gate")
        self.assertLess(runs[0], cab)
        self.assertTrue(manifest < runs[1] < archive)
        self.assertIn("'--negative-control', '%s'" % NEGATIVE, text)
        self.assertIn("'--manifest'", text)
        # And on the source pins, between the flavour selection and everything the build copies.
        selection = text.index("'release_flavour.py'")
        pins = text.index("'--sources', $sourcesPath, '--root', $Root")
        self.assertTrue(selection < pins < runs[0])

    def selection(self, umd=pe(*LOG, "DXVK_HUD"), source="build/amdgpu_wddm_d3d11.dll"):
        """A flavour selection of two rows, the KMD and the D3D11 shell, with the files they pin under self.dir."""
        self.write("build/bc250kmd.sys", pe(*RELEASE_KMD))
        self.write("build/amdgpu_wddm_d3d11.dll", umd)
        rows = {"flavour": "release", "files": [
            {"path": "payload/kmd/" + K["file"], "source": "build/bc250kmd.sys"},
            {"path": "payload/d3d11/amdgpu_wddm_d3d11.dll", "source": source},
        ]}
        return self.write("selected.json", json.dumps(rows).encode("utf-8"))

    def run_pins(self, path):
        return self.run_gate("--flavour", "release", "--sources", str(path), "--root", str(self.dir))

    def test_source_pins_are_judged_before_the_build(self):
        # The release row of a file whose lab build holds a name must not pin a file that holds it. The planted
        # name is one the declaration demands of the D3D11 shell's lab build, so this is the rc1-to-rc4 defect:
        # a lab-flavour image pinned as release, which the gate on the source pins refuses.
        planted = sorted({n for item, names in lab_items().items()
                          for n in names if item in ("amdgpu_wddm_d3d11.dll", "d3d11/amdgpu_wddm_d3d11.dll")})
        self.assertTrue(planted, "the declaration names no lab-only name of the D3D11 shell")
        self.assertEqual(self.run_pins(self.selection()), 0)
        self.assertEqual(self.run_pins(self.selection(umd=pe(*LOG, "DXVK_HUD", planted[0]))), 1)

    def test_source_pins_fail_closed(self):
        # A row whose file is not there, a selection of the other flavour, and --sources without --root.
        self.assertEqual(self.run_pins(self.selection(source="build/not-built.dll")), 2)
        rows = json.loads((self.selection()).read_text(encoding="utf-8"))
        rows["flavour"] = "lab"
        self.assertEqual(self.run_pins(self.write("selected.json", json.dumps(rows).encode("utf-8"))), 2)
        self.assertEqual(self.run_gate("--flavour", "release", "--sources", str(self.selection())), 2)

    def lab_selection(self, drop_file=None):
        """A lab flavour selection over the whole lab_slots() set, less the copies of one file name."""
        self.write("build/bc250kmd.sys", pe(*LAB_KMD))
        rows = [{"path": "payload/kmd/" + K["file"], "source": "build/bc250kmd.sys"}]
        for s, names in sorted(lab_slots().items()):
            if drop_file in (s, s.rsplit("/", 1)[1]):
                continue
            self.write("build/%s" % s, pe(*LOG, *names))
            rows.append({"path": "payload/" + s, "source": "build/" + s})
        return self.write("selected-lab.json", json.dumps({"flavour": "lab", "files": rows}).encode("utf-8"))

    def test_source_pins_note_only_what_no_row_can_pin(self):
        # The lab arm over the source pins: setup.exe reaches the package through -SetupApp, so package.not_pinned
        # names it and its absence from the pins is a note. Any other lab.every item missing from the pins is a
        # dropped row, which stays a failure here and not only in the package arm.
        self.assertEqual(DECL["package"].get("not_pinned"), ["setup.exe"])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(g.main(["--flavour", "lab", "--sources", str(self.lab_selection(drop_file="setup.exe")),
                                     "--root", str(self.dir)]), 0)
        self.assertIn("no source row pins setup.exe", out.getvalue())
        other = next(i for i in sorted(lab_items()) if "/" not in i and i != "setup.exe")
        self.assertEqual(self.run_gate("--flavour", "lab", "--sources", str(self.lab_selection(drop_file=other)),
                                       "--root", str(self.dir)), 1)

    def test_lab_set_passes_as_lab_only(self):
        self.lab_set()
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 0)
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_lab_file_without_one_lab_name_fails(self):
        # One build of the set lost the lab switch for one lookup (or the lookup moved): the lab arm must fail.
        for item, names in sorted(lab_items().items()):
            for n in names:
                with self.subTest(item=item, name=n):
                    self.setUp()
                    self.lab_set(drop_name=(slot_of(item), n))
                    self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_lab_set_without_a_named_file_fails(self):
        for item in sorted(lab_items()):
            with self.subTest(item=item):
                self.setUp()
                self.lab_set(drop_file=item)
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_path_items_are_payload_paths(self):
        # The payload directory itself as the scanned root gives the same payload paths.
        self.assertTrue([i for i in lab_items() if "/" in i], "the declaration has no path item to test with")
        self.lab_set()
        self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(self.dir / "pkg" / "bc250kmd.sys"),
                                       "--pe", str(self.dir / "pkg" / "files" / "payload")), 0)
        # A path item never matches a file named alone on the command line: the lab set fails closed on it.
        item = sorted(i for i in lab_items() if "/" in i)[0]
        self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(self.dir / "pkg" / "bc250kmd.sys"),
                                       "--pe", str(self.dir / "pkg" / "files" / "payload" / item)), 1)

    def test_unlisted_copy_with_a_path_listed_name_fails(self):
        # The list of paths is exact: another copy of the same file that holds the name means the build lines
        # changed and the declaration (and its note) is stale. Without the name, that copy passes.
        for f in DECL["forbidden"]:
            paths = [i for i in f["lab"].get("every", []) if "/" in i]
            if not paths:
                continue
            base = paths[0].rsplit("/", 1)[1]
            other = "unlisted/" + base
            needs = sorted({n for item, names in lab_items().items() if item == base for n in names})
            with self.subTest(name=f["name"]):
                self.setUp()
                self.lab_set(extra={other: needs})
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 0)
                self.setUp()
                self.lab_set(extra={other: needs + [f["name"]]})
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_lab_kmd_only_skips_the_umd_names(self):
        kmd = self.write("bc250kmd.sys", pe(*LAB_KMD))
        self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(kmd), "--kmd-only"), 0)

    def test_kmd_registry_names_are_declared(self):
        # BD-235: the lab-only registry values of the KMD are forbidden names whose lab build is the KMD.
        self.assertEqual(sorted(KMD_SPELLING), list(KMD_NAMES))

    def test_kmd_registry_name_fails_release_as_the_source_spells_it(self):
        # The release KMD must not hold a lab registry name, in ASCII or as the UTF-16LE string of the source.
        base = [s for s in LAB_KMD if s not in KMD_NAMES and s != K["flavour_markers"]["lab"] and
                s not in [e["marker"] for e in K["lab_escape_markers"]]]
        self.assertEqual(base, [])
        for name, spelt in sorted(KMD_SPELLING.items()):
            for encoding in ("ascii", "utf16"):
                with self.subTest(name=spelt, encoding=encoding):
                    data = pe(*RELEASE_KMD, utf16=(spelt,)) if encoding == "utf16" else pe(*RELEASE_KMD, spelt)
                    kmd = self.write("bc250kmd.sys", data)
                    out = self.dir / ("out-%s-%s" % (name, encoding))
                    self.assertEqual(self.run_gate("--flavour", "release", "--kmd", str(kmd), "--kmd-only",
                                                   "--out", str(out)), 1)
                    record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
                    hit = record["files"][0]["forbidden"][name]
                    self.assertEqual(hit["variant"], {encoding: 1})
        # Inside a longer text in another case the name is prose, as for every forbidden name.
        kmd = self.write("bc250kmd.sys", pe(*RELEASE_KMD, "lab: PagingBoundaryMode absent, the default (pad)"))
        self.assertEqual(self.run_gate("--flavour", "release", "--kmd", str(kmd), "--kmd-only"), 0)

    def test_lab_kmd_holds_the_registry_names_as_the_source_spells_them(self):
        lab = [s for s in LAB_KMD if s not in KMD_NAMES]
        kmd = self.write("bc250kmd.sys", pe(*lab, utf16=tuple(KMD_SPELLING.values())))
        self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(kmd), "--kmd-only"), 0)
        for name, spelt in sorted(KMD_SPELLING.items()):
            with self.subTest(name=spelt):
                kmd = self.write("bc250kmd.sys", pe(*lab, utf16=tuple(v for v in KMD_SPELLING.values() if v != spelt)))
                self.assertEqual(self.run_gate("--flavour", "lab", "--kmd", str(kmd), "--kmd-only"), 1)

    def test_lab_kmd_without_one_registry_name_fails_the_lab_set(self):
        for name in KMD_NAMES:
            with self.subTest(name=name):
                self.setUp()
                self.lab_set()
                self.write("pkg/bc250kmd.sys", pe(*[s for s in LAB_KMD if s != name]))
                self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)

    def test_every_forbidden_name_has_a_lab_expectation(self):
        for f in DECL["forbidden"]:
            with self.subTest(name=f["name"]):
                self.assertTrue(f["lab"].get("every") or f["lab"].get("none", "").strip())

    def test_out_of_scope_name_is_recorded_not_failed(self):
        self.assertTrue(DECL.get("out_of_scope"), "the declaration has no out-of-scope entry to test with")
        name = DECL["out_of_scope"][0]["name"]
        self.release_set(pe(*LOG, name))
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
        # The planted-name control: every declared forbidden name, whatever its prefix, planted in a release
        # fixture fails the scan, and the failure is that name in that file alone. Coverage does not find a name
        # outside the driver's prefixes (VKD3D_LOG_FILE, RADV_DEBUG), so only the forbidden list can.
        unprefixed = [f["name"] for f in DECL["forbidden"] if not g.PREFIXED.fullmatch(f["name"])]
        self.assertTrue({"VKD3D_LOG_FILE", "DXVK_LOG_PATH", "RADV_DEBUG"} <= set(unprefixed))
        out = self.dir / "out"
        for f in DECL["forbidden"]:
            for enc, umd in (("ascii", pe(*LOG, f["name"])), ("utf16", pe(*LOG, utf16=(f["name"],)))):
                with self.subTest(name=f["name"], encoding=enc):
                    _, dll = self.release_set(umd)
                    self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg"),
                                                   "--out", str(out)), 1)
                    record = json.loads((out / "flavour-gate.json").read_text(encoding="utf-8"))
                    found = {r["slot"]: r["forbidden"] for r in record["files"]}
                    self.assertEqual(sorted(found), ["bc250kmd.sys", "umd/amdgpu_wddm_d3d11.dll"])
                    self.assertEqual(found["bc250kmd.sys"], {})
                    hit = found["umd/amdgpu_wddm_d3d11.dll"]
                    self.assertEqual(list(hit), [f["name"]])
                    self.assertEqual(hit[f["name"]][enc], 1)
                    self.assertEqual(record["failures"], ["%s: %s (%s)" % (dll, f["name"], g.describe(hit[f["name"]]))])

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

    def test_undeclared_prefixed_name_fails(self):
        # The negative control of the coverage rule: one name of the driver's prefixes that no list declares.
        for name in ("BC250_NOT_DECLARED_ANYWHERE", "AMDGPU_WDDM_NOT_DECLARED", "BC250XYZ_NOT_DECLARED"):
            with self.subTest(name=name):
                self.assertFalse(any(name == e["name"] for k in ("forbidden", "allowed", "out_of_scope")
                                     for e in DECL.get(k, [])))
                self.release_set(pe(*LOG, name))
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
                self.release_set(pe(*LOG, utf16=("x" + name,)))
                self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)
        self.setUp()
        self.lab_set(extra={"undeclared/a.dll": ["BC250_NOT_DECLARED_ANYWHERE"]})
        self.assertEqual(self.run_gate("--flavour", "lab", "--package", str(self.dir / "pkg")), 1)
        self.setUp()
        self.write("pkg/bc250kmd.sys", pe(*RELEASE_KMD, "RowCount must be 1..BC250_NOT_DECLARED_ANYWHERE"))
        self.write("pkg/a.dll", pe("x"))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 1)

    def test_declared_prefixed_names_pass(self):
        allowed = [e["name"] for e in DECL["allowed"] if g.PREFIXED.fullmatch(e["name"])]
        self.assertTrue(allowed, "the declaration has no allowed prefixed name to test with")
        self.release_set(pe(*LOG, *allowed, *[e["name"] for e in DECL.get("out_of_scope", [])]))
        self.assertEqual(self.run_gate("--flavour", "release", "--package", str(self.dir / "pkg")), 0)

    def test_prefixed_names(self):
        data = pe("BC250_FOO_%u", "xxBC250DXVK_BAR=1", "BC250_", "bc250_lower", "BC250d3d_zink.dll",
                  utf16=("AMDGPU_WDDM_WIDE",)) + b"\x01AMDGPU_WDDM_RAW\0"
        self.assertEqual(g.prefixed_names(data), {"BC250_FOO", "BC250DXVK_BAR", "AMDGPU_WDDM_WIDE", "AMDGPU_WDDM_RAW"})

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
            "lab.every path without note": lambda d: d["forbidden"][0].update(lab={"every": ["d3d12/a.dll"]}),
            "lab.every path, empty note": lambda d: d["forbidden"][0].update(lab={"every": ["d3d12/a.dll"], "note": " "}),
            "lab.every backslash": lambda d: d["forbidden"][0].update(lab={"every": ["d3d12\\a.dll"], "note": "x"}),
            "lab.every dot dot": lambda d: d["forbidden"][0].update(lab={"every": ["../a.dll"], "note": "x"}),
            "lab.every leading slash": lambda d: d["forbidden"][0].update(lab={"every": ["/a.dll"], "note": "x"}),
            "lab.every drive": lambda d: d["forbidden"][0].update(lab={"every": ["c:/a.dll"], "note": "x"}),
            "lab.every file and path": lambda d: d["forbidden"][0].update(
                lab={"every": ["a.dll", "d3d12/a.dll"], "note": "x"}),
            "lab.none with note": lambda d: d["forbidden"][0].update(lab={"none": "x", "note": "x"}),
            "lab.every upper case": lambda d: d["forbidden"][0].update(lab={"every": ["A.dll"]}),
            "lab.none without reason": lambda d: d["forbidden"][0].update(lab={"none": " "}),
            "forbidden and out of scope": lambda d: d["out_of_scope"].append(
                {"name": d["forbidden"][0]["name"], "where": "x", "reason": "x"}),
            "no package slots": lambda d: d.pop("package"),
            "package slot in upper case": lambda d: d["package"]["slots"].append("TOOLS/X.EXE"),
            "package slot twice": lambda d: d["package"]["slots"].append(d["package"]["slots"][0]),
            "package without the KMD": lambda d: d["package"].update(
                slots=[s for s in d["package"]["slots"] if not s.endswith(".sys")]),
            "package with a second .sys": lambda d: d["package"]["slots"].append("kmd/filter.sys"),
            "release_rule on a forbidden entry": lambda d: d["forbidden"][0].update(
                release_rule={"requires": ["abc"], "why": "x"}),
            "release_rule without requires": lambda d: next(
                e for e in d["allowed"] if "release_rule" in e).update(release_rule={"why": "x"}),
            "release_rule with a short text": lambda d: next(
                e for e in d["allowed"] if "release_rule" in e)["release_rule"].update(requires=["ab"]),
            "release_rule without why": lambda d: next(
                e for e in d["allowed"] if "release_rule" in e)["release_rule"].pop("why"),
        }
        for name, change in cases.items():
            with self.subTest(case=name):
                self.assertEqual(broken(change), 2)
        # The positive control of the path cases: a path with a note loads.
        self.assertEqual(broken(lambda d: d["forbidden"][0].update(lab={"every": ["d3d12/a.dll"], "note": "x"})), 0)
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
