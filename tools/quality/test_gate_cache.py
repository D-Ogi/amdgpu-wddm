"""Cache controls use inert fixtures and child processes, never a lab or driver."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import gate_cache as gate


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gate-control-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        self.source = self.repo / 'source.h'
        self.source.write_bytes(b'A')
        self.script = self.repo / 'check.ps1'
        self.script.write_text("Write-Output 'fixture PASS'", encoding='utf-8')
        quality = self.repo / 'tools/quality'
        quality.mkdir(parents=True)
        shutil.copyfile(Path(gate.__file__).with_name('gate-worker.ps1'), quality / 'gate-worker.ps1')
        self.tool = self.base / 'compiler.bin'
        self.tool.write_bytes(b'tool1')
        self.out = self.base / 'out'
        self.out.mkdir()
        self.plan_path = self.base / 'plan.json'
        self.plan = {'variables': {'Out': str(self.out), 'repo': str(self.repo),
                                  'Workspace': str(self.base), 'Kits': str(self.base), 'Profile': 'KmdPackage'},
                     'compiler': str(self.base), 'context': {},
                     'jobs': [{'name': 'sample', 'action': '& "$repo/check.ps1"'}]}
        self.manifest = {'timeout': 10, 'release_only': [],
                         'toolchain': [{'root': 'compiler', 'globs': ['compiler.bin']}],
                         'groups': {'sample': [{'root': 'repo', 'globs': ['*.h', '*.ps1']}]},
                         'checks': {'sample': {'inputs': ['sample']}}}
        self.env = patch.dict(os.environ, {'BC250_GATE_CACHE': str(self.base / 'cache'),
                                           'BC250_GATE_NOCACHE': '0'})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.calls = []

    def fake_process(self, command, cwd, env, log, timeout):
        self.calls.append((command, env))
        log.write_bytes(b'fixture PASS\n')
        return 0, False

    def run_gate(self, no_cache=False, real=False):
        self.plan_path.write_text(json.dumps(self.plan), encoding='utf-8')
        with contextlib.redirect_stdout(io.StringIO()):
            with (contextlib.nullcontext() if real else patch.object(gate, 'run_process', self.fake_process)):
                return gate.execute(self.plan, self.manifest, self.plan_path, no_cache, jobs=2)

    def test_content_script_glob_tool_and_environment_invalidate(self):
        self.assertEqual(self.run_gate()['status'], 'PASS')
        self.assertEqual(self.run_gate()['cache_hits'], 1)
        old = self.source.stat()
        self.source.write_bytes(b'B')
        os.utime(self.source, ns=(old.st_atime_ns, old.st_mtime_ns))
        self.assertEqual(self.run_gate()['cache_hits'], 0, 'same mtime and length must not hide a byte change')
        self.script.write_text("Write-Output 'changed'", encoding='utf-8')
        self.assertEqual(self.run_gate()['cache_hits'], 0)
        extra = self.repo / 'new.h'
        extra.write_bytes(b'header')
        self.assertEqual(self.run_gate()['cache_hits'], 0)
        extra.unlink()
        self.tool.write_bytes(b'tool2')
        self.assertEqual(self.run_gate()['cache_hits'], 0)
        with patch.dict(os.environ, {'BC250_FIXTURE_FLAG': '/O2'}):
            self.assertEqual(self.run_gate()['cache_hits'], 0)

    def test_no_cache_full_and_release_environment(self):
        self.run_gate()
        self.assertEqual(self.run_gate()['cache_hits'], 1)
        for force, profile, env_force in ((True, 'KmdPackage', '0'), (False, 'Full', '0'),
                                           (False, 'KmdPackage', '1')):
            self.plan['variables']['Profile'] = profile
            with patch.dict(os.environ, {'BC250_GATE_NOCACHE': env_force}):
                result = self.run_gate(force)
                self.assertEqual(result['cache_hits'], 0)
                self.assertTrue(result['cache_disabled'])
                self.assertEqual(self.calls[-1][1]['BC250_GATE_NOCACHE'], '1')

    def test_no_cache_does_not_write(self):
        self.run_gate(True)
        self.assertFalse((self.base / 'cache').exists())

    def test_missing_declaration_always_runs(self):
        self.manifest['checks'] = {}
        self.run_gate()
        result = self.run_gate()
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(result['uncacheable'], {'sample': 'undeclared inputs'})

    def test_fail_not_cached_and_dependents_do_not_run(self):
        self.plan['jobs'].append({'name': 'child', 'action': 'exit 0'})
        self.manifest['checks']['child'] = {'inputs': ['sample'], 'depends': ['sample']}
        def fail_process(command, cwd, env, log, timeout):
            self.calls.append(command)
            log.write_bytes(b'FAIL fixture')
            return 1, False
        with patch.object(self, 'fake_process', fail_process):
            self.assertEqual(self.run_gate()['status'], 'FAIL')
        self.assertFalse((self.base / 'cache').exists())
        self.assertEqual(len(self.calls), 1)

    def test_corrupt_artifact_never_supplies_pass(self):
        self.run_gate()
        log = next((self.base / 'cache').glob('*/files/sample.log'))
        log.write_bytes(b'corrupt')
        self.assertEqual(self.run_gate()['cache_hits'], 0)

    def test_receipt_cannot_escape_output_root(self):
        self.run_gate()
        receipt = next((self.base / 'cache').glob('*/pass.json'))
        data = json.loads(receipt.read_bytes())
        data['artifacts'][0]['path'] = '../outside.log'
        receipt.write_text(json.dumps(data))
        self.assertEqual(self.run_gate()['cache_hits'], 0)
        self.assertFalse((self.base / 'outside.log').exists())

    def test_inputs_moving_during_execution_are_not_published(self):
        def move(command, cwd, env, log, timeout):
            log.write_bytes(b'PASS')
            self.source.write_bytes(b'changed')
            return 0, False
        with patch.object(self, 'fake_process', move):
            self.assertEqual(self.run_gate()['status'], 'FAIL')
        self.assertFalse((self.base / 'cache').exists())

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows worker')
    def test_actual_worker_and_fresh_output_restore(self):
        self.assertEqual(self.run_gate(real=True)['status'], 'PASS')
        self.out = self.base / 'second-out'
        self.plan['variables']['Out'] = str(self.out)
        result = self.run_gate(real=True)
        self.assertEqual(result['cache_hits'], 1)
        self.assertIn('fixture PASS', (self.out / 'sample.log').read_text())
        self.script.write_text('exit 7', encoding='utf-8')
        result = self.run_gate(real=True)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['checks'][0]['exit_code'], 7)

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows worker')
    def test_runner_switches_and_values_preserve_binding(self):
        self.script.write_text('param([string]$Out,[string]$Text,[switch]$Flag)\n'
                               'if(!$Flag -or $Text -ne "--a b") {exit 9}\nWrite-Output "bound"')
        self.plan['jobs'] = [{'name': 'sample', 'runner': 'check.ps1',
                              'arguments': ['-Text', '--a b', '-Flag']}]
        self.assertEqual(self.run_gate(real=True)['status'], 'PASS')

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows worker')
    def test_runner_can_consume_expected_native_failure(self):
        self.script.write_text('param([string]$Out)\n& $env:ComSpec /c exit 1\n'
                               'if($LASTEXITCODE -ne 1){throw "control did not fail"}\n'
                               'Write-Output "expected native failure"')
        self.plan['jobs'] = [{'name': 'sample', 'runner': 'check.ps1', 'arguments': []}]
        self.assertEqual(self.run_gate(real=True)['status'], 'PASS')

    def test_negative_control_must_fail_inside_its_test(self):
        # BD-237: the oracle of a Fails job.
        good = b'build ok\ntest-start: t.exe\ncheck 1 ok\nFAIL check 2\ntest-end: t.exe exit 1\n'
        self.assertIsNone(gate.control_failure(1, good))
        crash = b'test-start: t.exe\nFAIL check 2\ntest-end: t.exe exit -1073741819\n'
        self.assertIn('exit code -1073741819', gate.control_failure(-1073741819, crash))
        self.assertIn('exit code 3221225477', gate.control_failure(3221225477, b'FAIL check 2\n'))
        self.assertIn('exit 1 expected', gate.control_failure(1, crash + b'later step failed\n'))
        source = b'FAIL source check: the anchor moved\n'
        self.assertIn('no test-end line', gate.control_failure(1, source))
        early = b'FAIL source check\ntest-start: t.exe\nall checks ok\ntest-end: t.exe exit 1\n'
        self.assertIn('no FAIL line inside', gate.control_failure(1, early))
        later = good + b'test-start: u.exe\nok\ntest-end: u.exe exit 0\n'
        self.assertIn('exit 1 expected', gate.control_failure(1, later))
        self.assertIn('no "test-start: t.exe"', gate.control_failure(1, b'FAIL\ntest-end: t.exe exit 1\n'))
        spaced = b'test-start: mailbox write sequence\nFAIL: changed\ntest-end: mailbox write sequence exit 1\n'
        self.assertIsNone(gate.control_failure(1, spaced, 'mailbox write sequence'))
        self.assertIn('native test program', gate.control_failure(1, spaced))
        # A source check of the runner fails in a control whose intended failure is the native test, and the other way.
        checks = b'test-start: source checks\nFAIL source check: x\ntest-end: source checks exit 1\n'
        self.assertIn('native test program', gate.control_failure(1, checks))
        self.assertIsNone(gate.control_failure(1, checks, 'source checks'))
        self.assertIn('must fail in the step "source checks"', gate.control_failure(1, good, 'source checks'))
        self.assertIn('exit code 0', gate.control_failure(0, good))

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows worker')
    def test_negative_control_through_host_native(self):
        # The runner helpers of host_native.ps1 with a native program (python) under a real gate run.
        native = Path(__file__).resolve().parents[2] / 'driver/kmd/test/host_native.ps1'
        def control(program):
            self.script.write_text(
                'param([string]$Out)\n$ErrorActionPreference="Stop"\n'
                f'. "{native}"\n'
                f'Exit-NativeTest "{sys.executable}" @("-c", "{program}")\n', encoding='utf-8')
            self.plan['jobs'] = [{'name': 'sample', 'runner': 'check.ps1', 'arguments': [], 'fails': True}]
            return self.run_gate(real=True)['checks'][0]
        row = control("print('FAIL check 2'); raise SystemExit(1)")
        self.assertEqual(row['status'], 'PASS', row)
        log = (self.out / 'sample.log').read_text()
        self.assertIn('test-start: python.exe', log)
        self.assertIn('test-end: python.exe exit 1', log)
        row = control("import ctypes; print('FAIL check 2', flush=True); ctypes.windll.kernel32.ExitProcess(0xC0000005)")
        self.assertEqual(row['status'], 'FAIL', row)
        self.assertIn('negative control', row['error'])
        row = control("print('no failure'); raise SystemExit(1)")
        self.assertEqual(row['status'], 'FAIL', row)
        self.assertIn('no FAIL line inside', row['error'])
        # A source check that fails before the test.
        self.script.write_text('param([string]$Out)\nWrite-Output "FAIL source check"\nthrow "anchor moved"\n',
                               encoding='utf-8')
        row = self.run_gate(real=True)['checks'][0]
        self.assertEqual(row['status'], 'FAIL', row)
        self.assertIn('no test-end line', row['error'])

    def test_every_negative_control_runner_marks_its_test(self):
        # Each runner of a Fails entry in quick.ps1 marks its test (host_native.ps1), so that the oracle above can
        # tell the intended failure from a crash or a source check.
        import re
        repo = Path(__file__).resolve().parents[2]
        quick = (repo / 'tools/quality/quick.ps1').read_text(encoding='utf-8')
        runners = sorted({m.group(1) for m in re.finditer(r"s='([^']+)'[^\r\n]*Fails=\$true", quick)})
        self.assertGreater(len(runners), 20)
        unmarked = []
        for runner in runners:
            text = (repo / runner.replace('\\', '/')).read_text(encoding='utf-8')
            marked = ('Exit-NativeTest' in text or 'Invoke-NativeStep' in text) and 'host_native.ps1' in text
            marked = marked or ("'test-start: " in text and "exit 1'" in text)
            bare = re.search(r'(?m)^\s*&\s*"\$Out\\[^"]+\.exe"[^\r\n]*\r?\n\s*exit \$LASTEXITCODE', text)
            if not marked or bare:
                unmarked.append(runner)
        self.assertEqual(unmarked, [])

    @unittest.skipUnless(os.name == 'nt', 'Windows job object')
    def test_timeout_stops_process_tree(self):
        started = self.base / 'started.txt'
        child = f'import time,pathlib; pathlib.Path({str(started)!r}).write_text("x"); time.sleep(60)'
        parent = 'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",sys.argv[1]]); time.sleep(60)'
        log = self.base / 'timeout.log'
        code, timeout = gate.run_process([sys.executable, '-c', parent, child], self.base, dict(os.environ), log, 6)
        self.assertTrue(timeout)
        self.assertIsNone(code)
        self.assertTrue(started.exists(), 'the child did not start before the deadline: the control proves nothing')
        # The child had the log open as its output. No process of the tree is left when run_process returns, so the
        # log can be removed at once. Before, the child was only told to end: in a package suite of 2026-11-24 it
        # still held the log when the test removed its directory (BD-382).
        log.unlink()

    def spec_fail_first(self, fail_times):
        """A process double: the check 'sample' fails this number of times, then passes. It records how many
        checks ran at the moment of each call."""
        active, seen, lock = [0], [], threading.Lock()
        def process(command, cwd, env, log, timeout):
            name = Path(cwd).name.split('.')[0]
            with lock:
                active[0] += 1
                seen.append((name, active[0], Path(cwd).name))
            try:
                time.sleep(0.2)
                fail = name == 'sample' and len([s for s in seen if s[0] == 'sample']) <= fail_times
                log.write_bytes(b'FAIL fixture\n' if fail else b'fixture PASS\n')
                return (1 if fail else 0), False
            finally:
                with lock:
                    active[0] -= 1
        return process, seen

    def three_checks(self):
        self.plan['variables']['Profile'] = 'Full'
        self.plan['jobs'] += [{'name': 'child', 'action': 'exit 0'}, {'name': 'other', 'action': 'exit 0'}]
        self.manifest['checks']['child'] = {'inputs': ['sample'], 'depends': ['sample']}
        self.manifest['checks']['other'] = {'inputs': ['sample']}

    def test_full_profile_runs_a_failed_check_again_alone(self):
        # BD-382: six package attempts of tester.39 each ran every check again from the start, and each one
        # lost one check to the load of the suite (a deadline, a timing rule, a file still held by a process).
        self.three_checks()
        process, seen = self.spec_fail_first(1)
        with patch.object(self, 'fake_process', process):
            report = self.run_gate()
        rows = {row['name']: row for row in report['checks']}
        self.assertEqual(report['status'], 'PASS')
        self.assertEqual([s[0] for s in seen].count('sample'), 2)
        again = [s for s in seen if s[0] == 'sample'][1]
        self.assertEqual(again[1], 1, 'the second run of the check is alone')
        self.assertNotEqual(again[2], 'sample', 'the second run has a work directory of its own')
        self.assertEqual(rows['sample']['attempts'], 2)
        self.assertEqual(rows['sample']['first']['status'], 'FAIL')
        self.assertEqual(rows['sample']['first']['exit_code'], 1)
        self.assertEqual((self.out / 'sample.first.log').read_bytes(), b'FAIL fixture\n')
        self.assertEqual((self.out / 'sample.log').read_bytes(), b'fixture PASS\n')
        # The check that waited for it runs after the second run, and the report names the check that ran twice.
        self.assertEqual(rows['child']['status'], 'PASS')
        self.assertEqual([s[0] for s in seen].count('child'), 1)
        self.assertEqual(report['run_again'], ['sample'])
        self.assertNotIn('attempts', rows['other'])

    def test_full_profile_a_check_that_fails_two_times_fails(self):
        self.three_checks()
        process, seen = self.spec_fail_first(2)
        with patch.object(self, 'fake_process', process):
            report = self.run_gate()
        rows = {row['name']: row for row in report['checks']}
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual([s[0] for s in seen].count('sample'), 2)
        self.assertEqual(rows['sample']['status'], 'FAIL')
        self.assertEqual(rows['sample']['attempts'], 2)
        self.assertEqual(rows['child']['error'], 'prerequisite failed')
        self.assertEqual([s[0] for s in seen].count('child'), 0)

    def test_other_profiles_do_not_run_a_failed_check_again(self):
        self.three_checks()
        self.plan['variables']['Profile'] = 'KmdPackage'
        process, seen = self.spec_fail_first(1)
        with patch.object(self, 'fake_process', process):
            report = self.run_gate(True)
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual([s[0] for s in seen].count('sample'), 1)
        self.assertEqual(report['run_again'], [])

    def test_full_profile_deadline_only_ends_a_hang(self):
        # BD-357: the release suite runs its checks beside each other and beside the other work of the host, so the
        # deadline that fits a check alone stops a good check there. The Full profile has one floor for every check.
        seen = []

        def record(command, cwd, env, log, timeout):
            seen.append((self.plan['variables']['Profile'], timeout))
            log.write_bytes(b'fixture PASS\n')
            return 0, False

        def run(profile, declared):
            self.plan['variables']['Profile'] = profile
            if declared is None:
                self.manifest['checks']['sample'].pop('timeout', None)
            else:
                self.manifest['checks']['sample']['timeout'] = declared
            self.plan_path.write_text(json.dumps(self.plan), encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()), patch.object(gate, 'run_process', record):
                gate.execute(self.plan, self.manifest, self.plan_path, True, jobs=2)

        self.manifest['full_timeout'] = 1800
        run('KmdPackage', None)
        run('KmdPackage', 600)
        run('Full', None)
        run('Full', 600)
        run('Full', 2400)
        self.assertEqual(seen, [('KmdPackage', 10), ('KmdPackage', 600), ('Full', 1800), ('Full', 1800),
                                ('Full', 2400)])
        # A manifest without the floor keeps the declared deadline in the Full profile too.
        del self.manifest['full_timeout']
        run('Full', 600)
        self.assertEqual(seen[-1], ('Full', 600))
        repo = Path(__file__).resolve().parents[2]
        manifest = json.loads((repo / 'tools/quality/gate-inputs.json').read_bytes())
        self.assertGreaterEqual(manifest['full_timeout'], 1800)

    def test_release_paths_force_uncached_children(self):
        repo = Path(__file__).resolve().parents[2]
        for name in ('build-release.ps1', 'rebuild-check.ps1'):
            source = (repo / 'tools/release' / name).read_text()
            self.assertIn("$env:BC250_GATE_NOCACHE = '1'", source)
        source = (repo / 'tools/release/build-release.ps1').read_text()
        self.assertIn("$qualityOut $repo 'Full'", source)
        manifest = json.loads((repo / 'tools/quality/gate-inputs.json').read_bytes())
        self.assertTrue({'ledger', 'status-map', 'release-provenance'} <= set(manifest['release_only']))


if __name__ == '__main__':
    unittest.main()
