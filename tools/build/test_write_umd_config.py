"""Host tests for the binary deployment boundary; no GPU or measured-cap claims."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("write-umd-config.py")
spec = importlib.util.spec_from_file_location("writer", SCRIPT)
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.engine = self.root / "engine.dll"
        self.icd = self.root / "icd.dll"
        self.engine.write_bytes(b"synthetic engine")
        self.icd.write_bytes(b"synthetic ICD")
        self.caps = dict(maximum_feature_level=0xB100, doubles=1,
                         compute_raw_structured=1, logic_op=1, tile_based=0,
                         pixel_min_precision=2, other_min_precision=3,
                         engine_sha256=hashlib.sha256(self.engine.read_bytes()).hexdigest(),
                         icd_sha256=hashlib.sha256(self.icd.read_bytes()).hexdigest())

    def test_binary_abi(self):
        record = writer.encode(self.caps, self.engine, self.icd)
        self.assertEqual(len(record), 108)
        self.assertEqual(record[:4], b"M14C")
        self.assertEqual(struct.unpack("<11I", record[:44]),
                         (0x4334314D, 1, 108, 0, 0xB100, 1, 1, 1, 0, 2, 3))
        self.assertEqual(record[44:76], bytes.fromhex(self.caps["engine_sha256"]))
        self.assertEqual(record[76:108], bytes.fromhex(self.caps["icd_sha256"]))

    def test_capability_rejections(self):
        for key, value in (("maximum_feature_level", 0xC000), ("doubles", 2),
                           ("doubles", True), ("logic_op", 1.0), ("tile_based", -1),
                           ("pixel_min_precision", 4), ("other_min_precision", -1),
                           ("compute_raw_structured", 0), ("logic_op", 0)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                writer.encode(dict(self.caps, **{key: value}), self.engine, self.icd)

    def test_hash_rejections(self):
        for key in ("engine_sha256", "icd_sha256"):
            for value in ("0" * 64, "f" * 64, "x" * 64, "1" * 63, None):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    writer.encode(dict(self.caps, **{key: value}), self.engine, self.icd)

    def test_changed_artifact(self):
        self.icd.write_bytes(b"different ICD")
        with self.assertRaises(ValueError):
            writer.encode(self.caps, self.engine, self.icd)

    def test_cli_preserves_existing_and_refuses_missing_input(self):
        caps = self.root / "caps.json"
        out = self.root / "amdgpu_wddm_d3d11.config"
        caps.write_text(json.dumps(self.caps), encoding="utf-8-sig")
        cmd = [sys.executable, str(SCRIPT), "--caps", str(caps), "--engine", str(self.engine),
               "--icd", str(self.icd), "--out", str(out)]
        run = lambda: subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(run().returncode, 0)
        out.write_bytes(b"preserve prior package")
        self.assertNotEqual(run().returncode, 0)
        self.assertEqual(out.read_bytes(), b"preserve prior package")
        out.unlink()
        caps.unlink()
        self.assertNotEqual(run().returncode, 0)
        self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
