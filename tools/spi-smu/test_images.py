"""Optional real-image integration tests. Set SPI_SMU_TEST_IMAGE explicitly."""
import os
import unittest
from pathlib import Path
from offline import inspect
from plan_patch import plan


@unittest.skipUnless(os.environ.get('SPI_SMU_TEST_IMAGE'), 'set SPI_SMU_TEST_IMAGE')
class ImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = os.environ['SPI_SMU_TEST_IMAGE']
        cls.report = inspect(cls.path)
        cls.image = Path(cls.path).read_bytes()

    def recipe(self):
        pos = self.report['smu'][0]['body_range'][0]
        return {'stage': 'S2', 'entry_type': self.report['smu'][0]['type'],
                'stock_sha256': self.report['sha256'], 'edits': [
                    {'offset': pos, 'before': self.image[pos:pos+1].hex(),
                     'after': bytes([self.image[pos] ^ 1]).hex()}]}

    def test_controls(self):
        self.assertTrue(self.report['s1']['equal'])
        self.assertTrue(self.report['s1']['reparse_equal'])
        self.assertEqual(self.report['s1']['sha256'], self.report['sha256'])
        self.assertEqual(len(self.report['s1']['components']), 2)
        self.assertFalse(self.report['s1']['all_firmware_containers_rebuilt'])
        for entry in self.report['smu']:
            self.assertTrue(entry['stock_signature_valid'])
            self.assertEqual(len(entry['negative_controls']), 4)
            self.assertEqual(entry['negative_controls'][0]['offset'], entry['body_range'][0] - 1)
            self.assertTrue(all(c['rejected'] for c in entry['negative_controls']))

    def test_smu_directory_extents(self):
        raw = [e for d in self.report['directories'] for e in d['raw_entries']]
        for smu in self.report['smu']:
            entries = [e for e in raw if e['type'] == smu['type'] and
                       e['normalized_offset'] == smu['range'][0]]
            self.assertTrue(entries)
            for entry in entries:
                self.assertEqual(entry['coverage_status'], 'complete')
                self.assertEqual(entry['declared_range'], smu['range'])
                self.assertEqual(entry['parsed_range'], smu['range'])

    def test_installed_p3_missing_loader_and_partial_type44(self):
        if self.report['sha256'] != '09fb260f498bbce5d2e67729d9b6d35d4a75ee833be52f32d22bc84cd2693103':
            self.skipTest('exact installed P3 fixture only')
        directory = next(d for d in self.report['directories'] if d['offset'] == 0x8e0000)
        self.assertEqual(len(directory['entries']), 18)
        self.assertEqual(len(directory['raw_entries']), 19)
        loader = next(e for e in directory['raw_entries'] if e['type'] == 1)
        self.assertEqual(loader['coverage_status'], 'missing')
        self.assertEqual(loader['raw_location'], 0xff8e0400)
        self.assertEqual(loader['declared_range'], [0x8e0400, 0x8eac00])
        other = next(e for e in directory['raw_entries'] if e['type'] == 0x44)
        self.assertEqual(other['coverage_status'], 'partial')
        self.assertEqual(other['declared_size'], 0x6610)
        self.assertEqual(other['parsed_range'], [0x9c3d00, 0x9c3f10])
        self.assertEqual(other['unparsed_ranges'], [[0x9c3f10, 0x9ca310]])

    def test_recipe(self):
        result = plan(self.path, self.recipe())
        self.assertEqual(result['image'], str(Path(self.path).resolve()))
        self.assertTrue(result['signature_preserved'])
        self.assertFalse(result['candidate_signature_valid'])
        self.assertFalse(result['release_ready'])
        self.assertEqual(len(result['diff_ranges']), 1)
        self.assertEqual(Path(self.path).read_bytes(), self.image)

    def test_wrong_stock(self):
        recipe = self.recipe()
        recipe['stock_sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            plan(self.path, recipe)

    def test_truncated_image(self):
        with self.assertRaises((ValueError, AssertionError)):
            inspect(self.image[:4096])

