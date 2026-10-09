"""Pins compare.py's decisions on synthetic d3d11bench results. Run by build.ps1 before the compiler."""

import contextlib
import copy
import io
import json
import os
import tempfile
import unittest

import compare


def run(frame_ms, d3d11="app-local", checksum="00ff", total_ms=100.0, environment=None, mode="window"):
    stats = lambda median: {"count": 299, "median": median, "p5": median, "p95": median, "mean": median,
                            "min": median, "max": median}
    return {
        "tool": "d3d11bench", "format": 1, "result": "measured", "exit": 0, "mode": mode, "width": 1280,
        "height": 720, "feature_level": "11_1", "adapter": {"vendor": "1002", "device": "13fe"},
        "d3d11": d3d11, "environment": environment or {}, "dxvk_conf": {"exe_dir": False, "cwd": False},
        "scenes": [
            {"name": "draws", "frames": 300, "warmup": 30, "draws": 2000, "frame_ms": stats(frame_ms),
             "record_ms": stats(frame_ms / 2), "present_ms": stats(0.1), "gpu_ms": stats(1.0), "checksum": checksum},
            {"name": "shaders", "shaders": 64, "total_ms": total_ms, "create_ms": stats(0.5), "draw_ms": stats(0.2),
             "checksum": "beef"},
        ],
    }


def side(values, d3d11):
    return [run(v, d3d11) for v in values]


class CompareTest(unittest.TestCase):
    def test_within_bound_passes(self):
        rows, code = compare.compare(side([10.0, 10.05, 10.0], "app-local"), side([10.3, 10.3, 10.32], "system"))
        self.assertEqual(code, 0)
        self.assertEqual([r["verdict"] for r in rows], ["PASS", "PASS"])

    def test_beyond_bound_fails(self):
        rows, code = compare.compare(side([10.0, 10.0, 10.0], "app-local"), side([11.0, 11.0, 11.0], "system"))
        self.assertEqual(code, 1)
        self.assertEqual(rows[0]["verdict"], "FAIL")
        self.assertAlmostEqual(rows[0]["change"], 0.10)

    def test_noisy_runs_near_the_bound_are_inconclusive(self):
        rows, code = compare.compare(side([10.0, 9.6, 10.4], "app-local"), side([10.5, 10.5, 10.5], "system"))
        self.assertEqual(rows[0]["verdict"], "INCONCLUSIVE")
        self.assertEqual(code, 3)

    def test_noise_does_not_hide_a_clear_regression(self):
        rows, code = compare.compare(side([10.0, 9.6, 10.4], "app-local"), side([13.0, 13.0, 13.0], "system"))
        self.assertEqual(rows[0]["verdict"], "FAIL")
        self.assertEqual(code, 1)

    def test_different_output_fails(self):
        candidate = side([10.0, 10.0, 10.0], "system")
        candidate[1]["scenes"][0]["checksum"] = "0000"
        rows, code = compare.compare(side([10.0, 10.0, 10.0], "app-local"), candidate)
        self.assertEqual(rows[0]["verdict"], "FAIL")
        self.assertEqual(rows[0]["reason"], "output differs")
        self.assertEqual(code, 1)

    def test_different_settings_are_unusable(self):
        candidate = side([10.0], "system")
        candidate[0]["scenes"][0]["draws"] = 1000
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), candidate)
        other_mode = [run(10.0, "system", mode="offscreen")]
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), other_mode)
        other_scenes = side([10.0], "system")
        other_scenes[0]["scene_revision"] = 2   # revision 1 results carry none
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), other_scenes)
        other_interval = side([10.0], "system")
        other_interval[0]["sync_interval"] = 1  # BD-099: a capped run is not the same work as an uncapped one
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), other_interval)

    def test_a_missing_sync_interval_reads_as_zero(self):
        """Results from before the option carry no sync_interval, and all of them presented at interval 0."""
        explicit = side([10.0], "system")
        explicit[0]["sync_interval"] = 0
        _, code = compare.compare(side([10.0], "app-local"), explicit)
        self.assertEqual(code, 0)

    def test_paths_are_checked_unless_waived(self):
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "system"), side([10.0], "system"))
        _, code = compare.compare(side([10.0], "system"), side([10.0], "system"), check_paths=False)
        self.assertEqual(code, 0)

    def test_configuration_must_match_unless_waived(self):
        candidate = [run(10.0, "system", environment={"DXVK_HUD": "fps"})]
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), candidate)
        _, code = compare.compare(side([10.0], "app-local"), candidate, ignore_configuration=True)
        self.assertEqual(code, 0)

    def test_drivers_must_match_unless_waived(self):
        base = side([10.0], "app-local")
        base[0]["icds"] = [{"path": "C:\\lab\\vulkan_radeon.dll", "sha256": "AA"}]
        same = side([10.0], "system")
        same[0]["icds"] = [{"path": "C:\\umd\\bc250radv.dll", "sha256": "AA"}]
        _, code = compare.compare(base, same)
        self.assertEqual(code, 0)
        other = side([10.0], "system")
        other[0]["icds"] = [{"path": "C:\\umd\\bc250radv.dll", "sha256": "BB"}]
        with self.assertRaises(compare.InputError):
            compare.compare(base, other)
        _, code = compare.compare(base, other, ignore_configuration=True)
        self.assertEqual(code, 0)

    def test_incomplete_run_is_unusable(self):
        failed = side([10.0], "system")
        failed[0]["result"] = "failed"
        with self.assertRaises(compare.InputError):
            compare.compare(side([10.0], "app-local"), failed)

    def test_main_reads_files_and_returns_the_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            def write(name, data):
                path = os.path.join(tmp, name)
                with open(path, "w", encoding="utf-8") as f:
                    json.dump(data, f)
                return path
            base = [write(f"b{i}.json", run(10.0)) for i in range(2)]
            good = [write(f"c{i}.json", run(10.2, "system")) for i in range(2)]
            slow = [write(f"s{i}.json", run(12.0, "system")) for i in range(2)]
            broken = write("x.json", copy.deepcopy(run(10.0, "system")) | {"format": 2})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.main(["--base", *base, "--candidate", *good]), 0)
                self.assertEqual(compare.main(["--base", *base, "--candidate", *slow]), 1)
                self.assertEqual(compare.main(["--base", *base, "--candidate", broken]), 2)


if __name__ == "__main__":
    unittest.main()
