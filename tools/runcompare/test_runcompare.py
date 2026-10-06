#!/usr/bin/env python3
"""Tests for runcompare.

Two kinds of fixture, on purpose:

  - short synthetic logs written in this file, for the parsing and normalization rules,
    which are easier to pin down when the log holds nothing else;
  - the real kept logs and state files of E16 runs 002, 003 and 004 under evidence/,
    read only, because a rule that works on invented lines and not on the lab's own
    output is worth nothing.

    python -m unittest discover -s tools/runcompare
"""

import json
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import runcompare as rc  # noqa: E402

EVIDENCE = REPO / "evidence" / "windows"
RUN_002 = EVIDENCE / "2026-09-21-E16-run-002"
RUN_003 = EVIDENCE / "2026-09-21-E16-run-003"
RUN_004 = EVIDENCE / "2026-09-21-E16-run-004"

HAVE_EVIDENCE = RUN_002.is_dir() and RUN_003.is_dir() and RUN_004.is_dir()


def ring_from(lines, name="ring-synthetic.log"):
    return rc.RingLog(name, lines)


def run_from(lines, name="synthetic", state=None):
    return rc.Run(name, ring_from(lines), state, [state] if state else [], [])


# A whole start, stop included, in as few lines as the driver can write one.
MINIMAL = [
    "bc250kmd 0x00070003 log kept at the stop: 8 lines, 0 lost to the wrap, 0 dropped above DISPATCH_LEVEL",
    "     0      0.000 stage 10",
    "     1      0.000 gate: EnableFullWddm 1",
    "     2      0.100 stage 20",
    "     3      0.200 stage 39",
    "     4      0.200 wddm: DRIVERCAPS 576 of 576 bytes: wddm 8192 sched 0x45 mm 0x60 flip 0x2 slots 0",
    "     5      0.200 wddm: QueryAdapterInfo type 1 in 0 out 576 -> 0x00000000",
    "     6      0.300 stage 70",
    "     7      0.300 stage 79",
]


class TestTables(unittest.TestCase):
    """The names come from the headers, so a wrong one is a regenerated-table problem."""

    def test_query_adapter_info_types(self):
        self.assertEqual(rc.qai_name(1), "DRIVERCAPS")
        self.assertEqual(rc.qai_name(13), "GPUMMUCAPS")
        self.assertEqual(rc.qai_name(15), "PHYSICALADAPTERCAPS")
        self.assertEqual(rc.qai_name(47), "64BITONLYCAPS")

    def test_unknown_type_is_not_invented(self):
        self.assertIn("not in this Kit's enum", rc.qai_name(9999))

    def test_ntstatus(self):
        self.assertEqual(rc.status_name(0x00000000), "STATUS_SUCCESS")
        self.assertEqual(rc.status_name(0xC00000BB), "STATUS_NOT_SUPPORTED")
        self.assertEqual(rc.status_name(0xDEADBEEF), "0xDEADBEEF")

    def test_nt_success(self):
        self.assertTrue(rc.status_ok(0x00000000))
        self.assertTrue(rc.status_ok(0x401E000A))     # informational is still success
        self.assertFalse(rc.status_ok(0xC00000BB))
        self.assertFalse(rc.status_ok(0x80000005))    # a warning is not NT_SUCCESS

    def test_cm_prob_43(self):
        self.assertEqual(rc.cm_prob_label("CM_PROB_FAILED_POST_START"),
                         "CM_PROB_FAILED_POST_START (Code 43)")
        self.assertEqual(rc.cm_prob_label("CM_PROB_NONE"), "CM_PROB_NONE (Code 0)")

    def test_stage_names(self):
        self.assertEqual(rc.stage_label(39), "stage 39 (StageStartDone)")
        self.assertEqual(rc.stage_label(70), "stage 70 (StageStopEnter)")
        self.assertEqual(rc.stage_label(12345), "stage 12345")


