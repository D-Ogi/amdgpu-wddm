"""Regression controls against the immutable measured client fixture."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("control", HERE / "analyze-runtime-audit-control.py")
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)
FIXTURE = HERE.parents[2] / "evidence/windows/2026-09-27-E34-audit-client001"

class AuditControlTests(unittest.TestCase):
    def test_measured_fixture_and_rejected_mutations(self):
        self.assertEqual(control.analyze(FIXTURE)["known_application_copy_bytes"], 3686400)
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            copy = Path(d)
            for p in FIXTURE.iterdir():
                if p.is_file():
                    shutil.copyfile(p, copy / p.name)
            for filename, before, after in (
                ("stdout.log", "bytes=3686400", "bytes=0"),
                ("stdout.log", "bad=0", "bad=1"),
                ("stderr.log", "marker=8", "marker=80"),
                ("modules.json", "CC82A2D9", "00000000"),
            ):
                with self.subTest(filename=filename, before=before):
                    path = copy / filename
                    original = path.read_text()
                    self.assertIn(before, original)
                    path.write_text(original.replace(before, after, 1))
                    with self.assertRaises((AssertionError, ValueError)):
                        control.analyze(copy)
                    path.write_text(original)

if __name__ == "__main__":
    unittest.main()
