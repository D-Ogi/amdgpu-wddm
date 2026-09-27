import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("audit", ROOT / "analyze-uploader-events.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
# Captured from M680's actual uploader execution, not a handcrafted passing stream.
LOG = (ROOT.parents[2] / "evidence/windows/2026-09-27-E34-uploader-audit-build/control.stderr.txt").read_text(encoding="utf-8-sig")


class Events(unittest.TestCase):
    def test_actual_control_and_pointer_reuse(self):
        result = audit.analyze(LOG)
        self.assertEqual((result["allocations"], result["requested_bytes"], result["helper_copy_bytes"]), (4, 4144, 4112))
        self.assertEqual(len(result["managers"]), 2)
        self.assertTrue(result["all_observed_managers_closed"])

    def test_dropped_allocation_and_copy(self):
        for event in ("alloc", "copy", "map", "create", "release"):
            lines = LOG.splitlines()
            index = next(i for i, line in enumerate(lines) if f"event={event} " in line and (event != "release" or "capacity=4096" in line))
            del lines[index]
            with self.subTest(event=event), self.assertRaises(ValueError):
                audit.analyze("\n".join(lines))

    def test_invalid_range_and_duplicate(self):
        alloc = next(line for line in LOG.splitlines() if "event=alloc " in line)
        for changed in (alloc.replace("offset=0", "offset=4096"), alloc + "\n" + alloc):
            with self.assertRaises(ValueError):
                audit.analyze(LOG.replace(alloc, changed, 1))

    def test_truncated_tail_is_only_a_prefix(self):
        prefix = "\n".join(LOG.splitlines()[:12])
        with self.assertRaises(ValueError):
            audit.analyze(prefix)
        result = audit.analyze(prefix, allow_live=True)
        self.assertFalse(result["all_observed_managers_closed"])
        self.assertEqual(result["lifetime_coverage"], "observed prefix only")

    def test_empty(self):
        with self.assertRaises(ValueError):
            audit.analyze("")


if __name__ == "__main__":
    unittest.main()