class TestRingParsing(unittest.TestCase):
    def test_kept_header(self):
        ring = ring_from(MINIMAL)
        self.assertTrue(ring.kept)
        self.assertEqual(ring.driver_version, 0x00070003)
        self.assertEqual(ring.lost, 0)
        self.assertEqual(len(ring.entries), 8)
        self.assertEqual(ring.unparsed, [])

    def test_live_header_and_trailer(self):
        ring = ring_from([
            "log     2 lines since this driver load, 0 lost to the wrap, 0 dropped above "
            "DISPATCH_LEVEL; ring 1024 lines of which the first 256 are kept; table display-only",
            "       0      0.002 stage 10",
            "       1      0.002 gate: EnableFullWddm 0",
            "                       2 lines printed",
        ])
        self.assertFalse(ring.kept)
        self.assertEqual(ring.table, "display-only")
        self.assertEqual(ring.gate("EnableFullWddm"), 0)
        self.assertEqual(ring.unparsed, [])

    def test_sequence_starts_at_the_last_stage_10(self):
        """A ring survives a stop and start; only the last load is the run under test."""
        lines = MINIMAL + [
            "     8      9.000 stage 10",
            "     9      9.000 gate: EnableFullWddm 1",
            "    10      9.100 stage 39",
        ]
        ring = ring_from(lines)
        self.assertEqual([e.text for e in ring.sequence()],
                         ["stage 10", "gate: EnableFullWddm 1", "stage 39"])

    def test_a_file_that_is_not_a_ring(self):
        ring = ring_from(["bc250kmd_cli.exe : no display adapter with hardware id PCI\\VEN_1002&DEV_13FE"])
        self.assertFalse(ring.is_ring)


class TestClassification(unittest.TestCase):
    def test_query_adapter_info(self):
        kind, key, data = rc.classify("wddm: QueryAdapterInfo type 15 in 4 out 20 -> 0xC00000BB")
        self.assertEqual(kind, "qai")
        self.assertEqual(data, {"type": 15, "in": 4, "out": 20, "status": 0xC00000BB})
        self.assertEqual(key, "wddm: QueryAdapterInfo type 15 in 4 out 20 -> 0xC00000BB")

    def test_caps_fields_of_both_wordings(self):
        _, key_old, old = rc.classify(
            "wddm: DRIVERCAPS 576 of 576 bytes: wddm 8192 sched 0x45 mm 0x60 paging node 0 flip 0x2 slots 0")
        _, key_new, new = rc.classify(
            "wddm: DRIVERCAPS into 576 bytes: wddm 8192 sched 0x45 mm 0x60 flip 0x12 slots 0 "
            "tdr 1 dflip 1 rot 1")
        # The two builds word the line differently; the alignment must not trip over that.
        self.assertEqual(key_old, key_new)
        self.assertEqual(dict(old["fields"])["paging node"], "0")
        self.assertEqual(dict(old["fields"])["flip"], "0x2")
        self.assertEqual(dict(new["fields"])["flip"], "0x12")
        self.assertEqual(dict(new["fields"])["rot"], "1")
        self.assertNotIn("paging node", dict(new["fields"]))

    def test_addresses_drop_out_and_sizes_stay(self):
        _, key, _ = rc.classify("wddm: segment 1 flags 0x00080404 gpu 0xF4008CA000 cpu 0x2708CA000 "
                                "size 0x1FD736000")
        self.assertEqual(key, "wddm: segment 1 flags 0x00080404 gpu <addr> cpu <addr> size 0x1FD736000")

        _, key, _ = rc.classify("mmio: BAR5 at 0xFE800000 mapped, writes off")
        self.assertEqual(key, "mmio: BAR5 at <addr> mapped, writes off")

        _, key, _ = rc.classify("vram: 0x270000000 + 0x200000000, MC 0xF400000000, "
                                "BAR0 0xC0000000 + 0x10000000, writes off")
        self.assertEqual(key, "vram: <addr> + 0x200000000, MC <addr>, BAR0 <addr> + 0x10000000, writes off")

    def test_summary_is_not_part_of_the_sequence(self):
        kind, key, _ = rc.classify("wddm summary: QueryAdapterInfo                    1  first at 17")
        self.assertEqual(kind, "summary")
        self.assertEqual(key, "wddm summary: QueryAdapterInfo 1 first at <seq>")
        self.assertNotIn("summary", rc.SEQUENCE_KINDS)

    def test_tdr_needs_the_three_stars(self):
        kind, _, _ = rc.classify("wddm summary: no TDR (ResetFromTimeout and RestartFromTimeout "
                                 "were never called)")
        self.assertEqual(kind, "summary")
        kind, _, _ = rc.classify("wddm: *** ResetFromTimeout: the scheduler timed this adapter out "
                                 "(nothing was running) ***")
        self.assertEqual(kind, "tdr")

    def test_create_context_answer_is_not_a_second_call(self):
        kind, _, _ = rc.classify("wddm: CreateContext node 0 engine 0x1 flags 0x00000005 private 0")
        self.assertEqual(kind, "ddi")
        kind, _, _ = rc.classify("wddm: CreateContext answered dma 4096 bytes segment set 0x0, "
                                 "lists 0/0, caps 0x00000001")
        self.assertEqual(kind, "ddi_answer")


