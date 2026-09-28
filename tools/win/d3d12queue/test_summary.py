# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import summarize_trace as summary


def ddi(edge, seq=1, tick=100, **fields):
    value = dict(event="ddi", edge=edge, sequence=seq, qpc=tick,
                 name="pfnCopyBufferRegion", thread=7)
    if edge == "end":
        value["status"] = "00000000"
    value.update(fields)
    return value


class SummaryTests(unittest.TestCase):
    def run_summary(self, events, trace=None):
        # Explicit temp root remains on P: when run from this workspace.
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[4] / "scratch") as temp:
            runtime = Path(temp) / "runtime.err"
            runtime.write_text("\n".join(json.dumps(e) if isinstance(e, dict) else e for e in events), encoding="utf-8")
            other = None
            if trace is not None:
                other = Path(temp) / "trace.jsonl"
                other.write_text("\n".join(json.dumps(e) for e in trace), encoding="utf-8")
            return summary.summarize(runtime, other)

    def test_pair_duration_and_normal_return(self):
        result = self.run_summary(["ordinary diagnostic text", dict(event="clock", frequency=1000),
                                   ddi("begin"), ddi("end", tick=125)])
        self.assertEqual(result["ddi"]["counts"]["paired"], 1)
        self.assertEqual(result["ddi"]["name_edges"]["pfnCopyBufferRegion"], 2)
        self.assertNotIn("names", result["ddi"])
        self.assertEqual(result["ddi"]["elapsed_ms"]["paired_total"], 25)
        self.assertEqual(result["ddi"]["last"]["outcome"], "normal_return")
        self.assertTrue(result["ddi"]["pairing_complete"])
        self.assertNotIn("gpu_success", result)

    def test_failure_and_unmatched(self):
        result = self.run_summary([ddi("begin"), ddi("end", status="80004005"),
                                   ddi("begin", seq=2), ddi("end", seq=3)])
        self.assertEqual(result["ddi"]["statuses"]["80004005"], 1)
        self.assertEqual(result["ddi"]["counts"]["unmatched_begin"], 1)
        self.assertEqual(result["ddi"]["counts"]["unmatched_end"], 1)
        self.assertFalse(result["ddi"]["pairing_complete"])

    def test_duplicate_does_not_overwrite_or_pair(self):
        result = self.run_summary([ddi("begin"), ddi("begin", tick=999),
                                   ddi("end", tick=200), ddi("end", tick=1000)])
        self.assertEqual(result["ddi"]["counts"]["paired"], 0)
        self.assertEqual(result["ddi"]["counts"]["duplicate_begin"], 1)
        self.assertEqual(result["ddi"]["counts"]["duplicate_end"], 1)
        self.assertEqual(result["ddi"]["anomaly_examples"][0]["issues"], ["duplicate_sequence"])

    def test_identity_and_order(self):
        result = self.run_summary([ddi("begin"), ddi("end", thread=8),
                                   ddi("end", seq=2), ddi("begin", seq=2),
                                   ddi("begin", seq=3), ddi("end", seq=3, tick=50)])
        self.assertEqual(result["ddi"]["counts"]["identity_mismatch"], 1)
        self.assertEqual(result["ddi"]["counts"]["order_mismatch"], 2)
        self.assertEqual(result["ddi"]["counts"]["paired"], 0)

    def test_corrupt_and_invalid_schema(self):
        result = self.run_summary(["{broken", dict(event="ddi", edge="begin", sequence=True),
                                   ddi("begin"), ddi("end")])
        self.assertEqual(result["input"]["invalid_json"], 1)
        self.assertEqual(result["input"]["invalid_schema"], 1)
        self.assertFalse(result["ddi"]["pairing_complete"])

    def test_hosted_ids_are_not_globally_paired(self):
        events = [dict(event="hosted-callback", edge="begin", sequence=1, op=6, thread=thread)
                  for thread in (1, 2)]
        events += [dict(event="hosted-callback", edge="end", sequence=1, op=6, status="00000000")]*2
        result = self.run_summary(events)
        self.assertEqual(result["hosted"]["counts"], {"begin": 2, "end": 2})
        self.assertEqual(result["hosted"]["pairing"], "not_attempted_device_local_ids")
        self.assertNotIn("paired", result["hosted"])
        self.assertEqual(result["hosted"]["operation_edges"]["6"], 4)
        self.assertNotIn("operations", result["hosted"])

    def test_allowlisted_last_scalars(self):
        result = self.run_summary([
            dict(event="ddi-format", format=28, output_present=1, support=15, pointer="SECRET"),
            dict(event="ddi-msaa", format=28, samples=4, flags=1, output_present=1, levels=2, payload="SECRET")])
        self.assertEqual(result["last_format"], dict(format=28, output_present=1, support=15))
        self.assertEqual(result["last_msaa"]["levels"], 2)
        self.assertNotIn("SECRET", json.dumps(result))

    def test_conflicting_or_missing_clock_has_no_duration(self):
        for clocks in ([], [dict(event="clock", frequency=1000), dict(event="clock", frequency=2000)]):
            result = self.run_summary(clocks + [ddi("begin"), ddi("end", tick=200)])
            self.assertNotIn("elapsed_ms", result["ddi"])

    def test_api_trace(self):
        result = self.run_summary([], [dict(sequence=1, elapsed_ms=10, phase="after",
                                               api="D3D12CreateDevice FL11_0", hr="80004001", pointer="SECRET")])
        self.assertEqual(result["api"]["last"]["hr"], "80004001")
        self.assertEqual(result["api"]["name_edges"]["D3D12CreateDevice FL11_0"], 1)
        self.assertNotIn("names", result["api"])
        self.assertNotIn("SECRET", json.dumps(result))

    def test_output_and_tracking_bounds(self):
        events = ["{SECRET" + "x" * summary.MAX_LINE]
        events += [ddi("begin", seq=i+1, name="pfnSynthetic"+str(i)) for i in range(2000)]
        with patch.object(summary, "MAX_IDS", 20):
            result = self.run_summary(events)
        self.assertEqual(result["input"]["oversized_lines"], 1)
        self.assertEqual(result["ddi"]["counts"]["untracked_records"], 1980)
        self.assertLessEqual(len(result["ddi"]["anomaly_examples"]), summary.MAX_EXAMPLES)
        self.assertLess(len(json.dumps(result)), 12000)
        self.assertNotIn("SECRET", json.dumps(result))


if __name__ == "__main__":
    unittest.main()
