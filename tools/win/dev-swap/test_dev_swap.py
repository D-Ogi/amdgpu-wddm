"""Host tests of dev_swap.py and its lab script, on a fake lab directory tree. No lab, no ssh, no DriverStore.

The fake transport runs lab\\dev-swap-lab.ps1 in Windows PowerShell 5.1 on this PC with -FakeRoot: the package,
the guard files and the held files come from a directory tree built here, so the same lab code runs that the lab
runs, without takeown and icacls. The PE images are real (packagecheck's test builder with exports, stamped with a
version resource), so the catalog image digest of the lab script is compared with image_identity on real input.

    python -B -m unittest discover -s tools/win/dev-swap -p "test_*.py"
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PS51 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
sys.path.insert(0, str(HERE))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


dev_swap = _load('dev_swap', HERE / 'dev_swap.py')
pebuild = _load('test_packagecheck', REPO / 'tools' / 'packagecheck' / 'test_packagecheck.py')
vsbuild = _load('test_versionstamp', REPO / 'tools' / 'build' / 'test_versionstamp.py')
import versionstamp  # noqa: E402  (on the path through dev_swap)
from image_identity import image_digest  # noqa: E402

PACKAGE = 'bc250kmd.inf_amd64_0123456789abcdef'
OTHER = 'bc250kmd.inf_amd64_fedcba9876543210'
CAPS = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)


def scratch_root() -> Path:
    base = Path(os.environ.get('BC250_TEST_TMP') or REPO / 'tools' / 'test-tmp')
    base.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix='dev-swap-', dir=base))


def make_image(path: Path, role, tag: str, original=None):
    """A real DLL of the role's architecture: a distinct .text, an export directory with the role's export at RVA
    0x2000 and, unless original is False, a version resource with that OriginalFilename. The images come from the
    version-stamp tests (tools/build) and the export table from the packagecheck tests."""
    plus = role.arch == 'x64'
    text = (b'\xc3' + tag.encode('ascii')).ljust(32, b'\0')
    edata = pebuild.export_blob(0x2000, list(role.exports[:1]) or ['Nothing'], role.original)
    leaves = None
    if original is not False:
        leaves = [(versionstamp.RT_VERSION, 1, 0x409, 0, versionstamp.version_resource(
            (1, 0, 0, 0), (1, 0, 0, 0), [('OriginalFilename', original or role.original)], 2))]
    data = bytearray(vsbuild.make_pe(plus=plus, sections=((b'.text', text, 0x60000020), (b'.edata', edata, 0x40000040)),
                                     leaves=leaves))
    dirs = 0x98 + (0xF0 if plus else 0xE0) - 16 * 8
    struct.pack_into('<II', data, dirs, 0x2000, len(edata))
    path.write_bytes(bytes(data))
    return path


def record(engine: str, icd: str) -> bytes:
    return struct.pack('<17I32s32s', 0x4334314D, 3, 132, 0, *CAPS, bytes.fromhex(engine), bytes.fromhex(icd))


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


class FakeLab:
    """The transport of dev_swap.main: the lab script in PowerShell 5.1 with -FakeRoot, and a local copy."""

    def __init__(self, root: Path):
        self.root = root
        self.calls = []

    def run(self, args, timeout):
        self.calls.append(args[1])
        # A PSModulePath inherited from PowerShell 7 hides the 5.1 modules; the lab session has its own.
        env = {k: v for k, v in os.environ.items() if k.upper() != 'PSMODULEPATH'}
        done = subprocess.run(env=env, args=[str(PS51), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                               str(dev_swap.LAB_SCRIPT), *args, '-FakeRoot', str(self.root)],
                              capture_output=True, text=True, timeout=timeout)
        return done.returncode, done.stdout, done.stderr

    def push(self, paths, remote_dir):
        Path(remote_dir).mkdir(parents=True, exist_ok=True)
        for p in paths:
            shutil.copyfile(p, Path(remote_dir) / Path(p).name)


@unittest.skipUnless(os.name == 'nt' and PS51.is_file(), 'the lab script runs in Windows PowerShell 5.1')
class DevSwapTests(unittest.TestCase):
    def setUp(self):
        self.root = scratch_root()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.table = dev_swap.role_table()
        self.by_key = {r.key: r for r in self.table}
        self.base = self.root / 'labbase'
        self.src = self.root / 'src'
        self.src.mkdir()
        self.pkg = self.make_package(PACKAGE, 'base')
        self.facts(PACKAGE)
        self.lab = FakeLab(self.root)
        self.installed = {k: sha(p) for k, p in self.paths(self.pkg).items()}

    # ---- the fake tree ----
    def make_package(self, name, tag):
        pkg = self.root / 'repo' / name
        for r in self.table:
            sub = pkg / ('wow64' if r.arch == 'x86' else '') / r.role.split('-')[0].replace('dxvk', 'd3d11').replace('vkd3d', 'd3d12')
            sub.mkdir(parents=True, exist_ok=True)
            if r.pe:
                make_image(sub / r.name, r, f'{tag}-{r.key}')
        for arch in ('x64', 'x86'):
            rec = self.by_key[f'{arch}:d3d11-record']
            eng = next(pkg.rglob(self.by_key[f'{arch}:dxvk'].name))
            icd = next(pkg.rglob(self.by_key[f'{arch}:d3d11-icd'].name))
            (eng.parent / rec.name).write_bytes(record(image_digest(eng), image_digest(icd)))
        (pkg / 'bc250kmd.sys').write_bytes(b'kmd')
        return pkg

    def facts(self, package, guard='', held=(), trip=None):
        (self.root / 'facts.json').write_text(json.dumps({
            'repo': str(self.root / 'repo'),
            'imagePath': f'\\SystemRoot\\System32\\DriverStore\\FileRepository\\{package}\\bc250kmd.sys',
            'adapters': [{'service': 'bc250kmd', 'inf': 'oem7.inf'}],
            'infFiles': {'oem7.inf': {'packages': [package], 'active': package}},
            'guard': guard, 'held': list(held)}), encoding='utf-8')
        gdir = self.root / 'guard'
        gdir.mkdir(exist_ok=True)
        if trip:
            (gdir / 'trip.json').write_text(json.dumps({'reason': trip}), encoding='utf-8')
        elif (gdir / 'trip.json').exists():
            (gdir / 'trip.json').unlink()

    def paths(self, pkg=None):
        pkg = pkg or self.pkg
        return {r.key: next(pkg.rglob(r.name)) for r in self.table}

    def new(self, key, tag, **kw):
        r = self.by_key[key]
        d = self.src / tag / r.arch
        d.mkdir(parents=True, exist_ok=True)
        return make_image(d / r.original, r, f'{tag}-{key}', **kw)

    def main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = dev_swap.main([*argv, '--lab-base', str(self.base), '--work', str(self.root / 'work')], lab=self.lab)
        text = out.getvalue()
        self.last = text
        return code, text

    def verdict(self, text):
        return [l for l in text.splitlines() if l.split(' ')[0] in ('PLAN', 'SWAP', 'STATUS', 'RESTORE')][-1]

    def assert_installed(self):
        now = {k: sha(p) for k, p in self.paths().items()}
        self.assertEqual(now, self.installed)

    # ---- the tests ----
    def test_the_role_table_takes_the_driverstore_names(self):
        self.assertEqual(self.by_key['x86:d3d11-icd'].name, 'amdgpu_wddm_radv_d3d1132.dll')
        self.assertEqual(self.by_key['x86:d3d11-record'].name, 'amdgpu_wddm_d3d1132.config')
        self.assertEqual(self.by_key['x64:d3d12-icd'].name, 'amdgpu_wddm_radv_d3d12.dll')
        self.assertEqual(self.by_key['x86:dxvk'].original, 'amdgpu_wddm_dxvk.dll')
        self.assertEqual(len({r.name.lower() for r in self.table}), len(self.table))

    def test_the_lab_script_parses_in_powershell_51(self):
        cmd = ("$e=$null;$t=$null;[void][Management.Automation.Language.Parser]::ParseFile('%s',[ref]$t,[ref]$e);"
               "$e|%%{$_.Extent.StartLineNumber.ToString()+' '+$_.Message};'errors '+$e.Count") % dev_swap.LAB_SCRIPT
        done = subprocess.run([str(PS51), '-NoProfile', '-Command', cmd], capture_output=True, text=True, timeout=120)
        self.assertIn('errors 0', done.stdout, done.stdout + done.stderr)

    def test_status_reads_the_package_and_the_lab_digest_is_the_image_identity(self):
        code, text = self.main('status')
        self.assertEqual(code, 0, text)
        self.assertIn('STATUS PASS every role as installed', text)
        self.assertIn('matches the active engine and ICD', text)
        self.assertNotIn('DOES NOT MATCH', text)
        _, out, _ = self.lab.run(['-Action', 'status', '-RolesB64', dev_swap.roles_b64(self.table), '-Base', str(self.base)], 120)
        facts = json.loads(next(l for l in out.splitlines() if l.startswith('DEVSWAP-JSON '))[13:])
        eng = self.paths()['x86:dxvk']
        self.assertEqual(facts['roles']['x86:dxvk']['digest'], image_digest(eng))
        self.assertEqual(facts['package'], PACKAGE)

    def test_swap_of_the_engine_pair_writes_both_records_and_restore_proves_the_originals(self):
        e64, e86 = self.new('x64:dxvk', 'int5'), self.new('x86:dxvk', 'int5')
        code, text = self.main('swap', '--file', f'x64:dxvk={e64}', '--file', f'x86:dxvk={e86}', '--package', PACKAGE)
        self.assertEqual(code, 0, text)
        self.assertIn('SWAP PASS written 4', text)
        p = self.paths()
        for arch, eng in (('x64', e64), ('x86', e86)):
            self.assertEqual(sha(p[f'{arch}:dxvk']), sha(eng))
            rec = p[f'{arch}:d3d11-record'].read_bytes()
            self.assertEqual(rec[68:100].hex().upper(), image_digest(eng))
            self.assertEqual(rec[100:].hex().upper(), image_digest(p[f'{arch}:d3d11-icd']))
            self.assertEqual(rec[:68], record('00' * 32, '00' * 32)[:68])
        code, text = self.main('status')
        self.assertIn('STATUS PASS 4 roles swapped', text)
        self.assertNotIn('DOES NOT MATCH', text)
        code, text = self.main('restore')
        self.assertEqual(code, 0, text)
        self.assertIn('RESTORE PASS restored 4, proved 4 of 4 by SHA-256, state deleted', text)
        self.assert_installed()
        code, text = self.main('restore')
        self.assertIn('RESTORE PASS nothing is swapped, nothing to restore', text)

    def test_a_second_swap_on_top_is_one_step_and_restore_returns_the_first_originals(self):
        e1 = self.new('x64:dxvk', 'int5')
        self.assertEqual(self.main('swap', '--file', f'x64:dxvk={e1}')[0], 0, self.last)
        s11, e2 = self.new('x64:d3d11-shell', 'rc1'), self.new('x64:dxvk', 'rc1')
        s12, s12w = self.new('x64:d3d12-shell', 'rc1'), self.new('x86:d3d12-shell', 'rc1')
        code, text = self.main('swap', '--file', f'x64:d3d11={s11}', '--file', f'x64:dxvk={e2}',
                               '--file', f'x64:d3d12={s12}', '--file', f'x86:d3d12={s12w}')
        self.assertEqual(code, 0, text)
        p = self.paths()
        self.assertEqual(p['x64:d3d11-record'].read_bytes()[68:100].hex().upper(), image_digest(e2))
        self.assertEqual(sha(p['x86:d3d12-shell']), sha(s12w))
        state = json.loads((self.base / 'state' / 'state.json').read_text(encoding='utf-8'))
        self.assertEqual(state['roles']['x64:dxvk']['sha256'], self.installed['x64:dxvk'])
        self.assertEqual(sorted(state['roles']), sorted(['x64:dxvk', 'x64:d3d11-record', 'x64:d3d11-shell',
                                                         'x64:d3d12-shell', 'x86:d3d12-shell']))
        self.assertEqual(self.main('restore')[0], 0, self.last)
        self.assert_installed()

    def test_the_icd_alias_swaps_both_icd_lanes_and_repins_the_record(self):
        icd = self.new('x64:d3d11-icd', 'icd')
        code, text = self.main('swap', '--file', f'x64:icd={icd}')
        self.assertEqual(code, 0, text)
        p = self.paths()
        self.assertEqual(sha(p['x64:d3d11-icd']), sha(icd))
        self.assertEqual(sha(p['x64:d3d12-icd']), sha(icd))
        self.assertEqual(p['x64:d3d11-record'].read_bytes()[100:].hex().upper(), image_digest(icd))

    def test_a_held_file_is_renamed_aside(self):
        self.facts(PACKAGE, held=['amdgpu_wddm_dxvk.dll'])
        e = self.new('x64:dxvk', 'held')
        code, text = self.main('swap', '--file', f'x64:dxvk={e}')
        self.assertEqual(code, 0, text)
        self.assertIn('renamed the loaded copy aside', text)
        aside = list(self.paths()['x64:dxvk'].parent.glob('amdgpu_wddm_dxvk.dll.swap-aside-*'))
        self.assertEqual(len(aside), 1)
        self.assertEqual(sha(aside[0]), self.installed['x64:dxvk'])
        self.assertEqual(self.main('restore')[0], 0, self.last)
        self.assert_installed()

    def test_plan_makes_no_lab_call(self):
        e = self.new('x64:dxvk', 'p')
        code, text = self.main('plan', '--file', f'x64:dxvk={e}')
        self.assertEqual(code, 0, text)
        self.assertIn('x64:d3d11-record', text)
        self.assertIn('ABI: neither', text)
        self.assertEqual(self.lab.calls, [])

    # ---- refusals: one line, nothing written ----
    def refused(self, *argv, says):
        code, text = self.main(*argv)
        self.assertEqual(code, 2, text)
        self.assertIn('REFUSED', self.verdict(text))
        self.assertIn(says, self.verdict(text))
        self.assert_installed()

    def test_refusals_of_the_input_files(self):
        e64 = self.new('x64:dxvk', 'a')
        self.refused('plan', '--file', f'x86:dxvk={e64}', says='the file is x64, the role is x86')
        self.refused('swap', '--file', f'x64:vkd3d={e64}', says='wrong role, it exports the entry point of dxvk')
        router = self.new('x64:d3d11-shell', 'r', original='bc250d3d_router.dll')
        self.refused('swap', '--file', f'x64:d3d11={router}', says='its OriginalFilename is bc250d3d_router.dll')
        text = self.src / 'not-a-pe.dll'
        text.write_bytes(b'hello')
        self.refused('swap', '--file', f'x64:dxvk={text}', says='not a PE image')
        self.refused('swap', '--file', f'x64:d3d11-record={e64}', says='never an input')
        self.assertEqual(self.lab.calls, [])

    def test_an_image_without_a_version_resource_is_named_in_the_plan(self):
        e = self.new('x64:dxvk', 'nv', original=False)
        code, text = self.main('plan', '--file', f'x64:dxvk={e}')
        self.assertEqual(code, 0, text)
        self.assertIn('no version resource: identified by its export only', text)

    def test_refusals_of_the_lab_state(self):
        e = self.new('x64:dxvk', 'g')
        self.facts(PACKAGE, guard='Running')
        self.refused('swap', '--file', f'x64:dxvk={e}', says='guard service runs')
        self.facts(PACKAGE, trip='cpu-temperature')
        self.refused('swap', '--file', f'x64:dxvk={e}', says='guard has a trip (cpu-temperature)')
        self.facts(PACKAGE)
        self.refused('swap', '--file', f'x64:dxvk={e}', '--package', OTHER, says=f'not the expected {OTHER}')

    def test_the_positive_control_refuses_a_record_that_does_not_pin_the_installed_engine(self):
        p = self.paths()
        rec = p['x64:d3d11-record']
        rec.write_bytes(record('AB' * 32, rec.read_bytes()[100:].hex()))
        self.installed = {k: sha(v) for k, v in p.items()}
        e = self.new('x64:dxvk', 'pc')
        self.refused('swap', '--file', f'x64:dxvk={e}', says='(positive control)')
        code, text = self.main('status')
        self.assertIn('DOES NOT MATCH', text)

    def test_the_lab_refuses_a_record_that_does_not_pin_the_new_engine(self):
        # The host is bypassed: a manifest with the installed record and a new engine reaches the lab script.
        e = self.new('x64:dxvk', 'bad')
        stage = self.base / 'in-test'
        stage.mkdir(parents=True)
        shutil.copyfile(e, stage / 'amdgpu_wddm_dxvk.dll')
        shutil.copyfile(self.paths()['x64:d3d11-record'], stage / 'amdgpu_wddm_d3d11.config')
        (stage / 'manifest.json').write_text(json.dumps({'package': PACKAGE, 'files': [
            {'key': 'x64:dxvk', 'file': 'amdgpu_wddm_dxvk.dll', 'sha256': sha(e)},
            {'key': 'x64:d3d11-record', 'file': 'amdgpu_wddm_d3d11.config', 'sha256': sha(stage / 'amdgpu_wddm_d3d11.config')}]}),
            encoding='utf-8')
        code, out, _ = self.lab.run(['-Action', 'swap', '-RolesB64', dev_swap.roles_b64(self.table), '-Base',
                                     str(self.base), '-InDir', str(stage)], 120)
        self.assertEqual(code, 2, out)
        self.assertIn('the files after the swap are', out.splitlines()[-1])
        # And an engine without any new record.
        (stage / 'manifest.json').write_text(json.dumps({'package': PACKAGE, 'files': [
            {'key': 'x64:dxvk', 'file': 'amdgpu_wddm_dxvk.dll', 'sha256': sha(e)}]}), encoding='utf-8')
        code, out, _ = self.lab.run(['-Action', 'swap', '-RolesB64', dev_swap.roles_b64(self.table), '-Base',
                                     str(self.base), '-InDir', str(stage)], 120)
        self.assertIn('carries no new record', out.splitlines()[-1])
        # And a staged file whose hash is not the one the host sent.
        (stage / 'manifest.json').write_text(json.dumps({'package': PACKAGE, 'files': [
            {'key': 'x64:d3d12-shell', 'file': 'amdgpu_wddm_dxvk.dll', 'sha256': '00' * 32}]}), encoding='utf-8')
        code, out, _ = self.lab.run(['-Action', 'swap', '-RolesB64', dev_swap.roles_b64(self.table), '-Base',
                                     str(self.base), '-InDir', str(stage)], 120)
        self.assertIn('the host sent', out.splitlines()[-1])
        self.assert_installed()
        self.assertFalse((self.base / 'state').exists())

    def test_a_state_of_another_package_is_stale(self):
        e = self.new('x64:dxvk', 's')
        self.assertEqual(self.main('swap', '--file', f'x64:dxvk={e}')[0], 0, self.last)
        self.pkg = self.make_package(OTHER, 'other')
        self.installed = {k: sha(p) for k, p in self.paths().items()}
        self.facts(OTHER)
        self.refused('swap', '--file', f'x64:dxvk={e}', says='the lab state belongs to package')
        code, text = self.main('restore')
        self.assertEqual(code, 2, text)
        self.assertIn('restore -DropStale deletes it', text)
        code, text = self.main('restore', '--drop-stale')
        self.assertEqual(code, 0, text)
        self.assertIn(f'dropped the stale state of package {PACKAGE}', text)
        self.assertFalse((self.base / 'state').exists())

    def test_output_masks_lab_addresses(self):
        self.assertEqual(dev_swap.mask('ssh 10.1.2.3 refused'), 'ssh <ip> refused')


if __name__ == '__main__':
    unittest.main()