class TestStateFile(unittest.TestCase):
    STATE = [
        "time 2026-09-21T21:09:39  phase gate x",
        "device    BC-250 GPU (amdgpu-wddm)   status Error   "
        "problem CM_PROB_FAILED_POST_START",
        "driver    0.7.3.1   (oem19.inf)",
        "umd       UserModeDriverName bc250umd.dll | bc250umd.dll | bc250umd.dll",
        "stages    last 79   history 10 20 30 31 32 33 34 35 39 70 79    unconfirmed 1",
        "keeplog   KeepLog 1   KeepStatus 0x00000000   files 4",
        "gates     EnableFullWddm 1   EnableMmio 1   EnableVram 1   EnableGart 0   EnablePsp 0   "
        "EnableGfx 0   EnableIh 0",
        "events    Display 4101 (TDR)      0",
        "reports   live kernel reports written since boot: 0",
        "version   0x00070003 (milestone 7 revision 3)",
        "presents  35",
    ]

    def test_fields(self):
        state = rc.StateFile("gate-x-210939.txt", self.STATE)
        self.assertEqual(state.phase, "gate x")
        self.assertEqual(state.status, "Error")
        self.assertEqual(state.problem, "CM_PROB_FAILED_POST_START")
        self.assertEqual(state.driver, "0.7.3.1")
        self.assertEqual(state.inf, "oem19.inf")
        self.assertEqual(state.gates["EnableFullWddm"], 1)
        self.assertEqual(state.gates["EnableIh"], 0)
        self.assertEqual(state.last_stage, 79)
        self.assertEqual(state.stage_history[-1], 79)
        self.assertEqual(state.unconfirmed, 1)
        self.assertEqual(state.events, [("Display 4101 (TDR)", 0)])
        self.assertEqual(state.escape_version, 0x00070003)
        self.assertEqual(state.presents, 35)


class TestNormalize(unittest.TestCase):
    def test_timestamps_and_sequence_numbers_go(self):
        lines = rc.normalize_log_lines(MINIMAL)
        self.assertEqual(lines[0],
                         "bc250kmd 0x00070003 log kept at the stop: <n> lines, 0 lost to the wrap, "
                         "0 dropped above DISPATCH_LEVEL")
        self.assertEqual(lines[1], "stage 10")
        self.assertEqual(lines[6], "wddm: QueryAdapterInfo type 1 in 0 out 576 -> 0x00000000")
        for line in lines:
            self.assertNotRegex(line, r"^\s*\d+\s+\d+\.\d+")

    def test_same_run_twice_normalizes_the_same(self):
        later = [MINIMAL[0].replace("8 lines", "8 lines")] + [
            line.replace("0.000", "1.500").replace("0.100", "1.600")
            for line in MINIMAL[1:]]
        self.assertEqual(rc.normalize_log_lines(MINIMAL), rc.normalize_log_lines(later))


