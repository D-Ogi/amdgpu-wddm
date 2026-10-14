"""Offline package-layout tests; no device, registry or certificate changes."""
import hashlib
import importlib.util
import json
import tempfile
import unittest
from unittest.mock import patch
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

_spec = importlib.util.spec_from_file_location("driver_package", Path(__file__).with_name("driver_package.py"))
package = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(package)
REPO = Path(__file__).resolve().parents[2]


class DriverPackageTests(unittest.TestCase):
    def sources(self):
        return {"opengl_icd": {"register": True, "architectures": ["x64"]},
                "mft_h264": {"register": True}, "files": []}

    def fixture(self, root):
        sources = self.sources()
        for row in package.members(sources):
            p = root / row["source"]
            p.parent.mkdir(parents=True, exist_ok=True)
            data = (b'{"ICD":{"library_path":".\\\\vulkan_radeon.dll","api_version":"1.3.0"}}'
                    if p.suffix == ".json" else row["source"].encode())
            p.write_bytes(data)
            sources["files"].append({"path": row["source"], "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        for row in sources['files']:
            if row['path'].endswith('.config'):
                p = root / row['path']
                hashes = [hashlib.sha256(p.with_name(name).read_bytes()).digest()
                          for name in ('amdgpu_wddm_dxvk.dll', 'amdgpu_wddm_radv.dll')]
                data = struct.pack('<11I32s32s', 0x4334314d, 1, 108, 0, 0xb100, 1, 1, 1, 0, 0, 0, *hashes)
                p.write_bytes(data)
                row.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
        inf = root / "payload/kmd/bc250kmd.inf"
        inf.parent.mkdir(parents=True)
        inf.write_bytes((REPO / "driver/kmd/bc250kmd.inf").read_bytes())
        return sources

    def test_unique_basenames_and_explicit_architectures(self):
        rows = package.members(self.sources())
        names = [Path(r["member"]).name.lower() for r in rows]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(rows), 26)
        self.assertTrue(all(r["member"].startswith("wow64/") for r in rows if r["arch"] == "x86"))
        self.assertNotIn("OpenGLDriverNameWow", package.registrations(rows))
        source = self.sources()
        source["opengl_icd"]["architectures"].append("x86")
        rows = package.members(source)
        self.assertEqual(package.registrations(rows)["OpenGLDriverNameWow"]["value"], ["%13%\\wow64\\opengl\\amdgpu_wddm_gl32.dll"])

    def test_inf_preserves_kernel_and_state_and_has_each_copy(self):
        text = (REPO / "driver/kmd/bc250kmd.inf").read_text()
        rows = package.members(self.sources())
        generated = package.render_inf(text, rows)
        self.assertIn('ServiceBinary = %13%\\bc250kmd.sys', generated)
        self.assertIn('AmdgpuStateCatalog', generated)
        self.assertIn('HKR,,UserModeDriverName,0x00010000,"<>"', generated)
        self.assertIn('HKR,,UserModeDriverNameWow,0x00010000,"<>"', generated)
        for row in rows:
            path = Path(row["member"])
            folder = str(path.parent).replace("/", "\\")
            self.assertIn(f'{path.name} = 1,{folder}', generated)
            self.assertIn(f' = 13,{folder}', generated)
            self.assertIn('\n' + path.name + '\n', generated)
        with self.assertRaises(ValueError):
            package.render_inf(generated, rows)

    def test_move_preserves_pe_bytes_and_binds_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.fixture(root)
            with patch.object(package, 'image_digest', side_effect=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()):
                report = package.stage(root, sources)
            for row in report["members"]:
                p = root / row["path"]
                self.assertFalse((root / row["source"]).exists())
                self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest().upper(), row["sha256"])
                if p.suffix == '.dll':
                    self.assertEqual(p.read_bytes(), row["source"].encode())
            wow = root / "payload/kmd/wow64/vulkan/radeon_icd32.json"
            self.assertEqual(json.loads(wow.read_text())["ICD"]["library_path"], ".\\vulkan_radeon32.dll")
            config = (root / 'payload/kmd/d3d11/amdgpu_wddm_d3d11.config').read_bytes()
            fields = struct.unpack('<17I32s32s', config)
            self.assertEqual(fields[:5], (0x4334314d, 3, 132, 0, 0xb100))
            self.assertEqual(fields[11:17], (0,) * 6)

    def test_config_refuses_unmeasured_pair_before_move(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.fixture(root)
            row = next(r for r in sources['files'] if r['path'].endswith('amdgpu_wddm_dxvk.dll'))
            (root / row['path']).write_bytes(b'new unmeasured engine')
            row.update(size=21, sha256=hashlib.sha256(b'new unmeasured engine').hexdigest())
            with self.assertRaisesRegex(ValueError, 'does not pin'):
                package.stage(root, sources)
            self.assertFalse((root / 'payload/kmd/desktop').exists())

    def test_reject_mismatch_before_any_move(self):
        for fault in ("changed", "unlisted", "missing"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                sources = self.fixture(root)
                first = root / sources["files"][0]["path"]
                if fault == "changed":
                    first.write_bytes(b"wrong")
                elif fault == "unlisted":
                    sources["files"].append({"path": "payload/desktop/unlisted.dll"})
                else:
                    sources["files"].pop()
                with self.assertRaises(ValueError):
                    package.stage(root, sources)
                self.assertTrue(first.exists())
                self.assertFalse((root / "payload/kmd/desktop").exists())

    def test_reject_duplicate_source_and_existing_target(self):
        for fault in ("duplicate", "existing"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                sources = self.fixture(root)
                if fault == "duplicate":
                    sources["files"].append(sources["files"][0])
                else:
                    p = root / "payload/kmd/desktop/bc250d3d_router.dll"
                    p.parent.mkdir()
                    p.write_bytes(b"keep")
                with self.assertRaises(ValueError):
                    package.stage(root, sources)
                if fault == "existing":
                    self.assertEqual(p.read_bytes(), b"keep")

    def test_reject_bad_member_names(self):
        for value in ("../escape.dll", "x//file.dll", "x\\file.dll", "/root.dll", "C:/file.dll", "x/f.dll:ads", "x/f.dll.", "NUL.dll", "dir/COM1.json"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                package.relative(value)

    def test_malformed_json_refused_before_copies(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sources = self.fixture(root)
            row = next(r for r in sources['files'] if r['path'].endswith('.json'))
            bad = b'{"ICD":null}'
            (root / row['path']).write_bytes(bad)
            row.update(size=len(bad), sha256=hashlib.sha256(bad).hexdigest())
            with self.assertRaises(ValueError):
                package.stage(root, sources)
            self.assertFalse((root / 'payload/kmd/desktop').exists())
            self.assertTrue((root / sources['files'][0]['path']).exists())


if __name__ == "__main__":
    unittest.main()
