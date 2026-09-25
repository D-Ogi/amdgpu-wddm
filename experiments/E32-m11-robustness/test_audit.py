"""Regression controls for the audit against measured good/bad E32 artifacts."""
import hashlib
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from audit import ROOT, audit

EVIDENCE = ROOT / "evidence/windows/2026-09-25-E32-resource-close"
REFERENCE = ROOT / "evidence/linux/2026-09-21-E14-vulkan-compute-reference"


def checks(result):
    return {x["check"]: x["pass"] for x in result["checks"]}


class AuditControls(unittest.TestCase):
    def test_short_good_control_is_not_24_hour_acceptance(self):
        result = audit(EVIDENCE / "phase-02", REFERENCE)
        values = checks(result)
        self.assertEqual(result["status"], "FAIL")
        self.assertFalse(values["24 monotonic hours"])
        self.assertTrue(values["every compute result equals CPU and Linux"])
        self.assertTrue(values["every model text equals Linux with full GPU offload"])
        self.assertTrue(values["all final pool counts and bytes equal initial values"])
        self.assertTrue(values["no growth beyond first active-worker checkpoint"])

    def test_measured_old_driver_retention_is_detected(self):
        values = checks(audit(EVIDENCE / "phase-01", REFERENCE))
        self.assertFalse(values["no growth beyond first active-worker checkpoint"])
        self.assertFalse(values["all final pool counts and bytes equal initial values"])
        self.assertTrue(values["every compute result equals CPU and Linux"])

    def test_self_consistent_wrong_compute_output_still_fails_reference(self):
        temp_root = ROOT.parent / "scratch/tmp"
        with tempfile.TemporaryDirectory(prefix="m11-audit-", dir=temp_root) as temporary:
            assert Path(temporary).resolve().is_relative_to(temp_root.resolve())
            folder = Path(temporary) / "control"
            shutil.copytree(EVIDENCE / "phase-02", folder)
            path = folder / "cycle-000001/compute.out"
            raw = path.read_bytes()
            self.assertIn(b"7018cdd513a22325", raw)
            path.write_bytes(raw.replace(b"7018cdd513a22325", b"0000000000000000"))
            ledger = folder / "cycles.jsonl"
            rows = [json.loads(x) for x in ledger.read_text(encoding="utf-8-sig").splitlines()]
            rows[0]["stdout_file_sha256"]["compute"] = hashlib.sha256(path.read_bytes()).hexdigest().upper()
            ledger.write_text("\n".join(json.dumps(x) for x in rows) + "\n")
            values = checks(audit(folder, REFERENCE))
            self.assertTrue(values["raw output hashes equal cycle ledger"])
            self.assertFalse(values["every compute result equals CPU and Linux"])


if __name__ == "__main__":
    unittest.main()