class TestObjectsSummary(unittest.TestCase):
    """One line up to KMD 0.7.207, two from 0.7.208 on (BD-070): both read the same."""

    ONE = MINIMAL[:7] + [
        "    14      0.300 wddm summary: objects created/destroyed: dev 2/2 ctx 1/1 proc 1/1 "
        "alloc 7/6, 3 alive",
    ]
    # The driver writes the allocation pair first and the line that ends in "alive" last, so one
    # stop summary is one block, and a reader which keeps the last line of this name still gets
    # the live count.
    TWO = MINIMAL[:7] + [
        "    14      0.300 wddm summary: objects created/destroyed: alloc 7/6",
        "    15      0.300 wddm summary: objects created/destroyed: dev 2/2 ctx 1/1 proc 1/1, "
        "3 alive",
    ]
    # Two starts and two stops in one ring. Only the first block may reach the caller.
    TWICE = TWO + [
        "    16      0.400 wddm summary: objects created/destroyed: alloc 90/80",
        "    17      0.400 wddm summary: objects created/destroyed: dev 9/8 ctx 7/6 proc 5/4, "
        "11 alive",
    ]
    # A 0.7.207 line that the 159-character log line cut short: the pairs are there, the live
    # count is gone. BD-070 is exactly this shape.
    TRUNCATED = MINIMAL[:7] + [
        "    14      0.300 wddm summary: objects created/destroyed: dev 2/2 ctx 1/1 proc 1/1 "
        "alloc 7/6",
    ]

    def test_one_line_and_two_lines_agree(self):
        for lines in (self.ONE, self.TWO):
            pairs, alive = run_from(lines).objects_summary
            self.assertEqual(pairs, {"dev": (2, 2), "ctx": (1, 1), "proc": (1, 1),
                                     "alloc": (7, 6)})
            self.assertEqual(alive, 3)

    def test_a_second_stop_summary_does_not_reach_the_first_row(self):
        pairs, alive = run_from(self.TWICE).objects_summary
        self.assertEqual(pairs, {"dev": (2, 2), "ctx": (1, 1), "proc": (1, 1), "alloc": (7, 6)})
        self.assertEqual(alive, 3)

    def test_a_truncated_line_gives_the_pairs_and_no_live_count(self):
        pairs, alive = run_from(self.TRUNCATED).objects_summary
        self.assertEqual(pairs, {"dev": (2, 2), "ctx": (1, 1), "proc": (1, 1), "alloc": (7, 6)})
        self.assertIsNone(alive)

    def test_a_missing_live_count_is_reported_and_does_not_raise(self):
        report = rc.show_report(run_from(self.TRUNCATED))
        rows = dict(item for _, body in report for item in body
                    if isinstance(item, tuple) and len(item) == 2)
        self.assertIn("the live count is not in the log", rows["objects"])

    def test_no_summary_at_all(self):
        self.assertEqual(run_from(MINIMAL[:7]).objects_summary, (None, None))


class TestDiffSynthetic(unittest.TestCase):
    def test_identical_runs(self):
        diff = rc.Difference(run_from(MINIMAL), run_from(MINIMAL))
        self.assertTrue(diff.identical)
        self.assertIn("No difference", diff.sentence())

    def test_one_run_goes_further(self):
        longer = MINIMAL[:7] + [
            "     6      0.250 wddm: QueryAdapterInfo type 13 in 0 out 32 -> 0x00000000",
            "     7      0.300 stage 70",
            "     8      0.300 stage 79",
        ]
        diff = rc.Difference(run_from(MINIMAL), run_from(longer))
        self.assertEqual(
            diff.sentence(),
            "First difference: after QueryAdapterInfo type 1 (DRIVERCAPS, answered) run B went on "
            "to QueryAdapterInfo type 13 (GPUMMUCAPS) -> STATUS_SUCCESS; run A stopped (stage 70).")

    def test_a_changed_answer_is_found(self):
        refused = [line.replace("type 1 in 0 out 576 -> 0x00000000",
                                "type 1 in 0 out 576 -> 0xC00000BB") for line in MINIMAL]
        diff = rc.Difference(run_from(MINIMAL), run_from(refused))
        self.assertIn("DRIVERCAPS (576 bytes)", diff.sentence())
        self.assertIn("STATUS_NOT_SUPPORTED", diff.sentence())

    def test_caps_wording_alone_is_a_fact_not_a_sequence_difference(self):
        reworded = [line.replace(
            "wddm: DRIVERCAPS 576 of 576 bytes: wddm 8192 sched 0x45 mm 0x60 flip 0x2 slots 0",
            "wddm: DRIVERCAPS into 576 bytes: wddm 8192 sched 0x45 mm 0x60 flip 0x12 slots 0 tdr 1")
            for line in MINIMAL]
        diff = rc.Difference(run_from(MINIMAL), run_from(reworded))
        self.assertTrue(diff.identical, "the sequences must still line up")
        facts = {row[0]: (row[1], row[2]) for row in diff.facts()}
        self.assertEqual(facts["DRIVERCAPS flip"], ("0x2", "0x12"))
        self.assertIn("DRIVERCAPS tdr", facts)
        self.assertIn("DRIVERCAPS bytes", diff.same_facts)


