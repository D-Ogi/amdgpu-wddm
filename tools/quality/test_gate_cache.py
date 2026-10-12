"""Cache controls use inert fixtures and child processes, never a lab or driver."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
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

    @unittest.skipUnless(os.name == 'nt', 'Windows job object')
    def test_timeout_stops_process_tree(self):
        marker = self.base / 'survivor.txt'
        child = f'import time,pathlib; time.sleep(1.5); pathlib.Path({str(marker)!r}).write_text("alive")'
        parent = 'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",sys.argv[1]]); time.sleep(10)'
        code, timeout = gate.run_process([sys.executable, '-c', parent, child], self.base, dict(os.environ),
                                         self.base / 'timeout.log', 0.5)
        self.assertTrue(timeout)
        self.assertIsNone(code)
        # An actual child must not outlive the worker's deadline.
        import time
        time.sleep(1.7)
        self.assertFalse(marker.exists())

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
