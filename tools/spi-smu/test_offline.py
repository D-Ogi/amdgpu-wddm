import unittest
from offline import digest, diff_ranges, patch, rebuild


class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.image = bytes(range(32))
        self.entry = {'body_range': [8, 24]}
        self.edit = {'offset': 10, 'before': '0a', 'after': 'ff'}

    def apply(self, edits=None, identity=None):
        return patch(self.image, self.entry, edits or [self.edit], identity or digest(self.image))

    def test_patch_preserves_surroundings(self):
        result = self.apply()
        self.assertEqual(diff_ranges(self.image, result), [[10, 11]])
        self.assertEqual(self.image[10], 10)

    def test_identity(self):
        with self.assertRaises(ValueError):
            self.apply(identity='0' * 64)

    def test_expected_bytes(self):
        with self.assertRaises(ValueError):
            self.apply([{**self.edit, 'before': '00'}])

    def test_boundary_and_signature_exclusion(self):
        for offset in (-1, 7, 24, 32):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.apply([{**self.edit, 'offset': offset}])

    def test_size_change_and_noop(self):
        for after in ('', 'ffff', '0a'):
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.apply([{**self.edit, 'after': after}])

    def test_overlap(self):
        with self.assertRaises(ValueError):
            self.apply([self.edit, self.edit])

    def test_rebuild_preserves_gaps(self):
        self.assertEqual(rebuild(self.image, [(8, 12), (16, 24)]), self.image)

    def test_rebuild_rejects_invalid_ranges(self):
        for spans in ([(8, 12), (11, 24)], [(-1, 3)], [(4, 33)], [(4, 4)]):
            with self.subTest(spans=spans), self.assertRaises(ValueError):
                rebuild(self.image, spans)

    def test_diff_end_and_separated_ranges(self):
        self.assertEqual(diff_ranges(b'abcde', b'xbcdy'), [[0, 1], [4, 5]])
        with self.assertRaises(ValueError):
            diff_ranges(b'a', b'ab')


if __name__ == '__main__':
    unittest.main()
