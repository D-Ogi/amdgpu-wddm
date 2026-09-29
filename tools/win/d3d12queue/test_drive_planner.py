# SPDX-License-Identifier: MIT
import copy
import unittest

from drive_planner import DRIVE_PLAN, DriveRefusal, DriveState, drive_loop


def receipt(sequence, command, success=True, previous=None, **overrides):
    # Match Session's staged lifetime: cleanup begins after the terminal receipt.
    if previous is not None:
        state = previous['state'].copy()
        copied, pending = previous['copy_success'], previous['gpu_pending']
    else:
        state = dict(device=command != 'create-device' or success,
                     queue=command not in ('create-device', 'create-queue') or
                     (command == 'create-queue' and success))
        copied, pending = command in ('status', 'exit'), False
    if command == 'create-device': state['device'] = success
    elif command == 'create-queue': state['queue'] = success
    elif command == 'copy': copied = success
    value = dict(schema=1, sequence=sequence, command=command, success=success,
                 hr="00000000" if success else "80004005", elapsed_ms=sequence,
                 state=state, copy_success=copied, gpu_pending=pending)
    value.update(overrides)
    return value


def snapshot(planner, next_receipt=None, **overrides):
    result = dict(started=True, runtime_ready=True, stop=False, cancel=False, terminal=False,
                  commands=[dict(sequence=n, command=c) for n, c in planner.expected.items()],
                  receipts=list(planner.receipts.values()))
    if next_receipt is not None:
        result["receipts"].append(next_receipt)
    result.update(overrides)
    return result


class FakeClock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