class TestBitDecoding(unittest.TestCase):
    """The bit names come from d3dkmddi.h through gen_tables, never from memory."""

    def test_create_context_flags(self):
        self.assertEqual(rc.decode_bits(0x5, rc.CREATECONTEXT_FLAGS),
                         "SystemContext | VirtualAddressing")
        self.assertEqual(rc.decode_bits(0x2, rc.CREATECONTEXT_FLAGS), "GdiContext")
        self.assertEqual(rc.decode_bits(0x0, rc.CREATECONTEXT_FLAGS), "none set")

    def test_context_info_caps(self):
        self.assertEqual(rc.decode_bits(0x1, rc.CONTEXTINFO_CAPS), "NoPatchingRequired")
        self.assertEqual(rc.decode_bits(0x7, rc.CONTEXTINFO_CAPS),
                         "NoPatchingRequired | DriverManagesResidency | UseIoMmu")

    def test_device_and_process_flags(self):
        self.assertEqual(rc.decode_bits(0x1, rc.CREATEDEVICE_FLAGS), "SystemDevice")
        self.assertEqual(rc.decode_bits(0x1, rc.CREATEPROCESS_FLAGS), "SystemProcess")
        # DXGK_DEVICE_RESERVED0 sits at bit 31 behind a 29-bit Reserved run.
        self.assertEqual(rc.CREATEDEVICE_FLAGS[31], "DXGK_DEVICE_RESERVED0")

    def test_an_unnamed_bit_is_reported_not_dropped(self):
        self.assertEqual(rc.decode_bits(0x80, rc.CONTEXTINFO_CAPS), "bit 7")
        self.assertEqual(rc.decode_bits(0x81, rc.CONTEXTINFO_CAPS), "NoPatchingRequired | bit 7")


class TestLifecycleSynthetic(unittest.TestCase):
    OBJECTS = MINIMAL[:7] + [
        "     6      0.200 wddm: CreateProcess flags 0x00000001 pasids 1",
        "     7      0.202 wddm: CreateDevice flags 0x00000001 pasid 0",
        "     8      0.202 wddm: CreateContext node 0 engine 0x1 flags 0x00000005 private 0",
        "     9      0.202 wddm: CreateContext answered dma 4096 bytes segment set 0x0, "
        "lists 0/0, caps 0x00000001",
        "    10      0.210 wddm: DestroyContext",
        "    11      0.210 wddm: DestroyDevice",
        "    12      0.212 wddm: DestroyProcess",
        "    13      0.300 stage 70",
        "    14      0.300 wddm summary: objects created/destroyed: dev 1/1 ctx 1/1 proc 1/1 "
        "alloc 0/0, 0 alive",
        "    15      0.300 stage 79",
    ]

    def life(self, lines=None):
        return run_from(lines or self.OBJECTS).lifecycle

    def test_counts_and_symmetry(self):
        life = self.life()
        self.assertEqual(life.counts["process"], (1, 1))
        self.assertEqual(life.counts["device"], (1, 1))
        self.assertEqual(life.counts["context"], (1, 1))
        self.assertEqual(life.counts["allocation"], (0, 0))
        self.assertTrue(life.symmetric)
        self.assertEqual(life.alive, 0)
        self.assertTrue(life.teardown_is_lifo)

    def test_span_is_first_create_to_last_destroy(self):
        first, last, span = self.life().span
        self.assertEqual(first.data["ddi"], "CreateProcess")
        self.assertEqual(last.data["ddi"], "DestroyProcess")
        self.assertAlmostEqual(span, 0.012, places=3)

    def test_asymmetry_is_caught(self):
        lines = [line for line in self.OBJECTS if "DestroyDevice" not in line]
        life = self.life(lines)
        self.assertEqual(life.counts["device"], (1, 0))
        self.assertFalse(life.symmetric)

    def test_create_context_flags_are_decoded(self):
        text = self.life().describe_create(self.life().context_create)
        self.assertIn("flags 0x00000005 (SystemContext | VirtualAddressing)", text)
        self.assertIn("engine affinity 0x1", text)
        self.assertIn("node 0", text)

    def test_every_answer_field_is_decoded(self):
        fields = dict(self.life().answer_fields)
        self.assertEqual(fields["DmaBufferSize"], "4096 bytes")
        self.assertEqual(fields["AllocationListSize"], "0")
        self.assertEqual(fields["PatchLocationListSize"], "0")
        self.assertIn("no segment", fields["DmaBufferSegmentSet"])
        self.assertEqual(fields["Caps"], "0x00000001 (NoPatchingRequired)")

    def test_segment_set_names_the_segment_ids(self):
        lines = [line.replace("segment set 0x0,", "segment set 0x5,") for line in self.OBJECTS]
        self.assertIn("(segment 1, 3)", dict(self.life(lines).answer_fields)["DmaBufferSegmentSet"])

    def test_next_expected_ddi(self):
        life = self.life()
        self.assertEqual(life.next_expected[0], "GetRootPageTableSize")
        # A run that got as far as the root page table is asked the next one instead.
        lines = self.OBJECTS[:10] + [
            "    10      0.204 wddm: GetRootPageTableSize asked 512, answered 512 entries",
            "    11      0.206 wddm: SetRootPageTable segment 1 offset 0x1000, 512 entries",
        ] + self.OBJECTS[10:]
        self.assertEqual(self.life(lines).next_expected[0], "CreateAllocation")

    def test_a_run_with_no_object_says_so(self):
        life = self.life(MINIMAL)
        self.assertFalse(life)
        self.assertIsNone(life.symmetric)
        text = "\n".join(rc.lifecycle_lines(run_from(MINIMAL)))
        self.assertIn("no object was created", text)
        self.assertIn("GetRootPageTableSize", text)
        self.assertIn("observed on no run yet", text)


