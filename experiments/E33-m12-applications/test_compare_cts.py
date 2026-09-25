"""Regression tests for incomplete and misleading CTS comparisons."""
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest
from compare_cts import compare


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ["BC250_TEST_TMP"])
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cases = self.root / "cases.txt"
        self.cases.write_text("dEQP-VK.a\ndEQP-VK.b\n")

    def run_pair(self, windows, linux):
        paths = []
        for side, rows in (("windows", windows), ("linux", linux)):
            path = self.root / (side + ".jsonl")
            path.write_text("".join(json.dumps({"case": "dEQP-VK." + name, "status": status}) + "\n"
                                    for name, status in rows), encoding="utf-8-sig")
            paths.append(path)
        return compare(self.cases, *paths, self.root / "out")

    def test_complete_match_with_skip(self):
        rows = [("a", "Pass"), ("b", "NotSupported")]
        result = self.run_pair(rows, list(reversed(rows)))
        self.assertEqual(result["status"], "RESULTS_MATCH")
        self.assertEqual(result["counts"]["windows"]["NotSupported"], 1)

    def test_equal_partial_runs_do_not_pass(self):
        result = self.run_pair([("a", "Pass")], [("a", "Pass")])
        self.assertEqual(result["issues"]["missing_result"], 1)
        self.assertEqual(result["status"], "REVIEW_REQUIRED")

    def test_skip_difference_is_visible(self):
        result = self.run_pair([("a", "Pass"), ("b", "NotSupported")],
                               [("a", "Pass"), ("b", "Pass")])
        self.assertEqual(result["issues"]["status_difference"], 1)

    def test_equal_failures_do_not_pass(self):
        rows = [("a", "Pass"), ("b", "Fail")]
        result = self.run_pair(rows, rows)
        self.assertEqual(result["issues"]["matching_nonpassing_result"], 1)

    def test_modern_cts_statuses_require_review(self):
        original_root = self.root
        for status in ("DeviceLost", "CapabilityWarning", "Waiver"):
            with self.subTest(status=status):
                self.root = original_root / status
                self.root.mkdir()
                rows = [("a", "Pass"), ("b", status)]
                result = self.run_pair(rows, rows)
                self.assertEqual(result["issues"]["matching_nonpassing_result"], 1)

    def test_unexpected_cases_are_reported(self):
        rows = [("a", "Pass"), ("b", "Pass")]
        result = self.run_pair(rows + [("c", "Pass")], rows)
        self.assertEqual(result["issues"]["unexpected_result"], 1)

    def test_duplicate_result_is_rejected(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.run_pair([("a", "Pass"), ("a", "Fail")], [])
        self.assertFalse((self.root / "out/summary.json").exists())

    def test_unknown_status_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_pair([("a", "Passed")], [])

    def test_duplicate_expected_case_is_rejected(self):
        self.cases.write_text("dEQP-VK.a\ndEQP-VK.a\n")
        with self.assertRaises(sqlite3.IntegrityError):
            self.run_pair([], [])


if __name__ == "__main__":
    unittest.main()