class PlannerTests(unittest.TestCase):
    def advance(self, planner, through):
        for sequence in range(1, through + 1):
            action = planner.observe(snapshot(planner, receipt(sequence, DRIVE_PLAN[sequence-1])))
            if action:
                planner.issuing(*action)

    def test_positive_sequence(self):
        planner = DriveState()
        self.advance(planner, 5)
        self.assertTrue(planner.terminal)
        self.assertEqual(planner.issued, {2, 3, 4, 5})
        self.assertEqual(planner.expected, dict(enumerate(DRIVE_PLAN, 1)))
        self.assertEqual(planner.receipts[1]['state'], dict(device=True, queue=False))
        self.assertFalse(planner.receipts[1]['copy_success'])
        self.assertFalse(planner.receipts[2]['copy_success'])
        for sequence in (3, 4, 5):
            self.assertTrue(planner.receipts[sequence]['copy_success'])
            self.assertFalse(planner.receipts[sequence]['gpu_pending'])

    def test_failure_exits_at_next_sequence(self):
        for failed in (1, 2, 3, 4):
            planner = DriveState()
            self.advance(planner, failed-1)
            self.assertEqual(planner.observe(snapshot(planner, receipt(failed, DRIVE_PLAN[failed-1], False))),
                             (failed+1, "exit"))

    def test_pending_failed_copy_retained_through_cleanup(self):
        for ending in ('exit', 'abort'):
            planner = DriveState()
            self.advance(planner, 2)
            failed = receipt(3, 'copy', False, gpu_pending=True)
            self.assertEqual(planner.observe(snapshot(planner, failed)), (4, 'exit'))
            planner.issuing(4, ending)
            terminal = receipt(4, ending, ending == 'exit', previous=failed)
            self.assertIsNone(planner.observe(snapshot(planner, terminal)))
            self.assertTrue(planner.terminal)
            self.assertTrue(planner.receipts[4]['gpu_pending'])
            self.assertFalse(planner.receipts[4]['copy_success'])

    def test_missing_created_device_or_queue_exits(self):
        cases = [(1, dict(state=dict(device=False, queue=False))),
                 (2, dict(state=dict(device=True, queue=False)))]
        for sequence, overrides in cases:
            planner = DriveState()
            self.advance(planner, sequence-1)
            self.assertEqual(planner.observe(snapshot(planner, receipt(sequence, DRIVE_PLAN[sequence-1], **overrides))),
                             (sequence+1, "exit"))

    def test_contradictory_copy_cannot_advance_to_status(self):
        for overrides in (dict(state=dict(device=False, queue=False)),
                          dict(state=dict(device=False, queue=True)),
                          dict(state=dict(device=True, queue=False)),
                          dict(copy_success=False), dict(gpu_pending=True)):
            planner = DriveState()
            self.advance(planner, 2)
            with self.assertRaises(DriveRefusal):
                planner.observe(snapshot(planner, receipt(3, 'copy', **overrides)))
            with self.assertRaises(DriveRefusal):
                planner.issuing(4, 'status')

    def test_creation_cannot_report_copy_or_pending_or_early_queue(self):
        for sequence, command in ((1, 'create-device'), (2, 'create-queue')):
            for overrides in (dict(copy_success=True), dict(gpu_pending=True)):
                planner = DriveState()
                self.advance(planner, sequence-1)
                with self.assertRaises(DriveRefusal):
                    planner.observe(snapshot(planner, receipt(sequence, command, **overrides)))
        for state in (dict(device=True, queue=True), dict(device=False, queue=True)):
            with self.assertRaises(DriveRefusal):
                DriveState().observe(snapshot(DriveState(), receipt(1, 'create-device', state=state)))

    def test_status_and_terminal_preserve_successful_copy(self):
        for sequence, command in ((4, 'status'), (5, 'exit')):
            for overrides in (dict(copy_success=False), dict(gpu_pending=True),
                              dict(state=dict(device=False, queue=False)),
                              dict(state=dict(device=True, queue=False)), dict(elapsed_ms=0)):
                planner = DriveState()
                self.advance(planner, sequence-1)
                with self.assertRaises(DriveRefusal):
                    planner.observe(snapshot(planner, receipt(sequence, command, **overrides)))

    def test_failed_status_preserves_copy_witness_and_exits(self):
        planner = DriveState()
        self.advance(planner, 3)
        failed = receipt(4, 'status', False)
        self.assertTrue(failed['copy_success'])
        self.assertEqual(planner.observe(snapshot(planner, failed)), (5, 'exit'))
        planner.issuing(5, 'exit')
        self.assertIsNone(planner.observe(snapshot(planner, receipt(5, 'exit', previous=failed))))
        self.assertTrue(planner.terminal)

    def test_failure_cleanup_states_are_not_forced_to_success(self):
        for failed_stage in (1, 2, 3):
            for retained in (False, True):
                planner = DriveState()
                self.advance(planner, failed_stage-1)
                extra = ({'state': dict(device=retained, queue=False)} if failed_stage == 1 else
                         {'state': dict(device=True, queue=retained)} if failed_stage == 2 else
                         {'gpu_pending': retained})
                failed = receipt(failed_stage, DRIVE_PLAN[failed_stage-1], False, **extra)
                action = planner.observe(snapshot(planner, failed))
                self.assertEqual(action, (failed_stage+1, 'exit'))
                planner.issuing(*action)
                terminal = receipt(failed_stage+1, 'exit', previous=failed)
                self.assertIsNone(planner.observe(snapshot(planner, terminal)))
                self.assertTrue(planner.terminal)

    def test_failed_copy_cannot_invent_exact_compare_witness(self):
        planner = DriveState()
        self.advance(planner, 2)
        with self.assertRaises(DriveRefusal):
            planner.observe(snapshot(planner, receipt(3, 'copy', False, copy_success=True)))

    def test_strict_receipt_fields_and_poisoned_state(self):
        for override in (dict(success=1), dict(sequence=True), dict(schema=True), dict(hr="bad"),
                         dict(hr="80004005"), dict(gpu_pending=0), dict(copy_success="false"),
                         dict(state=dict(device=1, queue=True)), dict(elapsed_ms=-1), dict(elapsed_ms=180001)):
            planner = DriveState()
            with self.assertRaises(DriveRefusal):
                planner.observe(snapshot(planner, dict(receipt(1, "create-device"), **override)))
            with self.assertRaises(DriveRefusal):
                planner.observe(snapshot(DriveState(), receipt(1, "create-device")))

    def test_malformed_snapshot_bounds(self):
        for value in (None, [], dict(), snapshot(DriveState(), stop=1),
                      snapshot(DriveState(), commands=[{}]*7), snapshot(DriveState(), receipts=[{}]*6)):
            with self.assertRaises(DriveRefusal):
                DriveState().observe(value)

    def test_no_early_or_arbitrary_issue_and_no_duplicate(self):
        planner = DriveState()
        with self.assertRaises(DriveRefusal):
            planner.issuing(2, "create-queue")
        action = planner.observe(snapshot(planner, receipt(1, "create-device")))
        with self.assertRaises(DriveRefusal):
            planner.issuing(2, "copy")
        planner.issuing(*action)
        with self.assertRaises(DriveRefusal):
            planner.issuing(*action)
        self.assertIsNone(planner.observe(snapshot(planner)))

    def test_replayed_receipt_is_unchanged_and_detached(self):
        planner = DriveState()
        value = snapshot(planner, receipt(1, "create-device", payload="secret"))
        action = planner.observe(value)
        self.assertNotIn("payload", planner.receipts[1])
        self.assertEqual(planner.observe(copy.deepcopy(value)), action)
        value["receipts"][0]["state"]["device"] = False
        self.assertTrue(planner.receipts[1]["state"]["device"])
        with self.assertRaises(DriveRefusal):
            planner.observe(value)

    def test_missing_or_duplicate_immutable_records(self):
        for mode in ("missing", "duplicate", "foreign", "command_missing"):
            planner = DriveState()
            first = receipt(1, "create-device")
            planner.observe(snapshot(planner, first))
            value = snapshot(planner)
            if mode == "missing": value["receipts"] = []
            if mode == "duplicate": value["receipts"].append(first)
            if mode == "foreign": value["commands"].append(dict(sequence=2, command="copy"))
            if mode == "command_missing": value["commands"] = []
            with self.assertRaises(DriveRefusal):
                planner.observe(value)

    def test_stop_cancel_and_terminal_are_sticky(self):
        for flag in ("stop", "cancel"):
            planner = DriveState()
            self.assertEqual(planner.observe(snapshot(planner, receipt(1, "create-device"), **{flag: True})), (2, "abort"))
            self.assertEqual(planner.observe(snapshot(planner)), (2, "abort"))
        planner = DriveState()
        self.assertIsNone(planner.observe(snapshot(planner, terminal=True)))
        self.assertIsNone(planner.observe(snapshot(planner, receipt(1, "create-device"))))
        with self.assertRaises(DriveRefusal): planner.issuing(2, "create-queue")

    def test_loop_startup_then_positive_sequence(self):
        clock, planner = FakeClock(), DriveState()
        issued, events = [], []
        def poll(timeout):
            self.assertGreater(timeout, 0)
            if clock.now < 1:
                return snapshot(planner, runtime_ready=False, commands=[])
            sequence = max(planner.expected)
            return snapshot(planner, receipt(sequence, planner.expected[sequence]))
        def issue(seq, verb, timeout):
            action = planner.observe(snapshot(planner, receipt(seq-1, planner.expected[seq-1])))
            self.assertEqual(action, (seq, verb))
            planner.issuing(seq, verb)
            issued.append((seq, verb))
        result = drive_loop(poll, issue, events.append, 10, clock, clock.sleep)
        self.assertEqual(result, "interactive_terminal_observed")
        self.assertEqual(issued, list(enumerate(DRIVE_PLAN[1:], 2)))
        self.assertLessEqual(clock.now, 10)

    def test_deadline_without_receipt_never_guesses_abort(self):
        clock, issued = FakeClock(), []
        result = drive_loop(lambda timeout: snapshot(DriveState()), lambda *a: issued.append(a),
                            lambda e: None, 3, clock, clock.sleep)
        self.assertEqual(result, "drive_deadline")
        self.assertFalse(issued)
        self.assertLessEqual(clock.now, 3)

    def test_deadline_or_stop_after_receipt_only_aborts(self):
        for stopped in (False, True):
            clock, issued = FakeClock(), []
            result = drive_loop(lambda timeout: snapshot(DriveState(), receipt(1, "create-device"), stop=stopped),
                                lambda *a: issued.append(a), lambda e: None, 1, clock, clock.sleep)
            self.assertEqual(issued[0][:2], (2, "abort"))
            self.assertEqual(len(issued), 1)
            self.assertEqual(result, "stop_observed" if stopped else "drive_deadline")

    def test_poll_timeout_consumes_budget_without_issue(self):
        clock, issued = FakeClock(), []
        def poll(timeout):
            clock.now += timeout
            return snapshot(DriveState(), receipt(1, "create-device"))
        self.assertEqual(drive_loop(poll, lambda *a: issued.append(a), lambda e: None, 1, clock, clock.sleep), "drive_deadline")
        self.assertFalse(issued)

    def test_uncertain_delivery_not_retried_or_printed(self):
        clock, calls, events, diagnostics = FakeClock(), [], [], []
        def issue(*args):
            calls.append(args)
            raise TimeoutError("secret remote stderr")
        result = drive_loop(lambda timeout: snapshot(DriveState(), receipt(1, "create-device")),
                            issue, events.append, 60, clock, clock.sleep, diagnostics.append)
        self.assertEqual(result, "transport_or_capture_failed")
        self.assertEqual(len(calls), 1)
        self.assertEqual(diagnostics, [dict(stage="issue", exception_type="TimeoutError", category="callback_failure")])
        self.assertNotIn("secret", str(events)+str(diagnostics))

    def test_invalid_budget_rejects_before_poll(self):
        for seconds in (True, 0, 66, float("nan"), float("inf"), "60"):
            with self.assertRaises(ValueError):
                drive_loop(lambda t: self.fail("unexpected poll"), None, None, seconds, FakeClock(), None)


if __name__ == "__main__":
    unittest.main()
