# SPDX-License-Identifier: MIT
import json
import tempfile
import unittest
from pathlib import Path

import recordbench_compare as compare

HEADER = ("phase,frame,record_cycles,work_cycles,submit_cycles,api_cycles_max,api_cycles_sum,record_us,submit_us,wait_us,"
          "verify_us,frame_us\n")


def write_session(root, name, experiment, api_cycles, frame_us, mismatches=0, hr="00000000"):
    directory = Path(root) / name
    directory.mkdir()
    lines = [f"Record bench seed 0123456789abcdef, lists 4, draws per list 512, executes 2, work us per group 64, phases "
             f"until 20000 ms, cycles per us 1000.0, AMDGPU_WDDM_D3D12_EXPERIMENT {experiment}"]
    for phase in compare.PHASES:
        lines.append(f"Record bench {phase} wall us: record mean 1.0 p50 1.0 p95 1.0, submit mean 1.0, gpu wait mean 1.0, "
                     f"verify mean 1.0, frame mean 1.0 p50 1.0 p95 1.0; cpu ms per frame: main thread 2.500, process 3.000, "
                     f"over 5000 ms")
    lines.append(f"Record bench: phases measured 3 of 3, frames 30, words 100, mismatches {mismatches}, mismatched draws 0, "
                 f"removals 0, failures 0, 20000 ms, cycles per us at the end 1001.5, AMDGPU_WDDM_D3D12_EXPERIMENT {experiment}")
    with open(directory / "trace.jsonl", "w", encoding="utf-8") as trace:
        trace.write(json.dumps(dict(sequence=3, elapsed_ms=1, phase="after", api="copy", hr="00000000")) + "\n")
        for index, api in enumerate(lines):
            code = hr if index == len(lines) - 1 else "00000000"
            trace.write(json.dumps(dict(sequence=3, elapsed_ms=2, phase="after", api=api, hr=code)) + "\n")
    with open(directory / "recordbench-frames.csv", "w", encoding="utf-8", newline="") as rows:
        rows.write(HEADER)
        for phase in compare.PHASES:
            for frame in range(16, 26):
                rows.write(f"{phase},{frame},0,0,0,{api_cycles},{api_cycles},10.0,1.0,1.0,1.0,{frame_us}\n")
    return directory


class RecordBenchCompareTest(unittest.TestCase):
    def test_table_and_change(self):
        with tempfile.TemporaryDirectory() as root:
            off = compare.session(write_session(root, "off", "absent", 4_000_000, 20000.0))
            on = compare.session(write_session(root, "on", "deferred-replay", 1_000_000, 15000.0))
            self.assertEqual(off["experiment"], "absent")
            self.assertEqual(on["experiment"], "deferred-replay")
            self.assertEqual(off["final"]["end_rate"], 1001.5)
            self.assertTrue(compare.passed(off) and compare.passed(on))
            figures = compare.phase_figures(on, "threaded")
            self.assertEqual(figures["frames"], 10)
            self.assertAlmostEqual(figures["api_us"], 1000.0)
            self.assertAlmostEqual(figures["thread_ms"], 2.5)
            text = compare.table(off, on)
            self.assertIn("api cpu us/frame", text)
            self.assertIn("-75.0 %", text)
            self.assertIn("-25.0 %", text)
            self.assertNotIn("did not pass", text)

    def test_failed_session_is_named(self):
        with tempfile.TemporaryDirectory() as root:
            off = compare.session(write_session(root, "off", "absent", 4_000_000, 20000.0))
            bad = compare.session(write_session(root, "bad", "deferred-replay", 1_000_000, 15000.0, mismatches=3, hr="80004005"))
            self.assertFalse(compare.passed(bad))
            self.assertIn("B did not pass", compare.table(off, bad))

    def test_missing_start_line(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "empty"
            directory.mkdir()
            (directory / "trace.jsonl").write_text("", encoding="utf-8")
            (directory / "recordbench-frames.csv").write_text(HEADER, encoding="utf-8")
            with self.assertRaises(ValueError):
                compare.session(directory)


if __name__ == "__main__":
    unittest.main()
