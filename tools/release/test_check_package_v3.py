"""Host tests of check_package_v3.py (BD-166): a staged package built here, no device and no machine state.

Each control has its negative twin: the stamped pair of both architectures passes, and a missing x86 record,
swapped pins, an edited header, an edited capability, changed image bytes, a record the member set does not
name and a lab payload under a release check all fail. One control runs the other way round: an engine with
an added certificate keeps the same image identity, so the record stays valid although the file bytes change.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


check = _load('check_package_v3', HERE / 'check_package_v3.py')
driver_package = _load('driver_package', HERE / 'driver_package.py')
# The PE images come from the version-stamp tests, which build them byte by byte (tools/build).
images = _load('test_versionstamp', HERE.parent / 'build' / 'test_versionstamp.py')

LANES = check.LANES
MEMBER = {'payload/d3d11/amdgpu_wddm_d3d11.config': 'payload/kmd/d3d11/amdgpu_wddm_d3d11.config',
          'payload/d3d11/amdgpu_wddm_dxvk.dll': 'payload/kmd/d3d11/amdgpu_wddm_dxvk.dll',
          'payload/d3d11/amdgpu_wddm_radv.dll': 'payload/kmd/d3d11/amdgpu_wddm_radv_d3d11.dll',
          'payload/wow64/d3d11/amdgpu_wddm_d3d11.config': 'payload/kmd/wow64/d3d11/amdgpu_wddm_d3d1132.config',
          'payload/wow64/d3d11/amdgpu_wddm_dxvk.dll': 'payload/kmd/wow64/d3d11/amdgpu_wddm_dxvk32.dll',
          'payload/wow64/d3d11/amdgpu_wddm_radv.dll': 'payload/kmd/wow64/d3d11/amdgpu_wddm_radv_d3d1132.dll'}
CAPS = tuple(range(1, 14))      # the thirteen capability DWORDs, each a value of its own


def pe(tag, cert=False):
    """A distinct PE image per role, so no two members can be confused."""
    return images.make_pe(sections=((b'.text', b'\xc3' * 8 + tag.encode('ascii').ljust(24, b'\0'), 0x60000020),),
                          cert=cert)


def config_v2(engine: bytes, icd: bytes, caps=CAPS) -> bytes:
    """The lane input as write-umd-config.py leaves it: version 2, full-file pins."""
    return struct.pack('<17I32s32s', check.MAGIC, 2, 132, 0, *caps,
                       hashlib.sha256(engine).digest(), hashlib.sha256(icd).digest())


@unittest.skipUnless(os.name == 'nt', 'the record pins the Windows catalog image identity')
class ConfigRecordTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.workspace = self.root / 'workspace'
        self.package = self.root / 'package' / 'files'
        self.sources = self.build()

    def build(self, flavour_of_package='release', caps=CAPS):
        """A workspace of pinned lane inputs and a package staged from one flavour of them."""
        rows = []
        for tag in ('release', 'lab'):
            for arch, cfg_slot, engine_slot, icd_slot in LANES:
                engine, icd = pe(f'{tag} {arch} engine'), pe(f'{tag} {arch} icd')
                content = {engine_slot: engine, icd_slot: icd, cfg_slot: config_v2(engine, icd, caps)}
                for slot, data in content.items():
                    source = f'build/{tag}/{arch}/' + slot.rsplit('/', 1)[1]
                    path = self.workspace / source
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    rows.append({'path': slot, 'flavour': tag, 'source': source,
                                 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest().upper()})
                if tag == flavour_of_package:
                    self.stage(arch, cfg_slot, engine_slot, icd_slot, tag)
        return {'schema': 4, 'files': rows}

    def stage(self, arch, cfg_slot, engine_slot, icd_slot, tag):
        """What driver_package.stage does to one lane: copy the images, convert the record to v3."""
        paths = {}
        for slot in (engine_slot, icd_slot):
            source = self.workspace / 'build' / tag / arch / slot.rsplit('/', 1)[1]
            target = self.package / MEMBER[slot]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            paths[slot] = target
        original = (self.workspace / 'build' / tag / arch / cfg_slot.rsplit('/', 1)[1]).read_bytes()
        record = driver_package.signing_safe_config(original, paths[engine_slot], paths[icd_slot])
        target = self.package / MEMBER[cfg_slot]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(record)

    def member(self, slot) -> Path:
        return self.package / MEMBER[slot]

    def check(self, flavour='release', workspace=True):
        return check.problems(self.package, self.sources, flavour,
                              self.workspace if workspace else None, log=lambda *a, **k: None)

    # ----------------------------------------------------------------- the positive path
    def test_both_architectures_pass(self):
        self.assertEqual(self.check(), [])
        self.assertEqual(self.check(workspace=False), [])

    def test_a_certificate_does_not_change_the_verdict(self):
        """The pin is image identity: a signature changes the file bytes and not the record's verdict."""
        target = self.member('payload/d3d11/amdgpu_wddm_dxvk.dll')
        before = target.read_bytes()
        target.write_bytes(pe('release x64 engine', cert=True))
        self.assertNotEqual(hashlib.sha256(target.read_bytes()).digest(), hashlib.sha256(before).digest())
        self.assertEqual(self.check(workspace=False), [])

    def test_the_lab_package_passes_its_own_flavour(self):
        shutil.rmtree(self.package)
        self.sources = self.build(flavour_of_package='lab')
        self.assertEqual(self.check(flavour='lab'), [])

    # ----------------------------------------------------------------- the refusals
    def test_missing_config32_fails(self):
        self.member('payload/wow64/d3d11/amdgpu_wddm_d3d11.config').unlink()
        found = self.check()
        self.assertEqual(len(found), 1, found)
        self.assertIn('amdgpu_wddm_d3d1132.config', found[0])
        self.assertIn('x86', found[0])

    def test_a_missing_sibling_fails(self):
        self.member('payload/d3d11/amdgpu_wddm_radv.dll').unlink()
        found = self.check()
        self.assertTrue(any('amdgpu_wddm_radv_d3d11.dll' in line for line in found), found)

    def test_swapped_pins_fail(self):
        target = self.member('payload/d3d11/amdgpu_wddm_d3d11.config')
        values = list(struct.unpack(check.LAYOUT, target.read_bytes()))
        values[check.ENGINE], values[check.ICD] = values[check.ICD], values[check.ENGINE]
        target.write_bytes(struct.pack(check.LAYOUT, *values))
        found = self.check(workspace=False)
        self.assertEqual(len(found), 2, found)
        self.assertTrue(any('engine' in line for line in found) and any('ICD' in line for line in found), found)

    def test_header_edits_fail(self):
        target = self.member('payload/d3d11/amdgpu_wddm_d3d11.config')
        good = target.read_bytes()
        for index, value, word in ((0, 0x4334314c, 'magic'), (1, 2, 'version'), (2, 108, 'size'),
                                   (3, 1, 'reserved')):
            values = list(struct.unpack(check.LAYOUT, good))
            values[index] = value
            target.write_bytes(struct.pack(check.LAYOUT, *values))
            found = self.check(workspace=False)
            with self.subTest(field=word):
                self.assertEqual(len(found), 1, found)
                self.assertIn(word, found[0])
        target.write_bytes(good[:-1])
        found = self.check(workspace=False)
        self.assertIn('131 bytes', found[0])

    def test_a_capability_edit_fails_against_the_pinned_lane_inputs(self):
        target = self.member('payload/d3d11/amdgpu_wddm_d3d11.config')
        values = list(struct.unpack(check.LAYOUT, target.read_bytes()))
        values[6] += 1                                    # 'compute', the third capability DWORD
        target.write_bytes(struct.pack(check.LAYOUT, *values))
        # The pins still match, so only the rebuild from the pinned lane inputs can see this.
        self.assertEqual(self.check(workspace=False), [])
        found = self.check()
        self.assertEqual(len(found), 1, found)
        self.assertIn('compute', found[0])

    def test_changed_image_bytes_fail(self):
        target = self.member('payload/d3d11/amdgpu_wddm_dxvk.dll')
        data = bytearray(target.read_bytes())
        data[0x400] = (data[0x400] + 1) & 0xFF            # the first byte of .text
        target.write_bytes(bytes(data))
        found = self.check(workspace=False)
        self.assertTrue(any('engine' in line and 'image identity' in line for line in found), found)

    def test_an_unclaimed_record_fails(self):
        stray = self.package / 'payload' / 'd3d11' / 'amdgpu_wddm_d3d11.config'
        stray.parent.mkdir(parents=True, exist_ok=True)
        stray.write_bytes(self.member('payload/d3d11/amdgpu_wddm_d3d11.config').read_bytes())
        found = self.check(workspace=False)
        self.assertEqual(len(found), 1, found)
        self.assertIn('does not name', found[0])

    def test_a_lab_payload_does_not_pass_a_release_check(self):
        shutil.rmtree(self.package)
        self.sources = self.build(flavour_of_package='lab')
        found = self.check(flavour='release')
        self.assertTrue(found, 'a lab payload passed the release check')
        self.assertEqual(self.check(flavour='lab'), [])

    def test_a_workspace_input_that_lost_its_pin_fails(self):
        source = self.workspace / 'build' / 'release' / 'x64' / 'amdgpu_wddm_dxvk.dll'
        source.write_bytes(source.read_bytes() + b'\0')
        found = self.check()
        self.assertTrue(any('release-sources.json pins' in line for line in found), found)

    def test_an_unknown_flavour_is_refused(self):
        with self.assertRaises(ValueError):
            self.check(flavour='debug')


if __name__ == '__main__':
    unittest.main()
