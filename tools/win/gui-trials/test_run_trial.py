"""Offline host-runner contract tests. No target, credentials, UI or network."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
# Temporary trial outputs stay outside this repository (BC250_TEST_OUT, as the quality gate sets it).
TEMP = Path(os.environ.get('BC250_TEST_OUT') or tempfile.gettempdir())
spec = importlib.util.spec_from_file_location('gui_runner', HERE / 'run-trial.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


def config():
    return {'schema': 1, 'trial_id': 'fixture-001', 'trial': 'L1', 'stage': 'view',
            'directory': 'C:\\BC250\\tmp\\gui\\fixture-001', 'duration_seconds': 45,
            'target_computer_name': 'SYNTHETIC-LAB', 'artifacts': [],
            'expected_session_id': 1, 'expected_user_sid': 'S-1-5-21-1-2-3-1001',
            'scenario_adapter': {'path': 'C:\\BC250\\fixture.ps1', 'sha256': '1' * 64},
            'bounded_helper': {'path': 'C:\\BC250\\helper.exe', 'sha256': '2' * 64}}


def event(name, **kwargs):
    return {'schema': 1, 'protocol': r.PROTOCOL, 'event': name,
            'trial_id': 'fixture-001', 'trial': 'L1', 'stage': 'view', **kwargs}


class Clock:
    def __init__(self): self.time = 0
    def now(self): return self.time
    def sleep(self, delta): self.time += delta


class FakeIO:
    def __init__(self, terminal=True, fail=None):
        self.calls, self.terminal, self.fail = [], terminal, fail
        self.stop_requested = False
    def stage(self): self.calls.append('stage')
    def overlay(self, *args): self.calls.append('overlay')
    def mode(self, name):
        self.calls.append(name)
        if self.fail == name: raise RuntimeError('simulated transport loss')
        if name == 'Stop': self.stop_requested = True
        if name == 'Cleanup': return [event('cleanup', status='cleaned')]
        if name == 'Start': return [event('running')]
        if name == 'Observe':
            return [event('state', status='evidence-ready' if self.terminal else 'running',
                          can_cleanup=self.terminal or self.stop_requested)]
        return [event(name.lower())]
    def screenshot(self, path):
        self.calls.append('screenshot')
        return {'file': path.name, 'scale': 0.5, 'sha256': '3' * 64}
    def watts(self):
        self.calls.append('watts')
        return {'power_w': 50, 'freshness': 'unknown'}


class ContractTests(unittest.TestCase):
    def run_trial(self, fake):
        clock = Clock()
        with tempfile.TemporaryDirectory(dir=TEMP) as td:
            run = r.Trial(config(), Path(td), fake, clock.now, clock.sleep)
            return run.execute(), clock.time

    def test_invalid_duration_cannot_contact_lab(self):
        for bad in (True, 0, 44, 180.0, 181, float('inf'), float('nan'), '180'):
            with self.subTest(bad=bad):
                c = config(); c['duration_seconds'] = bad
                with self.assertRaises(ValueError): r.validate_config(c)

    def test_path_cannot_escape_trial_root(self):
        for key, val in [('trial_id', '../other'), ('directory', 'C:\\Windows'),
                         ('expected_session_id', 0)]:
            c = config(); c[key] = val
            with self.assertRaises(ValueError): r.validate_config(c)

    def test_check_only_does_not_construct_transport(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as td:
            file = Path(td) / 'config.json'; file.write_text(json.dumps(config()))
            with patch.object(r, 'LabIO', side_effect=AssertionError('network forbidden')):
                self.assertEqual(r.main(['--config', str(file)]), 0)

    def test_wrong_identity_event_is_not_evidence(self):
        e = event('running'); e['trial_id'] = 'other'
        with self.assertRaises(ValueError): r.events(json.dumps(e), config())

    def test_success_still_requires_operator_visual_review(self):
        io = FakeIO(); result, _ = self.run_trial(io)
        self.assertEqual(result['status'], 'evidence-ready')
        self.assertFalse(result['acceptance_pass'])
        self.assertIn('Cleanup', io.calls)
        self.assertNotIn('Stop', io.calls)
        self.assertLess(io.calls.index('Start'), io.calls.index('screenshot'))

    def test_start_ack_loss_still_attempts_stop_and_cleanup(self):
        io = FakeIO(fail='Start'); result, _ = self.run_trial(io)
        self.assertFalse(result['started'])
        self.assertNotIn('screenshot', io.calls)
        self.assertLess(io.calls.index('Stop'), io.calls.index('Cleanup'))

    def test_host_deadline_requests_stop_then_cleanup(self):
        io = FakeIO(terminal=False); result, elapsed = self.run_trial(io)
        self.assertEqual(elapsed, 45)
        self.assertEqual(result['status'], 'stopped')
        self.assertLess(io.calls.index('Stop'), io.calls.index('Cleanup'))

    def test_lost_observe_does_not_claim_closure(self):
        io = FakeIO(fail='Observe'); result, _ = self.run_trial(io)
        self.assertEqual(result['status'], 'recovery-required')
        self.assertIn('Stop', io.calls)
        self.assertNotIn('Cleanup', io.calls)

    def test_failed_cleanup_requires_recovery(self):
        io = FakeIO(fail='Cleanup'); result, _ = self.run_trial(io)
        self.assertEqual(result['status'], 'recovery-required')
        self.assertFalse(result['acceptance_pass'])

    def test_missing_running_proof_prevents_capture(self):
        io = FakeIO(); original = io.mode
        io.mode = lambda mode: [event('starting')] if mode == 'Start' else original(mode)
        result, _ = self.run_trial(io)
        self.assertFalse(result['started'])
        self.assertNotIn('screenshot', io.calls)
        self.assertIn('Stop', io.calls)

    def test_lab_io_uses_only_half_scale_endpoint(self):
        io = object.__new__(r.LabIO)
        class Mon:
            def image(self, path):
                self.path = path
                return '100x100', b'\x89PNG\r\n\x1a\nfixture'
        io.mon = Mon()
        with tempfile.TemporaryDirectory(dir=TEMP) as td:
            item = io.screenshot(Path(td) / 'image.png')
        self.assertIn('scale=0.5', io.mon.path)
        self.assertEqual(item['scale'], 0.5)

    def test_cleanup_waits_for_task_exit_after_result(self):
        io = FakeIO(); original = io.mode; observed = [0]
        def mode(name):
            if name == 'Observe':
                observed[0] += 1
                io.calls.append(name)
                return [event('state', status='evidence-ready', can_cleanup=observed[0] >= 3)]
            if name == 'Cleanup': self.assertGreaterEqual(observed[0], 3)
            return original(name)
        io.mode = mode
        result, elapsed = self.run_trial(io)
        self.assertEqual(result['status'], 'evidence-ready')
        self.assertEqual(elapsed, 5)

    def test_stage_capture_is_deduplicated(self):
        io = FakeIO(); original = io.mode; observed = [0]
        def mode(name):
            if name == 'Observe':
                observed[0] += 1
                return [event('stage', sequence=1, data={'name': 'offline-ready'}),
                        event('state', status='evidence-ready' if observed[0] > 1 else 'running',
                              can_cleanup=observed[0] > 1)]
            return original(name)
        io.mode = mode
        result, _ = self.run_trial(io)
        self.assertEqual([p['reason'] for p in result['screenshots']],
                         ['running', 'offline-ready', 'terminal'])

    def test_existing_remote_directory_is_not_overwritten(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as td:
            root = Path(td)
            for name in r.SUPPORT + ['L1.ps1', 'config.json']: (root / name).write_text('fixture')
            io = object.__new__(r.LabIO); io.config = config(); io.output = root
            commands = []
            def reserve(command):
                commands.append(command)
                raise RuntimeError('already exists')
            class Target:
                def push(self, *a, **k): raise AssertionError('must not overwrite')
            io.target = Target(); io.run = reserve
            with patch.object(r, 'HERE', root), self.assertRaises(RuntimeError): io.stage()
            self.assertIn('New-Item', commands[0])
            self.assertNotIn('-Force', commands[0])

    def test_replayed_events_deduplicate(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as td:
            trial = r.Trial(config(), Path(td), FakeIO())
            e = event('heartbeat', sequence=1)
            trial.keep([e, e]); trial.keep([e])
            self.assertEqual(len((Path(td) / 'host.jsonl').read_text().splitlines()), 1)


if __name__ == '__main__': unittest.main()
