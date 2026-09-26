import unittest
import importlib.util
from pathlib import Path
from piglit_remaining import remaining

class RemainderTests(unittest.TestCase):
    def test_retains_reviewed_failure_and_order(self):
        rows = {"b": {"result": "warn"}, "a": {"result": "pass"}}
        self.assertEqual(remaining(["a", "b", "c", "d"], rows, ["b"]), ["c", "d"])
        self.assertEqual(rows["b"]["result"], "warn")

    def test_no_unreviewed_or_spurious_review(self):
        for reviews in [[], ["b", "c"]]:
            with self.assertRaises(ValueError):
                remaining(["a", "b", "c"], {"b": {"result": "fail"}}, reviews)

    def test_no_crash_timeout_or_unfinished_resume(self):
        for status in ["crash", "timeout", "incomplete", "notrun"]:
            with self.assertRaises(ValueError):
                remaining(["a"], {"a": {"result": status}}, ["a"])

    def test_literal_hash_in_case_name(self):
        spec = importlib.util.spec_from_file_location("linux_guard", Path(__file__).with_name("piglit-linux-guard.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.literal_names("spec@shared-#column_major\n"), ["spec@shared-#column_major"])
        for text in ["a\na\n", "a \n", "\n"]:
            with self.assertRaises(ValueError):
                module.literal_names(text)

    def test_inventory_integrity(self):
        for names, rows in [(["a", "a"], {}), (["a"], {"b": {"result": "pass"}})]:
            with self.assertRaises(ValueError):
                remaining(names, rows, [])

if __name__ == "__main__":
    unittest.main()
