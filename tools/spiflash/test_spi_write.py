"""Exercise the real PowerShell writer with synthetic images and a fake child process."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
import unittest

HERE = Path(__file__).resolve().parent
SIZE = 16 * 1024 * 1024
CHIP = 'MX25L12835F/MX25L12845E/MX25L12865E'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"

@unittest.skipUnless(os.name == 'nt', 'SPI writer contract uses Windows PowerShell file-sharing and mutexes')
class WriterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pwsh = shutil.which('pwsh')
        if not cls.pwsh:
            raise RuntimeError('pwsh is required')
        location = os.environ.get('BC250_TEST_TMP')
        if not location:
            raise RuntimeError('set BC250_TEST_TMP to a private workspace directory outside repositories')
        cls.parent = Path(location).resolve(strict=True)
        cls.script = Path(os.environ.get('BC250_SPI_WRITE_SCRIPT', HERE / 'spi_write.ps1')).resolve()

    def setUp(self):
        # Inherit the workspace ACL. Python's Windows mode-0700 temp ACL excludes restricted tokens.
        self.temp = (self.parent / ('spi-host-' + uuid.uuid4().hex)).resolve()
        self.temp.mkdir()
        self.assertTrue(self.temp.is_relative_to(self.parent) and self.temp != self.parent)
        self.addCleanup(shutil.rmtree, self.temp)
        self.base = bytes(range(256)) * (SIZE // 256)
        candidate = bytearray(self.base); candidate[4095] ^= 1; candidate[4096] ^= 1
        self.candidate = bytes(candidate)
        self.backup = self.temp / 'backup.bin'; self.backup.write_bytes(self.base)
        self.image = self.temp / 'original image.bin'; self.image.write_bytes(self.candidate)
        self.chip = self.temp / 'chip.bin'; self.chip.write_bytes(self.base)
        self.mock = self.temp / 'fake flashrom.ps1'
        self.mock.write_text(f'& {ps_quote(sys.executable)} {ps_quote(HERE / "fake_flashrom_test.py")} @args\nexit $LASTEXITCODE\n')
        self.compatibility = self.temp / 'write-review.txt'
        self.compatibility.write_text('Synthetic backend only. Not a hardware compatibility approval.\n')
        self.plan = {
            'schema': 1, 'unit': 'A', 'purpose': 'vcn-repair', 'mode': 'write',
            'image_sha256': sha(self.candidate), 'base_sha256': sha(self.base),
            'chip': CHIP, 'chip_size': SIZE,
            'backup': {'path': str(self.backup), 'sha256': sha(self.base)},
            'edits': [{'offset': 4095, 'before': self.base[4095:4097].hex(), 'after': self.candidate[4095:4097].hex()}],
            'flashrom_sha256': sha(self.mock.read_bytes()), 'flashrom_version': 'flashrom v1.2',
            'flashrom_dependencies': [],
            'write_compatibility': {'path': str(self.compatibility), 'sha256': sha(self.compatibility.read_bytes())},
        }
        for name in ('libusb-1.0.dll', 'libftdi1.dll', 'libwinpthread-1.dll'):
            path = self.temp / name
            path.write_bytes(('Synthetic unused dependency: ' + name).encode())
            self.plan['flashrom_dependencies'].append({'path': str(path), 'sha256': sha(path.read_bytes())})

    def run_writer(self, scenario='ok', *, plan=None, extra=(), script=None, wrong_plan_hash=False, outroot=None):
        plan = self.plan if plan is None else plan
        planfile = self.temp / 'plan.json'
        planfile.write_text(json.dumps(plan))
        (self.temp / 'config.json').write_text(json.dumps({
            'scenario': scenario, 'chip_name': CHIP, 'original': str(self.image),
            'second_readback': '-SecondReadback' in extra}))
        callfile = self.temp / 'calls.jsonl'
        callfile.unlink(missing_ok=True)
        (self.temp / 'status.txt').unlink(missing_ok=True)
        previous = set((self.temp / 'outputs').glob('spi-write-*'))
        env = dict(os.environ, BC250_FAKE_SPI=str(self.temp), BC250_ROOT=str(self.parent), TEMP=str(self.temp), TMP=str(self.temp))
        args = [self.pwsh, '-NoProfile', '-File', str(script or self.script),
                '-Image', str(self.image), '-ImageSha256', plan['image_sha256'],
                '-BaseSha256', plan.get('base_sha256', sha(self.base)), '-PlanFile', str(planfile),
                '-PlanSha256', '0' * 64 if wrong_plan_hash else sha(planfile.read_bytes()),
                '-OutRoot', str(outroot or self.temp / 'outputs'), '-Flashrom', str(self.mock), *extra]
        p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=90)
        calls = [json.loads(x) for x in callfile.read_text().splitlines()] if callfile.exists() else []
        runs = list(set((self.temp / 'outputs').glob('spi-write-*')) - previous)
        receipt_path = runs[0] / 'result.json' if runs else None
        receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig')) if receipt_path and receipt_path.is_file() else None
        self.details = p.stdout + '\n' + p.stderr
        return p, calls, receipt, runs

    def assert_refused(self, result, message):
        p, calls, receipt, _ = result
        self.assertEqual(p.returncode, 2, self.details)
        self.assertIn(message, self.details)
        self.assertNotIn('write', [c['operation'] for c in calls])
        if receipt: self.assertFalse(receipt['array_write_invoked'])

    def test_success_and_diff_crosses_sector_boundary(self):
        p, calls, receipt, runs = self.run_writer()
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual(receipt['verdict'], 'PASS')
        self.assertEqual(self.chip.read_bytes(), self.candidate)
        diff = json.loads((runs[0] / 'diff.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(diff, {'bytes': 2, 'sectors_4k': 2, 'ranges': [{'offset': 4095, 'length': 2}]})
        self.assertEqual([x['operation'] for x in calls], ['version', 'probe', 'probe', 'before1',
            'probe-between-before', 'before2', 'write', 'probe-before-readback', 'after1', 'probe-after'])
        self.assertEqual(receipt['readbacks'], 1)
        self.assertEqual(len(list(runs[0].glob('after*.bin'))), 1)
        self.assertIn('1 complete readback', self.details)

    def test_second_readback_is_an_explicit_option(self):
        p, calls, receipt, runs = self.run_writer(extra=('-SecondReadback',))
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual([x['operation'] for x in calls][6:], ['write', 'probe-before-readback', 'after1',
            'probe-between-after', 'after2', 'probe-after'])
        self.assertEqual(receipt['readbacks'], 2)
        self.assertIn('2 complete readbacks', self.details)
    def test_bad_second_readback(self):
        p, _, receipt, _ = self.run_writer('second-readback-mismatch', extra=('-SecondReadback',))
        self.assertEqual(p.returncode, 1, self.details)
        self.assertEqual(receipt['readbacks'], 2)
        self.assertFalse(receipt['readback_matches'])
    def test_failed_readback_is_not_counted(self):
        p, _, receipt, _ = self.run_writer('after-fail')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertEqual(receipt['readbacks'], 0)

    def test_plan_hash(self): self.assert_refused(self.run_writer(wrong_plan_hash=True), 'plan hash mismatch')
    def test_plan_identity_guards(self):
        for field, value, reason in [('schema', 2, 'unsupported plan schema'),
                                     ('unit', 'B', 'unit A VCN work'),
                                     ('purpose', 'unrelated', 'unit A VCN work'),
                                     ('mode', 'restore', 'mode differs'),
                                     ('chip', 'other', 'chip or size differs'),
                                     ('chip_size', 1024, 'chip or size differs'),
                                     ('base_sha256', '0' * 64, 'base/backup/plan hashes differ'),
                                     ('flashrom_version', 'other', 'reviewed flashrom')]:
            with self.subTest(field=field):
                plan = copy.deepcopy(self.plan); plan[field] = value
                self.assert_refused(self.run_writer(plan=plan), reason)
    def test_edit_shape_guards(self):
        for edits, reason in [([{'offset': -1, 'before': '00', 'after': '01'}], 'outside image'),
                              ([{'offset': SIZE, 'before': '00', 'after': '01'}], 'outside image'),
                              ([{'offset': 1.5, 'before': '00', 'after': '01'}], 'must be an integer'),
                              ([{'offset': 1, 'before': 'xx', 'after': '01'}], 'hexadecimal pairs'),
                              ([{'offset': 1, 'before': '00', 'after': '0102'}], 'lengths differ'),
                              ({}, 'must be an array')]:
            with self.subTest(edits=edits):
                plan = copy.deepcopy(self.plan); plan['edits'] = edits
                self.assert_refused(self.run_writer(plan=plan), reason)
    def test_probe_identity_guards(self):
        for scenario, reason in [('probe-none', 'exactly one chip'), ('probe-multiple', 'multiple chip'),
                                 ('probe-status', 'omitted the status'), ('probe-chip', 'chip differs'),
                                 ('probe-size', 'size or bus differs'), ('version-fail', 'version command failed'),
                                 ('version-wrong', 'version command failed')]:
            with self.subTest(scenario=scenario): self.assert_refused(self.run_writer(scenario), reason)
    def test_sector_parameter_range(self):
        self.assert_refused(self.run_writer(extra=('-MaxSectors', '0')), 'MaxSectors must be')
    def test_output_inside_repository(self):
        # A synthetic .git marker proves rejection without writing any test image into the actual repo.
        repo = self.temp / 'repo'; repo.mkdir(); (repo / '.git').write_text('gitdir: synthetic')
        self.assert_refused(self.run_writer(outroot=repo / 'out'), 'inside a repository')
    def test_image_hash(self):
        self.image.write_bytes(self.base)
        self.assert_refused(self.run_writer(), 'image hash mismatch')
    def test_image_size(self):
        self.image.write_bytes(b'synthetic'); self.plan['image_sha256'] = sha(self.image.read_bytes())
        self.assert_refused(self.run_writer(), 'wrong length')
    def test_unapproved_edit(self):
        b = bytearray(self.candidate); b[100] ^= 1; self.image.write_bytes(b); self.plan['image_sha256'] = sha(b)
        self.assert_refused(self.run_writer(), 'outside the approved edits')
    def test_expected_bytes(self):
        self.plan['edits'][0]['before'] = '0000'
        self.assert_refused(self.run_writer(), 'expected bytes differ')
    def test_overlapping_edits(self):
        self.plan['edits'].append(copy.deepcopy(self.plan['edits'][0]))
        self.assert_refused(self.run_writer(), 'overlapping edits')
    def test_sector_limit(self): self.assert_refused(self.run_writer(extra=('-MaxSectors', '1')), 'sector limit exceeded')
    def test_backup_identity(self):
        self.plan['backup']['sha256'] = '0' * 64
        self.assert_refused(self.run_writer(), 'backup identity mismatch')
    def test_tool_pin(self):
        self.plan['flashrom_sha256'] = '0' * 64
        self.assert_refused(self.run_writer(), 'pinned file hash mismatch')
    def test_compatibility_pin(self):
        self.plan['write_compatibility']['sha256'] = '0' * 64
        self.assert_refused(self.run_writer(), 'compatibility review hash mismatch')
    def test_dependency_pins(self):
        plan = copy.deepcopy(self.plan); plan['flashrom_dependencies'] = []
        self.assert_refused(self.run_writer(plan=plan), 'missing pinned dependency')
        plan = copy.deepcopy(self.plan); plan['flashrom_dependencies'][0]['sha256'] = '0' * 64
        self.assert_refused(self.run_writer(plan=plan), 'pinned file hash mismatch')
        plan = copy.deepcopy(self.plan); plan['flashrom_dependencies'].append(plan['flashrom_dependencies'][0])
        self.assert_refused(self.run_writer(plan=plan), 'duplicate dependency')
    def test_private_workspace_boundary(self):
        self.assert_refused(self.run_writer(outroot=self.parent.parent / 'not-created'), 'outside BC250_ROOT')
    def test_probe_error_with_plausible_output(self): self.assert_refused(self.run_writer('probe-fail'), 'probe-before failed')
    def test_protection_unlock_allowed_and_recorded(self):
        p, calls, receipt, _ = self.run_writer('probe-protected')
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual(receipt['status_before'], '0x44')
        self.assertEqual(receipt['status_after'], '0x40')
        self.assertTrue(next(c for c in calls if c['operation'] == 'before1')['read_unlocked_protection'])
    def test_non_protection_status_changes_refused(self):
        self.assert_refused(self.run_writer('probe-invalid-status'), 'unexpected non-protection status bits')
    def test_changed_post_status_fails(self):
        p, _, receipt, _ = self.run_writer('post-status-changed')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertEqual(receipt['verdict'], 'FAIL')
        self.assertNotIn('VERDICT PASS', self.details)
    def test_readback_cannot_hide_protection_change(self):
        p, calls, receipt, _ = self.run_writer('write-sets-protection')
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual(receipt['status_probes'].get('after1-status'), '0x44', self.details)
        self.assertEqual(receipt['status_after'], '0x40')
        self.assertTrue(next(c for c in calls if c['operation'] == 'after1')['read_unlocked_protection'])
    def test_second_read_cannot_hide_protection_change(self):
        p, calls, receipt, _ = self.run_writer('read-sets-protection')
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual(receipt['status_probes'].get('before2-status'), '0x44', self.details)
        self.assertTrue(next(c for c in calls if c['operation'] == 'before2')['read_unlocked_protection'])
    def test_before_error_with_valid_file(self): self.assert_refused(self.run_writer('before-fail'), 'before1 command failed')
    def test_before_missing(self): self.assert_refused(self.run_writer('before-missing'), 'did not produce a file')
    def test_before_short(self): self.assert_refused(self.run_writer('before-short'), 'wrong length')
    def test_unstable_reads(self): self.assert_refused(self.run_writer('base-unstable'), 'base reads differ')
    def test_stale_read_file(self): self.assert_refused(self.run_writer('stale-before2'), 'destination exists')
    def test_current_variable_store_changed(self):
        b = bytearray(self.base); b[10] ^= 1; self.chip.write_bytes(b)
        self.assert_refused(self.run_writer(), 'chip base hash mismatch')
    def test_original_path_change_does_not_change_programmed_image(self):
        p, calls, _, _ = self.run_writer('mutate-original')
        self.assertEqual(p.returncode, 0, self.details)
        written = next(c for c in calls if c['operation'] == 'write')
        self.assertEqual(written['programmed_sha256'], sha(self.candidate))
        self.assertNotEqual(self.image.read_bytes(), self.candidate)
    def test_snapshot_cannot_be_written(self):
        p, calls, _, _ = self.run_writer('mutate-snapshot')
        self.assertEqual(p.returncode, 0, self.details)
        self.assertTrue(next(c for c in calls if c['operation'] == 'before1')['snapshot_write_denied'])
    def test_backup_snapshot_cannot_be_written(self):
        p, calls, _, _ = self.run_writer('mutate-backup')
        self.assertEqual(p.returncode, 0, self.details)
        self.assertTrue(next(c for c in calls if c['operation'] == 'before1')['snapshot_write_denied'])
    def test_failed_write_stays_failed_with_matching_readback(self):
        p, _, receipt, _ = self.run_writer('write-fail')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertEqual(receipt['verdict'], 'FAIL')
        self.assertTrue(receipt['readback_matches'])
        self.assertEqual(receipt['write_exit'], 9)
    def test_bad_readback(self):
        p, _, receipt, _ = self.run_writer('readback-mismatch')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertFalse(receipt['readback_matches'])
    def test_readback_error_with_valid_file(self):
        p, _, receipt, _ = self.run_writer('after-fail')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertEqual(receipt['verdict'], 'FAIL')
    def test_write_without_verify_marker(self):
        p, _, receipt, _ = self.run_writer('write-no-verify')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertTrue(receipt['readback_matches'])
    def test_partial_write(self):
        p, calls, receipt, _ = self.run_writer('write-partial')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertFalse(receipt['readback_matches'])
        self.assertEqual(sum(c['operation'] == 'write' for c in calls), 1)
    def test_post_status_failure(self):
        p, _, _, _ = self.run_writer('post-probe-fail')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertNotIn('VERDICT PASS', self.details)
    def test_receipt_failure_never_prints_pass(self):
        p, _, receipt, _ = self.run_writer('receipt-fail')
        self.assertEqual(p.returncode, 1, self.details)
        self.assertIsNone(receipt)
        self.assertNotIn('VERDICT PASS', self.details)
    def test_programmer_mutex(self):
        ready = self.temp / 'mutex-ready'
        command = ("$m=[Threading.Mutex]::new($false,'Global\\BC250-CH341A-SPI');"
                   "$null=$m.WaitOne();try{[IO.File]::WriteAllText(" + ps_quote(ready) + ", 'ready');"
                   "$null=[Console]::ReadLine()}finally{$m.ReleaseMutex();$m.Dispose()}")
        process = subprocess.Popen([self.pwsh, '-NoProfile', '-Command', command], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.02)
            self.assertTrue(ready.exists(), 'mutex helper did not start')
            self.assert_refused(self.run_writer(), 'another process owns')
        finally:
            process.communicate('\n', timeout=5)
    def test_explicit_restore(self):
        self.plan['mode'] = 'restore'; self.plan['image_sha256'] = sha(self.base)
        self.image.write_bytes(self.base); self.chip.write_bytes(self.candidate)
        p, _, receipt, _ = self.run_writer(extra=('-Restore', '-MaxSectors', '1'))
        self.assertEqual(p.returncode, 0, self.details)
        self.assertEqual(receipt['mode'], 'restore')
        self.assertEqual(self.chip.read_bytes(), self.base)
    def test_restore_requires_the_backup(self):
        self.plan['mode'] = 'restore'
        self.assert_refused(self.run_writer(extra=('-Restore',)), 'restore image is not')
    def test_noop_has_no_write(self):
        self.plan['image_sha256'] = sha(self.base); self.plan['edits'] = []; self.image.write_bytes(self.base)
        p, calls, receipt, _ = self.run_writer()
        self.assertEqual(p.returncode, 0, self.details)
        self.assertFalse(receipt['array_write_invoked'])
        self.assertNotIn('write', [c['operation'] for c in calls])
        p2, _, _, runs2 = self.run_writer()
        self.assertEqual(p2.returncode, 0, self.details)
        self.assertEqual(len(list((self.temp / 'outputs').glob('spi-write-*'))), 2)
        self.assertEqual(len(runs2), 1)

if __name__ == '__main__':
    unittest.main()
