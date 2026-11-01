"""Synthetic publication and admission controls. No network, lab or driver process."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('baseline_contract', Path(__file__).with_name('baseline_contract.py'))
bc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bc)


def fixture(root):
    """Synthetic members model the manifest's source/member rename contract."""
    package = root / 'package'
    package.mkdir()
    members, components, files = [], [], []
    sources = list(bc.SOURCES.values()) + [
        'payload/vulkan/radeon_icd.json', 'payload/wow64/desktop/bc250d3d_router.dll',
        'payload/wow64/d3d12/amdgpu_wddm_d3d12.dll', 'payload/wow64/vulkan/radeon_icd.json']
    renamed = {
        'payload/d3d12/amdgpu_wddm_radv.dll': 'd3d12/amdgpu_wddm_radv_d3d12.dll',
        'payload/wow64/desktop/bc250d3d_router.dll': 'wow64/desktop/bc250d3d_router32.dll',
        'payload/wow64/d3d12/amdgpu_wddm_d3d12.dll': 'wow64/d3d12/amdgpu_wddm_d3d1232.dll',
        'payload/wow64/vulkan/radeon_icd.json': 'wow64/vulkan/radeon_icd32.json'}
    for i, source in enumerate(sources):
        member = renamed.get(source, source.removeprefix('payload/'))
        relative = 'payload/kmd/' + member
        path = package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(('synthetic member ' + str(i)).encode())
        h = bc.digest(path)
        members.append(dict(source=source, member=member, path=relative, sha256=h))
        components.append(dict(role='test', package_path=relative,
                               install_path='DriverStore (bc250kmd.inf)\\' + member.replace('/', '\\'), sha256=h))
        files.append(dict(path=relative, sha256=h))
    for role, relative, install in (
        ('cli', 'payload/tools/bc250kmd_cli.exe', '<InstallDir>\\tools\\bc250kmd_cli.exe'),
        ('kmd', 'payload/kmd/bc250kmd.sys', 'DriverStore (bc250kmd.inf)\\bc250kmd.sys')):
        h = hashlib.sha256(role.encode()).hexdigest().upper()
        components.append(dict(role=role, package_path=relative, install_path=install, sha256=h))
        files.append(dict(path=relative, sha256=h))
    def reg(value):
        return dict(kind='MultiString', value=value)
    registrations = dict(
        UserModeDriverName=reg(['<>', '%13%\\desktop\\bc250d3d_router.dll', '%13%\\desktop\\bc250d3d_router.dll', '%13%\\d3d12\\amdgpu_wddm_d3d12.dll']),
        UserModeDriverNameWow=reg(['<>', '%13%\\wow64\\desktop\\bc250d3d_router32.dll', '%13%\\wow64\\desktop\\bc250d3d_router32.dll', '%13%\\wow64\\d3d12\\amdgpu_wddm_d3d1232.dll']),
        VulkanDriverName=reg(['%13%\\vulkan\\radeon_icd.json']),
        VulkanDriverNameWow=reg(['%13%\\wow64\\vulkan\\radeon_icd32.json']))
    manifest = dict(name='synthetic-tester.35', release='0.7.216.107-tester.35', flavour='release',
                    kmd_version='40.7.216.107', kmd_build='0.7.216.65', kmd_abi='0x000700D8',
                    driver_package=dict(flavour='release', root='payload/kmd', members=members, registration=registrations),
                    defaults=dict(desktop_router=dict(DwmForceCpu=0)), components=components, files=files)
    manifest_path = package / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
    return manifest, manifest_path


class BaselineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='baseline-contract-', dir=os.environ.get('BC250_TEST_TMP'))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest, self.path = fixture(self.root)
        self.hash = bc.digest(self.path)
        self.baseline = bc.derive(self.manifest, self.hash, self.path, r'C:\Program Files\amdgpu-wddm')
        self.pin = dict(release=self.manifest['release'], manifest_sha256=self.hash)

    def test_matching_publication_and_pending_admission(self):
        bc.validate_fresh(self.baseline, self.pin, self.manifest['release'])
        self.assertTrue(self.baseline['admission_required'])
        self.assertNotIn('final_installed', self.baseline)

    def test_old_newer_unpublished_and_wrong_manifest_refuse(self):
        for release, h in [('0.7.216.101-tester.30', self.hash), ('0.7.216.108-tester.36', self.hash),
                           (self.manifest['release'], 'F' * 64)]:
            with self.subTest(release=release, h=h):
                b = copy.deepcopy(self.baseline)
                b['release'].update(release=release, manifest_sha256=h)
                with self.assertRaises(bc.Refusal):
                    bc.validate_fresh(b, self.pin, self.manifest['release'])

    def test_stale_publication_pointer_refuses(self):
        with self.assertRaisesRegex(bc.Refusal, 'Published pin is stale'):
            bc.validate_fresh(self.baseline, self.pin, '0.7.216.108-tester.36')

    def test_current_maintenance_and_history(self):
        state = ('## Current, unit A off for maintenance\nGitHub release\n'
                 'v0.7.216.107-tester.35 (full release), manifest ABCDEF01\n'
                 '## Previous\nGitHub release v0.7.216.106-tester.34, manifest 01234567\n')
        self.assertEqual(('0.7.216.107-tester.35', 'ABCDEF01'), bc.current_publication(state))
        newer = state.replace('107-tester.35', '108-tester.36').replace('ABCDEF01', '12345678')
        self.assertEqual(('0.7.216.108-tester.36', '12345678'), bc.current_publication(newer))
        with self.assertRaises(bc.Refusal):
            bc.current_publication('## Current\nmaintenance\n## Previous\n' + state)

    def test_installed_package_block_carries_the_publication(self):
        # STATE.md since 2026-11-01: Current names the last trial, the package block names the GitHub release.
        state = ('## Current: original app-shell restored, 2026-11-01\n- trial notes, receipt D88EF743\n'
                 '## Previous completed trials\n- GitHub release v0.7.216.106-tester.34, manifest 01234567\n'
                 '## Installed package baseline and retained recovery material\n'
                 '- Package r7n (release zip D03EBC58, setup A7AAE25E, manifest 0D416A75). GitHub release\n'
                 '  v0.7.216.107-tester.35 (tag on be44a286, full release)\n'
                 '## Previous, 2026-10-30: tester.34 published\n- GitHub release v0.7.216.106-tester.34\n')
        self.assertEqual(('0.7.216.107-tester.35', '0D416A75'), bc.current_publication(state))
        both = state.replace('- trial notes, receipt D88EF743\n',
                             '- GitHub release v0.7.216.108-tester.36, manifest 89ABCDEF\n')
        with self.assertRaisesRegex(bc.Refusal, 'different publications'):
            bc.current_publication(both)
        same = state.replace('- trial notes, receipt D88EF743\n',
                             '- GitHub release v0.7.216.107-tester.35, manifest 0D416A75\n')
        self.assertEqual(('0.7.216.107-tester.35', '0D416A75'), bc.current_publication(same))
        with self.assertRaisesRegex(bc.Refusal, 'no Current block'):
            bc.current_publication(state.replace('## Current:', '## Earlier:'))
        with self.assertRaisesRegex(bc.Refusal, 'no publication record'):
            bc.current_publication(state.replace('GitHub release\n  v0.7.216.107', 'tag\n  v0.7.216.107'))

    def test_driverstore_renames_and_registration_are_derived(self):
        b = self.baseline
        self.assertTrue(b['session']['icd'].endswith('amdgpu_wddm_radv_d3d12.dll'))
        self.assertTrue(b['registration']['umd_names_wow'][3].endswith('amdgpu_wddm_d3d1232.dll'))
        self.assertIsInstance(b['registration']['vulkan_name'], str)
        self.assertEqual('40.7.216.107', b['kmd_version'])

    def test_bad_mapping_abi_and_missing_component_refuse(self):
        bad = []
        m = copy.deepcopy(self.manifest)
        m['driver_package']['members'][0]['member'] = '../escape.dll'
        bad.append(m)
        m = copy.deepcopy(self.manifest)
        m['kmd_version'] = '40.7.217.107'
        bad.append(m)
        m = copy.deepcopy(self.manifest)
        m['components'].pop(0)
        bad.append(m)
        m = copy.deepcopy(self.manifest)
        m['driver_package']['registration']['UserModeDriverName']['value'][1] = '%13%\\missing.dll'
        bad.append(m)
        for m in bad:
            with self.assertRaises(bc.Refusal):
                bc.derive(m, self.hash, self.path, r'C:\Program Files\amdgpu-wddm')

    def test_reordered_accepted_keys_do_not_change_in_place(self):
        b = copy.deepcopy(self.baseline)
        b['d3d12']['accepted'] = dict(reversed(list(b['d3d12']['accepted'].items())))
        self.assertTrue(bc.triplet(b, self.root, {})[3])

    def test_explicit_candidate_is_kept_and_not_accepted(self):
        candidate = self.root / 'candidate.dll'
        candidate.write_bytes(b'synthetic candidate')
        env = dict(SESSION_ENGINE=str(candidate))
        _, paths, _, accepted = bc.triplet(self.baseline, self.root, env)
        self.assertEqual(candidate, paths[1])
        self.assertFalse(accepted)
        with self.assertRaisesRegex(bc.Refusal, 'candidate'):
            bc.require(accepted, 'Final acceptance refuses candidate selection')
        # Candidate selection does not rescue a stale baseline.
        stale = copy.deepcopy(self.baseline)
        stale['release']['release'] = '0.7.216.101-tester.30'
        with self.assertRaises(bc.Refusal):
            bc.validate_fresh(stale, self.pin, self.manifest['release'])

    def test_explicit_missing_candidate_is_not_replaced(self):
        with self.assertRaisesRegex(bc.Refusal, 'missing'):
            bc.triplet(self.baseline, self.root, dict(SESSION_ENGINE=str(self.root / 'missing.dll')))

    def test_explicit_adapter_is_not_overridden_by_inherited_shell(self):
        with self.assertRaisesRegex(bc.Refusal, 'Conflicting explicit adapter'):
            bc.triplet(self.baseline, self.root, dict(SESSION_ADAPTER='adapter123',
                       SESSION_SHELL=self.baseline['session']['shell']))

    def test_no_publication_record_refuses(self):
        with self.assertRaises(FileNotFoundError):
            bc.load_baseline(self.root, self.root)
        with self.assertRaises(bc.Refusal):
            bc.receipt_release('{"tag":"v0.7.216.107-tester.35","draft":true}')

    def test_staged_old_or_changed_admission_refuses(self):
        with self.assertRaises(bc.Refusal):
            bc.check_staged({}, self.baseline)
        bc.check_staged(dict(published_release=self.baseline['release']), self.baseline)
        with self.assertRaises(bc.Refusal):
            bc.check_staged(dict(published_release=self.baseline['release'], final_installed={'boot_utc': 'synthetic'}), self.baseline)

    def test_admission_cannot_be_fabricated_by_updating_only_timestamp(self):
        incomplete = dict(manifest_sha256=self.hash, kmd_version=self.manifest['kmd_version'],
                          driver_store_root=r'C:\Windows\System32\DriverStore\FileRepository\bc250kmd.inf_amd64_synthetic',
                          boot_utc='2000-01-01T00:00:00Z', state_reader='cli-state-snapshot-v1', games={})
        with self.assertRaisesRegex(bc.Refusal, 'game records'):
            bc.derive(self.manifest, self.hash, self.path, r'C:\Program Files\amdgpu-wddm', incomplete)


if __name__ == '__main__':
    unittest.main()