@unittest.skipUnless(HAVE_EVIDENCE, "evidence/windows/2026-09-21-E16-run-00{2,3,4} not present")
class TestRealRuns(unittest.TestCase):
    """The lab's own files, read only."""

    def test_the_full_table_ring_is_the_one_taken(self):
        run = rc.load_run(RUN_002)
        self.assertEqual(run.ring.path.name, "ring-20260921-184902-205.log")
        self.assertEqual(run.ring.gate("EnableFullWddm"), 1)
        self.assertEqual(len(run.ring_choices), 2, "both rings of the run must be offered")

    def test_ring_override(self):
        other = RUN_002 / "ring-20260921-184857-976.log"
        run = rc.load_run(RUN_002, ring_path=other)
        self.assertEqual(run.ring.gate("EnableFullWddm"), 0)
        self.assertEqual(run.last_stage, 79)

    def test_run_002(self):
        run = rc.load_run(RUN_002)
        self.assertEqual(run.driver_version, 0x00070003)
        self.assertEqual(run.package, "0.7.3.0 (oem18.inf)")
        self.assertIn("absent", run.umd)
        self.assertEqual(run.problem, "CM_PROB_FAILED_POST_START")
        self.assertEqual([e.data["type"] for e in run.qai_calls], [1])
        self.assertEqual(run.caps.data["bytes"], 576)
        self.assertIsNone(run.first_refusal, "every answer run 002 gave succeeded")
        self.assertEqual(run.last_stage, 79)
        self.assertAlmostEqual(run.stop_after_start, 0.002, places=3)
        self.assertEqual(run.tdr_entries, [])
        self.assertEqual(run.objects_summary[1], 0)

    def test_run_003(self):
        run = rc.load_run(RUN_003)
        self.assertEqual(run.package, "0.7.3.1 (oem19.inf)")
        self.assertIn("bc250umd.dll", run.umd)
        self.assertEqual([e.data["type"] for e in run.qai_calls], [1, 15, 47])
        self.assertEqual(run.first_refusal.data["type"], 15)
        self.assertEqual(run.gates["EnableFullWddm"], 1)
        self.assertEqual(run.gates["EnableGfx"], 0)
        self.assertEqual(run.problem, "CM_PROB_FAILED_POST_START")

    def test_state_file_of_the_failed_start_is_the_one_paired(self):
        """Not the last file written: the run directory ends with the gates closed again."""
        run = rc.load_run(RUN_003)
        self.assertEqual(run.state.path.name, "gate-x-210939.txt")
        self.assertEqual(run.state.status, "Error")

    def test_first_difference_002_to_003(self):
        diff = rc.Difference(rc.load_run(RUN_002), rc.load_run(RUN_003))
        self.assertEqual(
            diff.sentence(),
            "First difference: after QueryAdapterInfo type 1 (DRIVERCAPS, answered) run B went on "
            "to QueryAdapterInfo type 15 (PHYSICALADAPTERCAPS) -> STATUS_NOT_SUPPORTED; "
            "run A stopped (stage 70).")
        facts = {row[0]: (row[1], row[2]) for row in diff.facts()}
        self.assertEqual(facts["package (DriverVer)"], ("0.7.3.0 (oem18.inf)", "0.7.3.1 (oem19.inf)"))
        self.assertIn("UserModeDriverName", facts)
        # The one thing runs 002 and 003 were meant to share.
        self.assertIn("driver version", diff.same_facts)
        for gate in ("EnableFullWddm", "EnableMmio", "EnableVram"):
            self.assertIn("gate " + gate, diff.same_facts)

    def test_first_difference_003_to_004(self):
        diff = rc.Difference(rc.load_run(RUN_003), rc.load_run(RUN_004))
        self.assertEqual(
            diff.sentence(),
            "First difference: after QueryAdapterInfo type 47 (64BITONLYCAPS, refused) run B went "
            "on to QueryAdapterInfo type 13 (GPUMMUCAPS) -> STATUS_SUCCESS; "
            "run A stopped (stage 70).")
        facts = {row[0]: (row[1], row[2]) for row in diff.facts()}
        self.assertEqual(facts["QueryAdapterInfo types"], ("1 15 47", "1 15 47 13 14 14 14 14 16"))
        self.assertEqual(facts["objects created/destroyed"][1], "dev 2/2, ctx 1/1, proc 1/1, alloc 0/0")
        self.assertEqual(facts["DRIVERCAPS flip"], ("0x2", "0x12"))
        self.assertEqual(facts["package (DriverVer)"], ("0.7.3.1 (oem19.inf)", "0.7.4.1 (oem20.inf)"))
        self.assertIn("DRIVERCAPS bytes", diff.same_facts)
        self.assertIn("TDR", diff.same_facts)
        # Both runs stop before the next stage, so this one fact has to match.
        self.assertIn("first DDI of the next stage not called", diff.same_facts)

    def test_run_004_query_adapter_info(self):
        """Types 10 and 11 are counted at the stop and never logged: they succeeded."""
        run = rc.load_run(RUN_004)
        self.assertEqual([e.data["type"] for e in run.qai_calls],
                         [1, 15, 47, 13, 14, 14, 14, 14, 16])
        counted = run.summary_qai_types
        self.assertEqual(counted[11][0], 2)
        self.assertEqual(counted[10][0], 1)
        self.assertEqual(sum(count for count, _ in counted.values()), 12)
        text = rc.render_show(rc.show_report(run))
        self.assertIn("QUERYSEGMENT4                  2 call(s) counted at the stop, 0 in the log",
                      text)

    def test_lifecycle_of_run_004(self):
        """The one run that got an object graph at all, and kept it for 2 ms."""
        life = rc.load_run(RUN_004).lifecycle
        self.assertTrue(life)
        self.assertEqual(life.counts["process"], (1, 1))
        self.assertEqual(life.counts["device"], (2, 2))
        self.assertEqual(life.counts["context"], (1, 1))
        self.assertTrue(life.symmetric)
        self.assertTrue(life.teardown_is_lifo)
        self.assertAlmostEqual(life.span[2], 0.002, places=3)
        self.assertIn("SystemContext | VirtualAddressing",
                      life.describe_create(life.context_create))
        fields = dict(life.answer_fields)
        self.assertEqual(fields["DmaBufferSize"], "4096 bytes")
        self.assertEqual(fields["Caps"], "0x00000001 (NoPatchingRequired)")
        self.assertEqual(life.next_expected[0], "GetRootPageTableSize")

    def test_runs_002_and_003_built_no_object(self):
        for directory in (RUN_002, RUN_003):
            life = rc.load_run(directory).lifecycle
            self.assertFalse(life, directory.name)
            self.assertEqual(life.next_expected[0], "GetRootPageTableSize")

    def test_lifecycle_section_is_in_show_and_diff(self):
        text = rc.render_show(rc.show_report(rc.load_run(RUN_004)))
        self.assertIn("object lifecycle", text)
        self.assertIn("SystemProcess", text)
        self.assertIn("SystemDevice", text)
        self.assertIn("NoPatchingRequired", text)
        self.assertIn("LIFO", text)
        diff_text = rc.render_diff(rc.Difference(rc.load_run(RUN_003), rc.load_run(RUN_004)))
        self.assertIn("object lifecycle", diff_text)
        self.assertIn("next DDI not called", diff_text)

    def test_show_names_every_type_it_prints(self):
        text = rc.render_show(rc.show_report(rc.load_run(RUN_003)))
        self.assertIn("PHYSICALADAPTERCAPS", text)
        self.assertIn("STATUS_NOT_SUPPORTED", text)
        self.assertIn("CM_PROB_FAILED_POST_START (Code 43)", text)
        self.assertIn("stage 39 (StageStartDone)", text)

    def test_json_of_a_diff(self):
        payload = rc.Difference(rc.load_run(RUN_002), rc.load_run(RUN_003)).to_json()
        self.assertFalse(payload["identical"])
        self.assertIn("PHYSICALADAPTERCAPS", payload["b_next"])
        self.assertEqual(len(payload["sequence_a"]), 21)


