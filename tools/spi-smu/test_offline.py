import unittest
from offline import digest, diff_ranges, patch, rebuild, rebuild_parsed


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


class ParsedRebuildTests(unittest.TestCase):
    class Component:
        def __init__(self, offset, data):
            self.offset, self.data = offset, data

        def get_address(self):
            return self.offset

        def get_bytes(self):
            return self.data

        def __len__(self):
            return len(self.data)

    class Entry(Component):
        pass

    def setUp(self):
        self.image = b'gapHDRbodySIGtail'
        self.entry = self.Entry(3, b'HDRbodySIG')
        self.entry.type = 8
        self.entry.encrypted = self.entry.compressed = False
        self.entry.header = self.Component(3, b'HDR')
        self.entry.body = self.Component(6, b'body')
        self.entry.signature = self.Component(10, b'SIG')

    def test_round_trip_preserves_opaque_data(self):
        rebuilt, records = rebuild_parsed(self.image, [self.entry])
        self.assertEqual(rebuilt, self.image)
        self.assertEqual([c['name'] for c in records[0]['components']], ['header', 'body', 'signature'])

    def test_extraction_corruption_is_not_hidden_by_original_slices(self):
        self.entry.body.data = b'Body'
        with self.assertRaisesRegex(ValueError, 'differs'):
            rebuild_parsed(self.image, [self.entry])

    def test_component_gap_and_overlap(self):
        for offset in (5, 7):
            self.entry.body.offset = offset
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'non-contiguous'):
                rebuild_parsed(self.image, [self.entry])

    def test_missing_tail(self):
        self.entry.signature.data = b'SI'
        with self.assertRaisesRegex(ValueError, 'cover'):
            rebuild_parsed(self.image, [self.entry])

    def test_duplicate_entries_and_empty_set(self):
        for entries in ([self.entry, self.entry], []):
            with self.subTest(count=len(entries)), self.assertRaises(ValueError):
                rebuild_parsed(self.image, entries)

    def test_encoded_entry_refused(self):
        for flag in ('encrypted', 'compressed'):
            setattr(self.entry, flag, True)
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, 'encoded'):
                rebuild_parsed(self.image, [self.entry])
            setattr(self.entry, flag, False)


if __name__ == '__main__':
    unittest.main()
