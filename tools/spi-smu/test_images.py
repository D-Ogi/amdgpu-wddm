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
        for entry in self.report['smu']:
            self.assertTrue(entry['stock_signature_valid'])
            self.assertEqual(len(entry['negative_controls']), 4)
            self.assertEqual(entry['negative_controls'][0]['offset'], entry['body_range'][0] - 1)
            self.assertTrue(all(c['rejected'] for c in entry['negative_controls']))

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