class TestCommandLine(unittest.TestCase):
    """The three commands have to work as processes, not only as imports."""

    def run_tool(self, *args):
        result = subprocess.run([sys.executable, str(HERE / "runcompare.py")] + list(args),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    @unittest.skipUnless(HAVE_EVIDENCE, "evidence not present")
    def test_normalize_a_real_log(self):
        out = self.run_tool("normalize", str(RUN_004 / "ring-20260921-192217-705.log"))
        self.assertIn("wddm: DRIVERCAPS <576 bytes>", out)
        self.assertIn("wddm: segment 1 flags 0x00080404 gpu <addr> cpu <addr> size 0x1FD736000", out)
        for line in out.splitlines():
            self.assertNotRegex(line, r"^\s*\d+\s+\d+\.\d+")

    @unittest.skipUnless(HAVE_EVIDENCE, "evidence not present")
    def test_show_and_diff_as_json(self):
        payload = json.loads(self.run_tool("show", str(RUN_002), "--json"))
        self.assertEqual(payload["driver_version"], "0x00070003")
        self.assertEqual(payload["problem_code"], 43)
        payload = json.loads(self.run_tool("show", str(RUN_004), "--json"))
        self.assertEqual(payload["lifecycle"]["objects"]["device"],
                         {"created": 2, "destroyed": 2})
        self.assertTrue(payload["lifecycle"]["symmetric"])
        self.assertEqual(payload["lifecycle"]["next_expected_ddi"], "GetRootPageTableSize")
        self.assertIsNone(payload["lifecycle"]["expected_order_observed_on"])
        payload = json.loads(self.run_tool("diff", str(RUN_002), str(RUN_003), "--json"))
        self.assertIn("type 15", payload["first_difference"])
        self.assertIn("lifecycle_a", payload)

    @unittest.skipUnless(HAVE_EVIDENCE, "evidence not present")
    def test_markdown(self):
        out = self.run_tool("diff", str(RUN_002), str(RUN_003), "--markdown")
        self.assertIn("| fact |", out)
        self.assertIn("# runcompare", out)


class TestGeneratedTables(unittest.TestCase):
    KITS = REPO.parent / "toolchain" / "nuget"

    @unittest.skipUnless((REPO.parent / "toolchain" / "nuget").is_dir(),
                         "no NuGet Kits on this machine")
    def test_checked_in_table_matches_the_headers(self):
        result = subprocess.run(
            [sys.executable, str(HERE / "gen_tables.py"), "--check", "--kits", str(self.KITS)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
