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

    def test_get_caps_and_layout_set_last_status(self):
        result = self.run_summary([
            dict(event="ddi-caps", type=1002, data_size=28, info_present=0, status="00000000"),
            dict(event="ddi-caps", type=1003, data_size=20, info_present=1,
                 status="8000400A", pointer="SECRET"),
            dict(event="ddi-layout-set", layout=1, unit=0, status="00000000", payload="SECRET")])
        self.assertEqual(result["last_caps"], dict(type=1003, data_size=20, info_present=1, status="8000400a"))
        self.assertEqual(result["last_layout_set"], dict(layout=1, unit=0, status="00000000"))
        self.assertEqual(result["input"]["ddi-caps"], 2)
        self.assertEqual(result["input"]["ddi-layout-set"], 1)
        self.assertNotIn("SECRET", json.dumps(result))

    def test_status_observation_invalid_scalars_preserve_previous(self):
        good = [dict(event="ddi-caps", type=1003, data_size=20, info_present=1, status="80004001"),
                dict(event="ddi-layout-set", layout=1, unit=0, status="00000000")]
        for valid in good:
            invalid = []
            for field in ("type", "data_size", "info_present") if valid["event"] == "ddi-caps" else ("layout", "unit"):
                for value in (True, "0", -1, 1 << 32):
                    invalid.append(dict(valid, **{field: value}))
                missing = valid.copy()
                del missing[field]
                invalid.append(missing)
            if valid["event"] == "ddi-caps":
                invalid.append(dict(valid, info_present=2))
            key = "last_caps" if valid["event"] == "ddi-caps" else "last_layout_set"
            result = self.run_summary([valid] + invalid)
            self.assertEqual(result["input"]["invalid_schema"], len(invalid))
            self.assertEqual(result[key], {k: v for k, v in valid.items() if k != "event"})

    def test_status_observation_rejects_invalid_or_missing_status(self):
        for event, scalar_fields, key in (
                ("ddi-caps", dict(type=1003, data_size=20, info_present=1), "last_caps"),
                ("ddi-layout-set", dict(layout=1, unit=0), "last_layout_set")):
            valid = dict(event=event, **scalar_fields, status="80004001")
            invalid = [dict(valid, status=value) for value in
                       (None, True, 0, "", "0000000", "000000000", "0x00000000", "GGGGGGGG", "00000000\n")]
            invalid.append(dict(event=event, **scalar_fields))
            result = self.run_summary([valid] + invalid)
            self.assertEqual(result["input"]["invalid_schema"], len(invalid))
            self.assertEqual(result[key]["status"], "80004001")
            self.assertEqual(result["input"][event], 1)

    def test_many_status_observations_remain_bounded(self):
        result = self.run_summary([dict(event="ddi-caps", type=index, data_size=20,
                                       info_present=0, status="00000000") for index in range(2000)])
        self.assertEqual(result["last_caps"]["type"], 1999)
        self.assertIsNone(result["last_layout_set"])
        self.assertLess(len(json.dumps(result)), 2000)

    def test_caps_and_node_observations_allowlist(self):
        memory = dict(event="ddi-caps-memory", type=1002, node=0, uma=1,
                      io_coherent=1, cache_coherent=0, heap_serialization=2,
                      resource_serialization=3)
        layout = dict(event="ddi-caps-layout", type=1060, layouts=7, swizzles=4,
                      standard64k=1, row_major=1, indexable=0)
        node = dict(event="ddi-node-map", count=1, first=0, lost=0)
        events = [dict(memory, pointer="SECRET"), dict(layout, payload="SECRET"),
                  dict(node, handle="SECRET")]
        result = self.run_summary(events)
        for key, event in (("last_caps_memory", memory), ("last_caps_layout", layout), ("last_node_map", node)):
            self.assertEqual(result[key], {k: v for k, v in event.items() if k != "event"})
            self.assertEqual(result["input"][event["event"]], 1)
        self.assertNotIn("SECRET", json.dumps(result))

    def test_invalid_caps_do_not_replace_last_valid_observation(self):
        valid = dict(event="ddi-caps-memory", type=1002, node=0, uma=1,
                     io_coherent=1, cache_coherent=0, heap_serialization=0,
                     resource_serialization=0)
        invalid = [dict(valid, type=1060), dict(valid, node=-1),
                   dict(valid, node=1 << 32), dict(valid, uma=True),
                   dict(valid, cache_coherent=2), dict(valid, resource_serialization="0")]
        missing = valid.copy()
        del missing["heap_serialization"]
        result = self.run_summary([valid] + invalid + [missing])
        self.assertEqual(result["input"]["invalid_schema"], 7)
        self.assertEqual(result["input"]["ddi-caps-memory"], 1)
        self.assertEqual(result["last_caps_memory"]["type"], 1002)
        self.assertEqual(result["last_caps_memory"]["node"], 0)
        self.assertIsNone(result["last_caps_layout"])
        self.assertIsNone(result["last_node_map"])

    def test_caps_layout_and_node_flags_are_numeric_bits(self):
        layout = dict(event="ddi-caps-layout", type=1060, layouts=0xffffffff,
                      swizzles=0xffffffff, standard64k=0, row_major=1, indexable=1)
        node = dict(event="ddi-node-map", count=0, first=0, lost=1)
        result = self.run_summary([layout, node, dict(layout, type=1002),
                                   dict(layout, row_major=2), dict(node, lost=False),
                                   dict(node, count=1 << 32)])
        self.assertEqual(result["input"]["invalid_schema"], 4)
        self.assertEqual(result["last_caps_layout"]["layouts"], 0xffffffff)
        self.assertEqual(result["last_node_map"], dict(count=0, first=0, lost=1))

    def test_get_caps_pair_uses_existing_json_schema(self):
        result = self.run_summary([ddi("begin", name="pfnGetCaps"),
                                   ddi("end", name="pfnGetCaps", tick=110),
                                   "other: " + json.dumps(ddi("begin"))])
        self.assertEqual(result["ddi"]["counts"]["paired"], 1)
        self.assertEqual(result["ddi"]["name_edges"]["pfnGetCaps"], 2)
        self.assertEqual(result["input"]["text_lines"], 1)
        self.assertTrue(result["ddi"]["pairing_complete"])

    def test_many_observations_keep_only_last(self):
        events = [dict(event="ddi-node-map", count=1, first=index, lost=0,
                       payload="SECRET") for index in range(2000)]
        result = self.run_summary(events)
        self.assertEqual(result["input"]["ddi-node-map"], 2000)
        self.assertEqual(result["last_node_map"], dict(count=1, first=1999, lost=0))
        self.assertLess(len(json.dumps(result)), 2000)
        self.assertNotIn("SECRET", json.dumps(result))

    def test_conflicting_or_missing_clock_has_no_duration(self):
        for clocks in ([], [dict(event="clock", frequency=1000), dict(event="clock", frequency=2000)]):
            result = self.run_summary(clocks + [ddi("begin"), ddi("end", tick=200)])
            self.assertNotIn("elapsed_ms", result["ddi"])

    def test_heap_observation_widths_flags_and_allowlist(self):
        event = "ddi-create-heap-resource"
        fields = summary.OBSERVATIONS[event][1]
        valid = dict.fromkeys(fields, 0)
        valid.update(event=event, heap_present=1, heap_readable=1, bytes=1 << 40,
                     alignment=65536, resource_present=1, resource_readable=1,
                     resource_type=1, layout=1, width=(1 << 64)-1)
        invalid = [dict(valid, bytes=1 << 64), dict(valid, width=-1),
                   dict(valid, row_pitch=1 << 32), dict(valid, heap_readable=True),
                   dict(valid, row_major_present=2), dict(valid, num_castable_formats="0")]
        missing = valid.copy()
        del missing["slice_pitch"]
        result = self.run_summary([dict(valid, handle="SECRET", pointer="SECRET")] + invalid + [missing])
        self.assertEqual(result["last_heap_resource"], {k: v for k, v in valid.items() if k != "event"})
        self.assertEqual(result["input"]["invalid_schema"], len(invalid)+1)
        self.assertEqual(result["input"][event], 1)
        self.assertNotIn("SECRET", json.dumps(result))

    def test_shell_allocation_pair_is_explicitly_named(self):
        result = self.run_summary([ddi("begin", name="shellAllocateMemory"),
                                   ddi("end", name="shellAllocateMemory", tick=110, code="80004001")])
        self.assertEqual(result["ddi"]["counts"]["paired"], 1)
        self.assertTrue(result["ddi"]["pairing_complete"])
        self.assertEqual(result["ddi"]["last"]["name"], "shellAllocateMemory")
        self.assertIsNone(summary.ddi_name("shellArbitraryPayload"))

    def test_api_trace(self):
        result = self.run_summary([], [dict(sequence=1, elapsed_ms=10, phase="after",
                                               api="D3D12CreateDevice FL11_0", hr="80004001", pointer="SECRET")])
        self.assertEqual(result["api"]["last"]["hr"], "80004001")
        self.assertEqual(result["api"]["name_edges"]["D3D12CreateDevice FL11_0"], 1)
        self.assertNotIn("names", result["api"])
        self.assertNotIn("SECRET", json.dumps(result))

    CHURN_START = ("Reset churn seed 0123456789abcdef, threads 4, batches 2000, starts until 20000 ms, draws per list 1024, "
                   "renew 2, root words 32, cbv words 16, ring regions 2")

    @staticmethod
    def churn(api, hr="00000000", phase="after", elapsed=100, thread=None):
        value = dict(sequence=3, elapsed_ms=elapsed, phase=phase, api=api, hr=hr)
        if thread is not None:
            value["thread"] = thread
        return value

    @staticmethod
    def churn_summary(batches=600, mismatches=0, removals=0, failures=0, budget="reached"):
        return (f"Reset churn: batches {batches} of 2000, threads 4, draws per list 1024, lists {batches * 4}, "
                f"draws {batches * 4096}, words {batches * 4096 * 48}, mismatches {mismatches}, mismatched draws {min(mismatches, 1)}, "
                f"removals {removals}, record failures {failures}, renewals {batches * 2}, resets {batches * 2}, "
                f"time budget {budget}, 20012 ms, batch ms mean 33 max 71, record 12, submit 15, verify 6")

    def test_reset_churn_pass(self):
        trace = [self.churn("copy", phase="before"), self.churn(self.CHURN_START),
                 self.churn("Reset churn CreateRootSignature", phase="before"), self.churn("Reset churn CreateRootSignature"),
                 self.churn("Reset churn CreateCommandAllocator", phase="before", thread=2),
                 self.churn("Reset churn CreateCommandAllocator", thread=2), self.churn("Reset churn setup done"),
                 self.churn("Reset churn progress: batch 550, lists 2200, mismatches 0, 18500 ms")]
        trace += [self.churn(f"Reset churn thread {t} end: lists 600, renewals 300, resets 300", thread=t) for t in range(4)]
        trace += [self.churn(self.churn_summary()), self.churn("copy")]
        churn = self.run_summary([], trace)["api"]
        self.assertNotIn("invalid_schema", churn["input"])
        self.assertEqual(churn["name_edges"]["Reset churn"], 12)
        self.assertEqual(churn["last"]["api"], "copy")
        churn = churn["reset_churn"]
        self.assertEqual(churn["criterion"], "pass")
        self.assertEqual(churn["reasons"], [])
        self.assertEqual(churn["config"]["seed"], "0123456789abcdef")
        self.assertEqual(churn["config"]["draws_per_list"], 1024)
        self.assertEqual(churn["summary"]["batches"], 600)
        self.assertEqual(churn["summary"]["words"], 600 * 4096 * 48)
        self.assertEqual(churn["summary"]["time_budget"], "reached")
        self.assertEqual(churn["summary"]["hr"], "00000000")
        self.assertEqual(churn["last_progress"]["batch"], 550)
        self.assertEqual(churn["thread_ends"]["3"], dict(lists=600, renewals=300, resets=300))
        self.assertEqual(churn["counts"]["other"], 5)
        self.assertNotIn("failures", churn["counts"])

    def test_reset_churn_removal(self):
        removed = "887a0005"
        trace = [self.churn(self.CHURN_START), self.churn("Reset churn setup done"),
                 self.churn("Reset churn batch 12 thread 3: ResetCommandList", hr=removed, thread=3),
                 self.churn("Reset churn batch 12: recording failed on thread 3", hr=removed),
                 self.churn(f"Reset churn device removed at batch 12: reason {removed}", hr=removed),
                 self.churn(self.churn_summary(batches=12, removals=1, failures=1, budget="not reached"), hr=removed)]
        churn = self.run_summary([], trace)["api"]["reset_churn"]
        self.assertEqual(churn["criterion"], "fail")
        self.assertEqual(churn["removal"], dict(batch=12, reason=removed))
        self.assertEqual(churn["reasons"], ["device_removed", "record_failure", "failure_lines", "status"])
        self.assertEqual(churn["failure_examples"][0], dict(api="Reset churn batch 12 thread 3: ResetCommandList", hr=removed))
        self.assertEqual(churn["counts"]["failures"], 2)
        # A removal line alone fails the run even when the process never wrote its summary.
        churn = self.run_summary([], trace[:1] + trace[4:5])["api"]["reset_churn"]
        self.assertEqual(churn["criterion"], "fail")
        self.assertEqual(churn["reasons"], ["device_removed", "no_summary"])

    def test_reset_churn_mismatches_are_parsed_and_bounded(self):
        head = "Reset churn mismatch thread 2 batch 42 draw 7 cbv word 3, readback offset 1484, upload offset 1804, expected 1a2b3c4d, actual "
        sources = ["5e6f7a8b, actual is from thread 1 batch 40 draw 9 root word 31", "00000007, actual is a slot index",
                   "deadbeef, actual is unknown"]
        trace = [self.churn(self.CHURN_START)]
        trace += [self.churn(head + sources[i % 3], hr="80004005") for i in range(3 * summary.MAX_EXAMPLES)]
        trace.append(self.churn("Reset churn mismatch thread 0 batch 1 draw 0 root word 0, readback offset 0, upload offset none, "
                                "expected 00000000, actual 00000005, actual is a slot index", hr="80004005"))
        churn = self.run_summary([], trace)["api"]["reset_churn"]
        self.assertEqual(churn["counts"]["mismatch"], 3 * summary.MAX_EXAMPLES + 1)
        self.assertEqual(len(churn["mismatch_examples"]), summary.MAX_EXAMPLES)
        first = churn["mismatch_examples"][0]
        self.assertEqual(first, dict(thread=2, batch=42, draw=7, part="cbv", word=3, readback_offset=1484, upload_offset=1804,
                                     expected="1a2b3c4d", actual="5e6f7a8b",
                                     source=dict(kind="run", thread=1, batch=40, draw=9, part="root", word=31)))
        self.assertEqual(churn["mismatch_examples"][1]["source"], dict(kind="slot_index"))
        self.assertEqual(churn["mismatch_examples"][2]["source"], dict(kind="unknown"))
        self.assertEqual(churn["criterion"], "fail")
        self.assertEqual(churn["reasons"], ["mismatch", "no_summary"])
        self.assertNotIn("failures", churn["counts"])
        self.assertLess(len(json.dumps(churn)), 6000)

    def test_reset_churn_incomplete_without_summary(self):
        trace = [self.churn(self.CHURN_START), self.churn("Reset churn setup done"),
                 self.churn("Reset churn progress: batch 50, lists 200, mismatches 0, 1700 ms")]
        churn = self.run_summary([], trace)["api"]["reset_churn"]
        self.assertEqual(churn["criterion"], "incomplete")
        self.assertEqual(churn["reasons"], ["no_summary"])
        self.assertIsNone(churn["summary"])
        # Mismatches reported by progress alone are enough to fail.
        trace.append(self.churn("Reset churn progress: batch 100, lists 400, mismatches 3, 3400 ms", hr="80004005"))
        self.assertEqual(self.run_summary([], trace)["api"]["reset_churn"]["reasons"], ["mismatch", "no_summary"])

    def test_reset_churn_malformed_lines_block_a_pass(self):
        malformed = ["Reset churn: batches many of 2000", "Reset churn mismatch thread 1",
                     "Reset churn device removed at batch 12: reason 887A0005",
                     "Reset churn progress: batch 99999999999999999999999, lists 1, mismatches 0, 1 ms",
                     "Reset churn thread 1 end: lists 1", self.CHURN_START + " extra"]
        trace = [self.churn(self.CHURN_START)] + [self.churn(text) for text in malformed] + [self.churn(self.churn_summary())]
        trace += [self.churn("Reset churnX"), self.churn("Reset churn " + "x" * 600)]
        result = self.run_summary([], trace)["api"]
        self.assertEqual(result["input"]["invalid_schema"], 2)
        churn = result["reset_churn"]
        self.assertEqual(churn["counts"]["malformed"], len(malformed))
        self.assertEqual(churn["criterion"], "incomplete")
        self.assertEqual(churn["reasons"], ["malformed_lines"])
        self.assertIsNone(churn["removal"])
        self.assertIsNone(churn["last_progress"])
        self.assertEqual(churn["thread_ends"], {})
        # Other runs carry no reset churn section.
        self.assertNotIn("reset_churn", self.run_summary([], [self.churn("copy")])["api"])

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
