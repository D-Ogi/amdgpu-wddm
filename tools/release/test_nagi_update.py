"""Self-test of the release side of Nagi self-update (tools/release/nagi_update.py, docs/design/self-update.md).

    python -m unittest discover -s tools/release -p test_nagi_update.py   # the 'nagi-update-writer' gate of quick.ps1

A small package tree in a temporary folder: a KMD and a CLI (PE images), the state catalog, the INF and the catalog,
documents and the control application folder. The tests hold:
  - the kind of each path (driver, apps, or in no identity);
  - the identity: a new signature of a PE file is no change, a changed byte of a driver or an apps file is, and a
    changed INF, catalog or document is not (negative controls for each kind);
  - the apps pack: the same files give the same bytes, and the pack holds exactly the files of payload/control;
  - the manifest: every field the installer reads, and its refusals (a release outside the tag grammar, an asset name
    that is not the release asset, an apps pack with a file more).
BC250_TEST_OUT, when set, holds the temporary trees (quick.ps1 keeps them off drive C:).
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import zipfile

import nagi_update

OUT = os.environ.get("BC250_TEST_OUT") or None
RELEASE = "0.7.216.109-tester.37"
CATALOG = "c" * 64


def fake_pe(body, signature=b""):
    """A PE32+ image with a CheckSum and a security directory entry; with a signature, a certificate table at the end
    (as test_provenance.fake_pe)."""
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
    data[optional + 64:optional + 68] = struct.pack("<I", 0x1234)
    data[security:security + 8] = struct.pack("<II", len(image), len(signature))
    return bytes(data) + signature


class Package:
    FILES = {
        "payload/kmd/bc250kmd.sys": lambda: fake_pe(b"kmd body"),
        "payload/kmd/bc250kmd.inf": lambda: b"DriverVer = 11/04/2026,0.7.216.109\n",
        "payload/kmd/bc250kmd.cat": lambda: b"catalog bytes",
        "payload/kmd/amdgpu-state.bin": lambda: b"AMDST02\0" + struct.pack("<II", 2, 0) + CATALOG.encode("ascii") + b"\0" * 16,
        "payload/tools/bc250kmd_cli.exe": lambda: fake_pe(b"cli body"),
        "payload/cert/amdgpu-wddm-release.cer": lambda: b"certificate",
        "payload/control/amdgpu_wddm_control.exe": lambda: fake_pe(b"control body"),
        "payload/control/strings/en.txt": lambda: b"app.title|ok|?|Nagi\n",
        "README.md": lambda: b"# notes\n",
        "licenses/MIT.txt": lambda: b"licence",
        "compatibility.json": lambda: b"{}",
    }

    def __init__(self, base):
        self.root = Path(base) / "files"
        for path, make in self.FILES.items():
            self.write(path, make())
        self.write_manifest()

    def write(self, path, data):
        p = self.root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def write_manifest(self, release=RELEASE):
        files = sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*") if p.is_file() and p.name != "manifest.json")
        kmd = hashlib.sha256((self.root / "payload/kmd/bc250kmd.sys").read_bytes()).hexdigest().upper()
        m = {"release": release, "flavour": "release", "built_utc": "2026-11-04T00:00:00Z", "kmd_version": "0.7.216.1",
             "kmd_abi": "0x000700D8", "files": [{"path": f} for f in files],
             "components": [{"package_path": "payload/kmd/bc250kmd.sys", "sha256": kmd}]}
        (self.root / "manifest.json").write_text(json.dumps(m, indent=1), encoding="utf-8")
        return m


class NagiUpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nagi-update-", dir=OUT)
        self.pkg = Package(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def ids(self):
        return nagi_update.identity(self.pkg.root)

    def test_kinds(self):
        k = nagi_update.kind_of
        self.assertEqual(k("payload/control/amdgpu_wddm_control.exe"), "apps")
        self.assertEqual(k("payload/kmd/bc250kmd.sys"), "driver")
        self.assertEqual(k("payload/tools/bc250kmd_cli.exe"), "driver")
        self.assertEqual(k("payload/kmd/amdgpu-state.bin"), "driver")
        for none in ("payload/kmd/bc250kmd.inf", "payload/kmd/bc250kmd.cat", "payload/cert/x.cer", "README.md",
                     "licenses/MIT.txt", "compatibility.json", "manifest.json", "NOTICE"):
            self.assertIsNone(k(none), none)
        # A document inside a folder is part of its kind: only the documents of the package root stay out.
        self.assertEqual(k("payload/tools/notes.md"), "driver")

    def test_identity_counts_and_state(self):
        i = self.ids()
        self.assertEqual(i["driver"]["files"], 3)
        self.assertEqual(i["apps"]["files"], 2)
        self.assertEqual(i["driver"]["state_schema"], 2)
        self.assertEqual(i["driver"]["catalog_id"], CATALOG)

    def test_new_signature_is_no_change(self):
        before = self.ids()
        self.pkg.write("payload/kmd/bc250kmd.sys", fake_pe(b"kmd body", b"another signature"))
        self.pkg.write("payload/control/amdgpu_wddm_control.exe", fake_pe(b"control body", b"a signature"))
        self.assertEqual(self.ids(), before)

    def test_changed_files_change_their_kind_only(self):
        before = self.ids()
        self.pkg.write("payload/tools/bc250kmd_cli.exe", fake_pe(b"cli body 2"))
        after = self.ids()
        self.assertNotEqual(after["driver"]["content_id"], before["driver"]["content_id"])
        self.assertEqual(after["apps"]["content_id"], before["apps"]["content_id"])
        self.pkg.write("payload/control/strings/en.txt", b"app.title|ok|?|Nagi 2\n")
        again = self.ids()
        self.assertNotEqual(again["apps"]["content_id"], after["apps"]["content_id"])
        self.assertEqual(again["driver"]["content_id"], after["driver"]["content_id"])

    def test_release_stamps_are_no_change(self):
        before = self.ids()
        self.pkg.write("payload/kmd/bc250kmd.inf", b"DriverVer = 11/05/2026,0.7.216.110\n")
        self.pkg.write("payload/kmd/bc250kmd.cat", b"another catalog")
        self.pkg.write("README.md", b"# other notes\n")
        self.assertEqual(self.ids(), before)

    def test_bad_state_catalog_refused(self):
        self.pkg.write("payload/kmd/amdgpu-state.bin", b"AMDST01\0" + b"\0" * 80)
        with self.assertRaises(ValueError):
            self.ids()

    def test_pack_deterministic_and_exact(self):
        a = nagi_update.make_packs(self.pkg.root, RELEASE, Path(self.tmp) / "a")
        b = nagi_update.make_packs(self.pkg.root, RELEASE, Path(self.tmp) / "b")
        self.assertEqual(a["sha256"], b["sha256"])
        self.assertEqual(Path(a["path"]).read_bytes(), Path(b["path"]).read_bytes())
        self.assertEqual(a["asset"], f"nagi-apps-{RELEASE}.zip")
        with zipfile.ZipFile(a["path"]) as z:
            self.assertEqual(sorted(z.namelist()), ["amdgpu_wddm_control.exe", "strings/en.txt"])
            for info in z.infolist():
                self.assertEqual(info.date_time, nagi_update.ZIP_TIME)
        self.assertEqual([f["path"] for f in a["files"]], ["amdgpu_wddm_control.exe", "strings/en.txt"])
        with self.assertRaises(ValueError):
            nagi_update.make_packs(self.pkg.root, "latest", Path(self.tmp) / "c")

    def release_zip(self, name=None):
        z = Path(self.tmp) / (name or f"amdgpu-wddm-tester-{RELEASE}.zip")
        z.write_bytes(b"PK release zip stand-in")
        return z

    def test_manifest_fields(self):
        pack = nagi_update.make_packs(self.pkg.root, RELEASE, Path(self.tmp) / "out")
        z = self.release_zip()
        m = nagi_update.make_manifest(self.pkg.root, z, pack["path"], "0.7.216.108-tester.36")
        self.assertEqual(m["schema"], "nagi.update/1")
        self.assertEqual(m["release"], RELEASE)
        self.assertEqual(m["min_installed"], "0.7.216.108-tester.36")
        drv, apps = m["components"]
        ids = self.ids()
        self.assertEqual((drv["name"], drv["kind"], drv["restart"]), ("driver", "driver", True))
        self.assertEqual(drv["asset"], z.name)
        self.assertEqual(drv["sha256"], hashlib.sha256(z.read_bytes()).hexdigest().upper())
        self.assertEqual(drv["size"], z.stat().st_size)
        self.assertEqual(drv["content_id"], ids["driver"]["content_id"])
        self.assertEqual(drv["driver"]["kmd_abi"], "0x000700D8")
        self.assertEqual(drv["driver"]["state_schema"], 2)
        self.assertEqual(drv["driver"]["catalog_id"], CATALOG)
        self.assertEqual(drv["driver"]["kmd_sha256"], hashlib.sha256((self.pkg.root / "payload/kmd/bc250kmd.sys").read_bytes()).hexdigest().upper())
        self.assertEqual(drv["driver"]["package_manifest_sha256"], hashlib.sha256((self.pkg.root / "manifest.json").read_bytes()).hexdigest().upper())
        self.assertEqual((apps["name"], apps["kind"], apps["restart"]), ("apps", "apps", False))
        self.assertEqual(apps["sha256"], pack["sha256"])
        self.assertEqual(apps["content_id"], ids["apps"]["content_id"])
        self.assertEqual(apps["apps"]["kmd_abis"], ["0x000700D8"])
        self.assertEqual(apps["apps"]["state_schemas"], [2])
        self.assertEqual([f["path"] for f in apps["apps"]["files"]], ["amdgpu_wddm_control.exe", "strings/en.txt"])

    def test_manifest_takes_the_recorded_identity(self):
        m = self.pkg.write_manifest()
        m["update_components"] = {"driver": {"content_id": "D" * 64}, "apps": {"content_id": "A" * 64}}
        (self.pkg.root / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
        pack = nagi_update.make_packs(self.pkg.root, RELEASE, Path(self.tmp) / "out")
        out = nagi_update.make_manifest(self.pkg.root, self.release_zip(), pack["path"], RELEASE)
        self.assertEqual([c["content_id"] for c in out["components"]], ["D" * 64, "A" * 64])

    def test_manifest_refusals(self):
        pack = nagi_update.make_packs(self.pkg.root, RELEASE, Path(self.tmp) / "out")
        with self.assertRaises(ValueError):
            nagi_update.make_manifest(self.pkg.root, self.release_zip(), pack["path"], "1.0")
        with self.assertRaises(ValueError):
            nagi_update.make_manifest(self.pkg.root, self.release_zip("amdgpu-wddm-tester-0.7.216.108-tester.36.zip"), pack["path"], RELEASE)
        extra = Path(self.tmp) / "extra" / f"nagi-apps-{RELEASE}.zip"
        extra.parent.mkdir()
        shutil.copy(pack["path"], extra)
        with zipfile.ZipFile(extra, "a") as z:
            z.writestr("evil.dll", b"x")
        with self.assertRaises(ValueError):
            nagi_update.make_manifest(self.pkg.root, self.release_zip(), extra, RELEASE)

    def test_cli(self):
        out = Path(self.tmp) / "cli"
        self.assertEqual(nagi_update.main(["packs", "--package", str(self.pkg.root), "--release", RELEASE, "--out", str(out)]), 0)
        z = self.release_zip()
        target = out / "nagi-update.json"
        self.assertEqual(nagi_update.main(["manifest", "--package", str(self.pkg.root), "--release-zip", str(z),
                                           "--apps-pack", str(out / f"nagi-apps-{RELEASE}.zip"), "--min-installed", RELEASE,
                                           "--out", str(target)]), 0)
        self.assertEqual(json.loads(target.read_text(encoding="utf-8"))["release"], RELEASE)
        self.assertEqual(nagi_update.main(["packs", "--package", str(self.pkg.root), "--release", "x", "--out", str(out)]), 1)


if __name__ == "__main__":
    unittest.main()
