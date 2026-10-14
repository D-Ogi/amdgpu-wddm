"""Host tests of the record side of versionstamp.check_release (BD-166): which records a release must carry.

The public gate decides from release-sources.json's own source mapping, not from what the package happens to
hold, so a release that lost its x86 record, or all of its records, or whose record has a corrupt header
fails here. The pre-stage controls of check_package_v3 cover the same ground one layer down; these run
through check_release, which is what the release script calls.

A release-sources.json that does not pin the lane inputs at all describes no such package: the legacy pins of
a release before the two flavours, and the fixtures of the other host tests. That file is checked under the
names the package carries, and an invalid selection of a file that does pin them propagates instead of
quietly turning into that mode.

No payload file, no device, no machine state: the images are built byte by byte by test_versionstamp.
"""
import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vs = _load('versionstamp', HERE / 'versionstamp.py')
images = _load('test_versionstamp', HERE / 'test_versionstamp.py')
VERSION, TABLE = images.VERSION, images.TABLE
LANES = (('x64', 'payload/d3d11'), ('x86', 'payload/wow64/d3d11'))


def pe(tag):
    return images.make_pe(sections=((b'.text', b'\xc3' * 8 + tag.encode('ascii').ljust(24, b'\0'), 0x60000020),))


class RecordGateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.top = self.root / 'release'
        self.package = self.top / 'files'
        self.build()

    def build(self, version=2):
        """A pre-stage release whose release-sources.json pins its six lane inputs."""
        rows = []
        for arch, lane in LANES:
            engine, icd = pe(f'{arch} engine'), pe(f'{arch} icd')
            caps = tuple(range(1, 14)) if version == 2 else tuple(range(1, 8))
            record = struct.pack('<%dI32s32s' % (4 + len(caps)), 0x4334314D, version,
                                 132 if version == 2 else 108, 0, *caps,
                                 hashlib.sha256(engine).digest(), hashlib.sha256(icd).digest())
            for name, data in (('amdgpu_wddm_dxvk.dll', engine), ('amdgpu_wddm_radv.dll', icd),
                               (vs.M14C_NAME, record)):
                path = self.package / lane / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                rows.append({'path': f'{lane}/{name}', 'flavour': 'release',
                             'source': f'build/{arch}/{name}', 'size': len(data),
                             'sha256': hashlib.sha256(data).hexdigest().upper()})
        kmd = self.package / 'payload' / 'kmd'
        kmd.mkdir(parents=True, exist_ok=True)
        (kmd / 'driver.sys').write_bytes(images.make_pe())
        self.inf = kmd / 'bc250kmd.inf'
        self.inf.write_text('[Version]\nProvider = %Provider%\n\n[Strings]\nProvider    = "amdgpu-wddm"\n',
                            encoding='ascii')
        (self.top / 'setup.exe').write_bytes(images.make_pe())
        self.sources = self.root / 'release-sources.json'
        self.sources.write_text(json.dumps({'schema': 4, 'files': rows}), encoding='utf-8')
        self.table = dict(TABLE, files=dict(TABLE['files'], **{'setup.exe': 'test setup'}))

    def records(self, flavour='release'):
        """Only the problems about the configuration records; the version resources are another gate."""
        found = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=lambda *_: None,
                                 flavour=flavour)
        return [line for line in found if 'config' in line or 'record' in line]

    def test_a_complete_prestage_release_has_no_record_problem(self):
        self.assertEqual(self.records(), [])

    def test_a_v1_pair_has_no_record_problem(self):
        self.build(version=1)
        self.assertEqual(self.records(), [])

    def test_a_missing_x86_record_fails(self):
        (self.package / 'payload/wow64/d3d11' / vs.M14C_NAME).unlink()
        found = self.records()
        self.assertTrue(any('x86: the package has no' in line for line in found), found)

    def test_a_release_without_any_record_fails(self):
        for _, lane in LANES:
            (self.package / lane / vs.M14C_NAME).unlink()
        self.assertEqual(len(self.records()), 2, self.records())

    def test_a_corrupt_header_fails(self):
        target = self.package / 'payload/d3d11' / vs.M14C_NAME
        data = bytearray(target.read_bytes())
        data[0] ^= 0xFF
        target.write_bytes(bytes(data))
        found = self.records()
        self.assertTrue(any('magic' in line for line in found), found)

    def test_a_changed_engine_fails(self):
        target = self.package / 'payload/d3d11/amdgpu_wddm_dxvk.dll'
        target.write_bytes(target.read_bytes() + b'\0')
        found = self.records()
        self.assertTrue(any('pins the engine' in line for line in found), found)

    def test_a_flavour_the_manifest_does_not_describe_is_refused(self):
        """A manifest that pins the lane inputs is a release manifest: an invalid selection of it must not
        become the mode that checks a package under the names it carries."""
        data = json.loads(self.sources.read_text(encoding='utf-8'))
        data['flavour'] = 'release'
        self.sources.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaises(ValueError):
            vs.member_sources(self.sources, 'lab')

    # ------------------------------------------------- "built" is a table, and it refuses what it does not name
    def built_table(self, **extra):
        """The release's own shape of the table: every shipped PE named under 'built' as well."""
        names = dict.fromkeys(self.table['files'], 'the test build writes its resource')
        names.update(extra)
        return dict(self.table, built=names)

    def test_a_shipped_pe_that_the_built_table_does_not_name_is_a_package_error(self):
        """A slot added to the package with no build step behind it: the table claims every file it
        describes, so the new one is accused by name instead of being measured against nothing."""
        stray = self.package / 'payload' / 'd3d11' / 'amdgpu_wddm_extra.dll'
        stray.write_bytes(images.make_pe())
        found = vs.check_release(self.top, VERSION, self.sources, self.inf, self.built_table(),
                                 log=lambda *_: None)
        self.assertTrue(any('amdgpu_wddm_extra.dll: nothing stamps it' in line for line in found), found)

    def test_a_file_the_built_table_names_is_checked_like_a_stamped_file(self):
        """Not an exemption: the file still has to carry the release's resource."""
        table = self.built_table()
        found = vs.check_release(self.top, VERSION, self.sources, self.inf, table, log=lambda *_: None)
        self.assertTrue(any('amdgpu_wddm_dxvk.dll' in line and 'no version resource' in line for line in found),
                        found)

    def test_an_empty_built_table_refuses_nothing(self):
        """A table that declares no stamping cannot accuse a file: the host-test fixtures stay as they are."""
        found = vs.check_release(self.top, VERSION, self.sources, self.inf, self.table, log=lambda *_: None)
        self.assertFalse(any('nothing stamps it' in line for line in found), found)

    def test_the_release_table_has_to_claim_every_file_it_describes(self):
        """Where the completeness lives: leaving a file out of 'built' in the release's own table would turn
        the refusal off instead of failing, so the table is refused when it is read."""
        path = self.root / 'versioninfo.json'
        table = {'schema': 1, 'files': {'a.dll': 'the first', 'b.dll': 'the second'},
                 'built': {'a.dll': 'its own build writes it'}, 'exempt': {}}
        path.write_text(json.dumps(table), encoding='utf-8')
        with self.assertRaises(vs.StampError) as caught:
            vs.load_table(path)
        self.assertIn("nothing in 'built' writes b.dll", str(caught.exception))
        table['exempt']['b.dll'] = 'a reason for having no resource'
        path.write_text(json.dumps(table), encoding='utf-8')
        self.assertEqual(set(vs.load_table(path)['files']), {'a.dll', 'b.dll'})

    def test_a_partial_table_accuses_nobody(self):
        """A host test's table names one built file and no more: it does not claim to know what stamps the
        rest, so the gate checks resources and leaves the refusal alone."""
        stray = self.package / 'payload' / 'd3d11' / 'amdgpu_wddm_extra.dll'
        stray.write_bytes(images.make_pe())
        table = dict(self.table, built={'driver.sys': 'its own .rc in the test'},
                     files=dict(self.table['files'], **{'driver.sys': 'test driver'}))
        found = vs.check_release(self.top, VERSION, self.sources, self.inf, table, log=lambda *_: None)
        self.assertFalse(any('nothing stamps it' in line for line in found), found)

    def test_a_file_that_pins_nothing_of_the_member_set_maps_nothing(self):
        other = self.root / 'legacy-sources.json'
        other.write_text(json.dumps({'files': [{'path': 'payload/tools/other.exe', 'third_party': {}}]}),
                         encoding='utf-8')
        self.assertEqual(vs.member_sources(other, 'release'), {})


if __name__ == '__main__':
    unittest.main()
